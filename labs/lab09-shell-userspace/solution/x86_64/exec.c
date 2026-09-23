/* Lab9：ELF 加载器——把磁盘上的一个文件变成一个可以执行的用户地址空间。
 *
 * 这是本 Lab 的核心。Lab7 的 exec 是个名不副实的东西：它"重新映射内嵌的
 * 那一份 user_prog.bin"，参数里连文件名都没有，因为那时候还没有文件系统。
 * Lab8 有了文件系统但没动 exec。本文件补上真正的那一步——exec 接收一个
 * 路径，从磁盘读出 ELF，按 program header 的描述建立一个全新的地址空间，
 * 然后让这次系统调用"返回"到新程序的入口。做完这件事，"用户程序"才第一
 * 次成为一个独立于内核的东西：内核镜像里不再有任何用户代码，六个程序都
 * 躺在 fs.img 里，内核是在运行时解析它们的。
 *
 * ── 这个文件在两个架构下逐字节相同 ────────────────────────────────
 *
 * 跟 fs.c 一样，x86_64/ 和 riscv64/ 目录下各有一份完全相同的拷贝，由
 * `make check-shared-iface` 机械守卫。ELF 加载这件事几乎完全与架构无关:
 * 文件格式是 System V ABI 规定的，段的权限翻译成页表标志位走的是
 * pagetable.h 那层已经抹平架构差异的接口，"把字节读进新分配的物理页"
 * 用的是 fs_read + memcpy。整个文件只有一处 #if defined(__x86_64__),
 * 就在下面：内核偏移映射的基址，和本内核期望的 e_machine 取值。
 *
 * 两处都不是"逻辑不同"，是"同一个逻辑里的两个常量不同"——这正是把它们
 * 收进一个 #if 块、而不是把整个文件复制成两份各自演化的理由。
 *
 * ── 结构：先全部校验，再一次性提交 ────────────────────────────────
 *
 * exec 有一个让它比其它系统调用都难写的性质：它要*销毁*调用者的地址
 * 空间，然后重建。一旦开始销毁，就再也回不去了——旧的映射已经拆掉、
 * 物理页已经还给 kalloc，此时如果发现"这个 ELF 其实是坏的"，调用者
 * 已经没有一个能继续执行的地址空间可以返回了。
 *
 * 所以本文件严格分成两个阶段，中间有一条明确的提交线：
 *
 *   提交线之前：只读。查文件、读 ELF 头、校验每一个 program header、
 *     把 argv 字符串拷进内核缓冲区。任何一步不满意就 return 负数，
 *     调用者的地址空间一个字节都没动过，exec 干净地失败，用户程序
 *     能收到错误码继续跑。
 *
 *   提交线之后：不允许失败。拆旧映射、分配新页、读入段内容、建栈、
 *     压 argv。这一段里出现问题只能 panic——不是偷懒，是这个阶段真的
 *     没有正确的退路了（见 uvm_clear() 上方注释对"另一种做法"的讨论)。
 *
 * 这条线在代码里是一句注释加一个函数调用的位置关系，但它是 exec 最重要
 * 的设计约束。sh.c 直接依赖它：shell fork 出子进程后 exec 一个打错的
 * 命令名，子进程必须能从失败的 exec 里回来、打印 "command not found"、
 * 然后 exit(1)。如果 exec 的失败路径会毁掉地址空间，这件事就不可能做到,
 * shell 只要输错一个命令就会连带崩掉一个进程。
 *
 * ── 一个前提：exec 全程不会被切走 ─────────────────────────────────
 *
 * 下面的 argv 暂存缓冲区是 static 的（全局唯一一份），这只有在"同一时刻
 * 最多一个进程在执行 exec_load()"时才安全。本课程满足这个前提，而且理由
 * 在两个架构上是同一条：系统调用全程关中断。x86_64 是 IA32_FMASK 里
 * 清掉了 IF（syscall_init() 设的），riscv64 是硬件在进入 S 态 trap 时
 * 自动把 sstatus.SIE 清零。时钟中断进不来，也就没有任何机会 yield() 到
 * 另一个进程去调第二次 exec。
 *
 * 这个前提写在这里，是因为它是"沉默的"：哪天有人为了让内核可抢占而在
 * 系统调用里开中断，这几个 static 缓冲区会变成两个进程同时读写的共享
 * 状态，而症状会是"某个程序偶尔拿到别人的 argv"——极难追。真实内核里
 * 这类缓冲区放在 per-CPU 或者进程自己的内核栈上，本课程用 static 换取
 * 简单，代价是必须把前提写清楚。
 */
#include "types.h"
#include "proc.h"
#include "elf.h"
#include "fs.h"
#include "kalloc.h"
#include "pagetable.h"
#include "panic.h"
#include "console.h"
#include "string.h"

/* 本文件唯一的架构差异——见文件顶部说明。
 *
 * KERNEL_VIRT_BASE：kalloc_page() 返回物理地址，而本文件要往那些页里
 * memset/memcpy/fs_read，必须先加上这个偏移变成内核虚拟地址才能解引用
 * （pagetable_activate() 之后物理地址不再是合法的可解引用地址，这是
 * Lab4 起反复强调的那条规矩）。两个架构的内核都把全部物理内存按固定
 * 偏移映射到高端，只是基址不同。
 *
 * EXPECTED_EM：本内核只能执行本架构的指令。ELF 是跨架构的容器格式，
 * 一个 riscv64 的 ELF 在 x86_64 内核上每个字段都能正确解析，只有真正
 * 跳进去执行时才会炸——所以这个检查必须显式做，不做的话症状是"加载
 * 成功、一进用户态就是非法指令"，排查方向会完全错。 */
#if defined(__x86_64__)
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull
#define EXPECTED_EM      EM_X86_64
#else
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull
#define EXPECTED_EM      EM_RISCV
#endif

/* 用户地址空间布局。
 *
 *   0x400000 起：程序的各个段，具体放哪几页由 ELF 的 program header 决定
 *                （user.ld 把 ENTRY 定在 0x400000，代码段在 0x400000，
 *                 数据段在 0x401000）。
 *   USER_STACK_TOP 往下 USER_STACK_PAGES 页：用户栈。
 *
 * 栈顶选 0x800000（8MiB），跟程序段之间隔着 4MiB 的空洞。Lab7 的栈顶是
 * 0x402000，紧贴在程序页后面——那时候程序恒定只占一页，"紧贴"是安全的。
 * 现在程序占几页由 ELF 说了算，栈必须挪到一个"程序不可能长到那里"的地址
 * 去，否则某天有人的程序多了几百行、代码段涨到第三页，就会静默地把栈页
 * 覆盖掉（pagetable_map 对同一个虚拟地址映射两次不会报错，只是后者覆盖
 * 前者）。那个 bug 的症状是"程序刚进 main 就乱跳"，而原因在链接产物的
 * 大小上，中间隔了整整一层，非常难查。
 *
 * 空洞本身不占任何物理内存——没有映射的虚拟地址范围在页表里就是一堆
 * 空的表项，不需要为"留出 4MiB"付出任何代价。这是虚拟内存最直接的一个
 * 好处，也是为什么真实系统敢把栈放在地址空间极高处、把堆放在极低处,
 * 中间留几百 TB 空洞：地址空间是免费的，物理内存才要钱。
 *
 * 栈给 2 页（8KiB）。本 Lab 的程序栈用量都极小（最深的是 sh 的
 * run_cmd -> run_child -> exec，几十字节的局部变量），1 页就够；给 2 页
 * 是为了让"栈由多页组成"这件事在代码里真的是一个循环，而不是一个退化
 * 成单次的特例——学生改成 4 页时不需要改结构。 */
#define USER_STACK_TOP   0x800000ull
#define USER_STACK_PAGES 2

/* 允许加载的最低用户虚拟地址。
 *
 * 取 0x400000（和 user.ld 的 ENTRY 地址一致）而不是 0：地址 0 附近那一页
 * 必须永远保持*没有映射*，这样解引用空指针才会触发缺页异常。如果允许段
 * 加载到 0 页，一个程序可以把自己的代码映射到那里，此后这个进程里所有
 * 空指针解引用都变成正常的读写，最典型的一类 bug 就此失去它唯一的自动
 * 检测手段。真实系统把这件事做成了全局策略（Linux 的
 * vm.mmap_min_addr，默认 64KiB），起因是一系列"内核解引用空指针、而攻击
 * 者事先在 0 页放好了数据"的提权漏洞。 */
#define USER_MIN_VADDR   0x400000ull

/* argv 暂存区的两个上限。
 *
 * 为什么必须暂存：argv 里的指针和字符串都躺在*调用者的*地址空间里（是
 * sh.c 的 argv_buf 和 buf 这两个数组，在旧的用户栈/.bss 上）。exec 要
 * 做的第一件不可逆的事就是拆掉那个地址空间——拆完之后再去读 argv，读到
 * 的是已经被 kfree_page() 还回池子、可能已经分配给新页表节点的物理页。
 * 所以顺序只能是：先把字符串完整拷进内核自己的内存，再拆。
 *
 * 这是 exec 实现里最容易漏的一步，因为漏了之后"大部分时候还是对的"——
 * 旧物理页刚被释放、内容还没被覆盖，读出来仍然是原来的字符串。要等到
 * 某次分配恰好复用了那一页，才会突然表现成"argv[0] 是乱码"。这类
 * "读已经释放的内存，靠运气正确"的 bug 在内核里是最难复现的一类。
 *
 * MAXARG 取 16：sh.c 的 MAXARGS 是 10，留一点余量。UARGBYTES 取 512:
 * 本 Lab 最长的命令行是 initrc 里的 `cat /motd.txt | grep lab`，几十
 * 字节；512 足够，而且它同时是"新栈上放得下"的保证——见 build_stack()。 */
#define MAXARG     16
#define UARGBYTES  512

/* argv 暂存区。static 的安全性论证见文件顶部"exec 全程不会被切走"。
 *
 * 两个数组分别存"字符串内容"和"每个字符串在 ubuf 里的起始下标"。存下标
 * 而不是存指针，是因为这些字符串最终要被拷到*新的用户栈*上，那时候每个
 * 字符串的用户态地址才确定；在暂存阶段谈"指针"没有意义。
 *
 * ubytes 记录本次实际用掉多少字节。它不是"省一点拷贝"的优化——因为 ubuf
 * 是 static，上一次 exec 留下的内容还在里面，如果 build_stack() 图省事
 * 把整个 512 字节都拷到新栈上，上一条命令的参数就会躺在新进程的栈上，
 * 新进程读得到。本 Lab 里泄漏的是"上一条 shell 命令"，看着无害；同样的
 * 代码放到真实系统上，泄漏的可能是上一个进程命令行里的密码。内核往用户
 * 空间拷东西，只拷确实该给它的那些字节，多一个都不行。 */
static char   ubuf[UARGBYTES];
static uint32_t uoff[MAXARG];
static int      unarg;
static uint32_t ubytes;

/* 把 argv 的内容整份拷进内核暂存区，成功返回 0，失败返回 -1。
 *
 * 必须在提交线之前调用（理由见 ubuf 上方注释）。失败的两种情况——参数
 * 太多、字符串总长超限——都是用户程序能触发的正常错误，返回 -1 让 exec
 * 干净失败，不 panic。
 *
 * 关于直接解引用用户指针：本课程从 Lab7 的 sys_write 开始就是这么做的,
 * 内核和用户共用同一份页表（内核范围在每个进程页表里都有映射），一个
 * 用户态地址在内核里可以直接当指针用。这是一个*刻意的简化*，也是一个
 * 真实的安全漏洞：用户程序传一个内核地址进来，内核就会替它读/写内核
 * 内存。真实内核必须逐个地址检查"这个地址确实属于调用者的用户范围"
 * （Linux 的 copy_from_user/access_ok），本课程把这件事留给挑战任务,
 * 在 README 里有说明。这里不额外重复，只强调一点：argv 是本 Lab 里
 * 第一个"用户传进来的、内核要跟着走两层指针"的参数（先读指针数组，
 * 再读每个指针指向的字符串），比 sys_write 的单个缓冲区更能说明为什么
 * 真实内核需要那套检查——两层里任何一层都可能指向不该读的地方。 */
static int stage_argv(char *const argv[])
{
    unarg = 0;
    ubytes = 0;
    uint32_t used = 0;

    if (argv == NULL) {
        /* 允许 argv 为空：退化成"只有程序名都没有"的调用。本 Lab 的
         * 用户程序不会这么用（crt0 需要 argc>=1 才有 argv[0]），但内核
         * 不该因为一个空指针就 panic。 */
        return 0;
    }

    for (int i = 0; argv[i] != NULL; i++) {
        if (i >= MAXARG) {
            return -1;
        }

        const char *s = argv[i];
        uoff[i] = used;

        /* 逐字节拷，同时检查总长——不能先 strlen 再判断，因为 strlen
         * 本身会先走完整个字符串；如果用户传进来的"字符串"没有结尾的
         * NUL（完全可能，内核不能假设用户数据格式正确），strlen 会一路
         * 读下去，直到撞进某个没有映射的页触发缺页。边拷边判上限，读到
         * 的字节数天然被 UARGBYTES 限住，坏数据最多让 exec 失败，不会
         * 让内核走进不可控的读取。 */
        for (uint32_t j = 0;; j++) {
            if (used >= UARGBYTES) {
                return -1;
            }
            ubuf[used] = s[j];
            used++;
            if (s[j] == '\0') {
                break;
            }
        }
        unarg = i + 1;
    }

    ubytes = used;
    return 0;
}

/* 从文件里读*恰好* n 字节到 dst，少一个字节都算失败。
 *
 * fs_read 的约定是"返回实际读到的字节数"（读到文件尾就少给），这对
 * cat 那种"读到多少算多少"的调用者是合适的，但对 ELF 解析完全不行:
 * ELF 头必须是完整 64 字节，program header 必须是完整 56 字节，短读
 * 意味着文件被截断了。如果把短读当成功，后面解析的是一个半截结构体
 * 加上缓冲区里的残留内容，e_phoff/p_vaddr 全是垃圾值，而第一个真正
 * 的报错会出现在很远的地方（比如"段的虚拟地址不合法"），把人引向
 * 错误的方向。让"文件太短"在读的那一刻就成为错误。 */
static int read_exact(uint32_t inum, uint32_t off, void *dst, uint32_t n)
{
    uint32_t got = fs_read(inum, off, dst, n);
    return (got == n) ? 0 : -1;
}

/* 校验 ELF 头。只读，不改任何状态；返回 0 表示"这个文件看起来能加载"。
 *
 * 每一项检查都对应一种真实会发生的错误输入，不是凑数：
 *   - 魔数：路径指到了一个不是 ELF 的文件（比如 `./motd.txt`）。
 *   - EI_CLASS/EI_DATA：32 位或大端的 ELF。本课程不会产生，但学生把
 *     宿主机上某个 32 位程序塞进 fsroot/ 是完全可能的。
 *   - e_type：PIE 可执行文件是 ET_DYN，需要重定位，我们不支持（elf.h
 *     里 ET_DYN 那段注释解释了这个坑怎么被踩到）。
 *   - e_machine：跨架构的 ELF，见 EXPECTED_EM 上方注释。
 *   - e_phentsize/e_phnum：phdr 表本身的一致性，见 elf.h 里 e_phentsize
 *     那段注释。e_phnum==0 意味着没有任何段可加载，加载出来的进程一进
 *     用户态就在未映射的地址取指。
 *
 * 全部用 kprintf 报告具体哪一项不满意再返回。exec 失败时用户只能看到
 * 一个 -1，"为什么失败"必须由内核打出来——否则唯一的线索是 shell 那句
 * "exec failed"，而失败原因可能是十来种里的任意一种。 */
static int check_ehdr(const struct Elf64_Ehdr *e, const char *path)
{
    if (e->e_ident[EI_MAG0] != ELFMAG0 || e->e_ident[EI_MAG1] != ELFMAG1 ||
        e->e_ident[EI_MAG2] != ELFMAG2 || e->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf("exec: %s: 不是 ELF 文件（魔数不对）\n", path);
        return -1;
    }
    if (e->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf("exec: %s: 不是 64 位 ELF（EI_CLASS=%u）\n", path,
                (uint32_t)e->e_ident[EI_CLASS]);
        return -1;
    }
    if (e->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("exec: %s: 不是小端 ELF（EI_DATA=%u）\n", path,
                (uint32_t)e->e_ident[EI_DATA]);
        return -1;
    }
    if (e->e_type != ET_EXEC) {
        kprintf("exec: %s: e_type=%u，只支持 ET_EXEC（PIE 是 ET_DYN=3，"
                "需要重定位，本课程不支持）\n", path, (uint32_t)e->e_type);
        return -1;
    }
    if (e->e_machine != EXPECTED_EM) {
        kprintf("exec: %s: e_machine=%u，本内核只能执行 %u\n", path,
                (uint32_t)e->e_machine, (uint32_t)EXPECTED_EM);
        return -1;
    }
    if (e->e_phentsize != sizeof(struct Elf64_Phdr)) {
        kprintf("exec: %s: e_phentsize=%u，期望 %u\n", path,
                (uint32_t)e->e_phentsize, (uint32_t)sizeof(struct Elf64_Phdr));
        return -1;
    }
    if (e->e_phnum == 0) {
        kprintf("exec: %s: 没有 program header，没有任何段可加载\n", path);
        return -1;
    }
    return 0;
}

/* 校验一个 PT_LOAD 段。只读；返回 0 表示这个段可以安全加载。
 *
 * file_size 是文件的实际字节数，用来判断段声称的内容是否真的在文件里。
 *
 * 这里的每一项都在防一类具体的坏事，值得逐条看，因为"加载器要校验什么"
 * 是这个 Lab 真正的知识点之一——加载器读的是*磁盘上的、可能被任意构造
 * 的字节*，它和用户程序传进来的参数一样不可信。真实世界里 ELF 加载器的
 * 校验漏洞是内核提权漏洞的经典来源。
 *
 *   1. p_memsz < p_filesz：ELF 的约定是 memsz >= filesz（差额是 .bss)。
 *      反过来意味着"文件里的字节比内存里的位置还多"，后面按 memsz 分配、
 *      按 filesz 拷贝就会写出界。
 *
 *   2. p_offset + p_filesz 溢出或超过文件大小：段声称的内容不在文件里。
 *      不查的话 fs_read 会短读，段的尾部是缓冲区残留内容。溢出要单独
 *      查——两个 uint64 相加可以绕回一个很小的数，让"超过文件大小"这个
 *      检查通过。这是整个函数里最容易漏的一行。
 *
 *   3. p_vaddr 必须页对齐：本加载器按页建立映射，段起始不对齐的话
 *      "这一页里从哪个字节开始是段内容"就需要额外处理。真实加载器确实
 *      处理这种情况（mmap 允许段在页内有偏移），我们选择要求对齐并在
 *      不满足时明确报错——user.ld 保证了这一点，所以这个限制对本课程的
 *      产物没有影响，而它把 load_segment() 简化了一整个维度。
 *
 *   4. 地址范围必须落在 [USER_MIN, USER_STACK_TOP - 栈) 里：这是最关键
 *      的一条。少了它，一个恶意构造的 ELF 可以声明 p_vaddr 落在内核
 *      地址范围，pagetable_map() 会老老实实把用户可写的页映射到内核
 *      虚拟地址上去——用户程序于此获得了改写内核内存的能力。这是一条
 *      真正的权限边界检查，不是防手误。
 *
 * 有一项检查*不*在这里做：总页数是否超出 NUSERPAGE 的预算。那是所有段
 * 加上栈之后的整体性质，不是单个段的性质，所以由 exec_load() 在校验循环
 * 里累加着查——放在这里的话，每个段都"单独看着没超"但加起来超了的情况会
 * 漏过校验，到提交线之后才被 uvm_track() 发现，而那时已经没有退路了。 */
static int check_phdr(const struct Elf64_Phdr *ph, const char *path,
                      uint32_t file_size)
{
    if (ph->p_memsz < ph->p_filesz) {
        kprintf("exec: %s: 段的 p_memsz < p_filesz，ELF 损坏\n", path);
        return -1;
    }
    if (ph->p_offset + ph->p_filesz < ph->p_offset ||
        ph->p_offset + ph->p_filesz > (uint64_t)file_size) {
        kprintf("exec: %s: 段声称的内容超出文件范围（文件只有 %u 字节）\n",
                path, file_size);
        return -1;
    }
    if ((ph->p_vaddr & (PAGE_SIZE - 1)) != 0) {
        kprintf("exec: %s: 段的 p_vaddr=%p 没有页对齐\n", path,
                (uintptr_t)ph->p_vaddr);
        return -1;
    }
    if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
        kprintf("exec: %s: 段的地址范围溢出\n", path);
        return -1;
    }
    if (ph->p_vaddr < USER_MIN_VADDR ||
        ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE) {
        kprintf("exec: %s: 段的地址范围 [%p, %p) 超出允许的用户范围\n", path,
                (uintptr_t)ph->p_vaddr,
                (uintptr_t)(ph->p_vaddr + ph->p_memsz));
        return -1;
    }
    return 0;
}

/* 把 ELF 的 p_flags 翻译成本课程页表的 PTE_FLAG_*。
 *
 * 三件事值得注意：
 *
 *   1. PTE_FLAG_USER 无条件加上。这是用户程序的段，ring3/U 态必须能访问,
 *      不加的话一进用户态就是缺页/访问故障。
 *
 *   2. PF_R 没有对应的标志位。本课程的页表接口里"可读"是隐含的——一个
 *      存在的映射天然可读，没有"映射了但不能读"这种状态（x86_64 的 PTE
 *      里确实没有读权限位；riscv64 有 R 位，pagetable.c 在建立映射时
 *      统一置上）。所以这里只翻译 W 和 X。
 *
 *   3. 位编号是反的。ELF 里 X=1/W=2/R=4，本课程的 PTE_FLAG_WRITABLE/
 *      EXECUTABLE 是 bit 0/1。两套编号没有任何对应关系，必须逐位显式
 *      翻译，不能整体位运算糊过去——elf.h 里 PF_X 那段注释说明了翻译
 *      错的两种后果，其中"数据段变成可执行"是不报错的那种。 */
static uint32_t flags_of(uint32_t p_flags)
{
    uint32_t f = PTE_FLAG_USER;
    if (p_flags & PF_W) {
        f |= PTE_FLAG_WRITABLE;
    }
    if (p_flags & PF_X) {
        f |= PTE_FLAG_EXECUTABLE;
    }
    return f;
}

/* 加载一个 PT_LOAD 段：逐页分配物理内存、填内容、建立映射。
 *
 * 在提交线之后调用，不允许失败——分配不到内存只能 panic（见 exec_load()
 * 里提交线那段注释）。
 *
 * 每一页的处理是同一套三步，这三步覆盖了 filesz/memsz 的全部三种形状,
 * 不需要为它们分别写分支：
 *
 *   1. memset 整页为 0。
 *   2. 如果这一页落在 [0, p_filesz) 范围内，从文件读入重叠的那部分。
 *   3. 建立映射。
 *
 * 于是：filesz == memsz 的段每页都走完整的 1+2；filesz == 0 的纯 .bss 段
 * 每页都只走 1（第 2 步的 copy_len 算出来是 0，自然跳过）；0 < filesz <
 * memsz 的段前面几页走 1+2、后面几页只走 1，而跨界那一页走 1 + 部分 2。
 * 三种形状是同一段代码的三种取值，不是三条路径——这正是"先清零再覆盖"
 * 这个顺序的价值所在。
 *
 * 反过来，如果按"先拷 filesz 字节、再把剩下的清零"来写，就必须显式算出
 * 那个分界点、并且处理它落在页中间的情况，而清零的起点算错一页或者写成
 * p_vaddr 就会把刚拷进来的内容抹掉。sh.c 里 prompt[] 那段注释讨论的正是
 * 这个 bug 及其症状——它之所以能被本 Lab 的测试抓到，是因为 sh 是唯一
 * 一个有初始化过的 .data 的程序。
 *
 * 为什么先 memset 再 fs_read 而不是反过来：kalloc_page() 明确不保证返回
 * 零页（kalloc.h 里写了），拿到的页里是上一个使用者留下的内容。不清零的
 * 症状是"未初始化的全局变量里有垃圾"，而垃圾的具体内容取决于之前哪个
 * 进程用过这一页——同一个程序两次运行表现不同。 */
static void load_segment(struct proc *p, uint32_t inum,
                         const struct Elf64_Phdr *ph)
{
    uint32_t flags = flags_of(ph->p_flags);

    for (uint64_t off = 0; off < ph->p_memsz; off += PAGE_SIZE) {
        uintptr_t vaddr = (uintptr_t)(ph->p_vaddr + off);

        void *page_phys = kalloc_page();
        if (page_phys == NULL) {
            panic("exec: load_segment: kalloc_page() 失败（已过提交线，无法回退）");
        }
        uint8_t *page_kva = (uint8_t *)((uintptr_t)page_phys + KERNEL_VIRT_BASE);

        memset(page_kva, 0, PAGE_SIZE);

        /* 这一页和文件内容 [0, p_filesz) 的重叠部分有多长。
         *
         * off >= p_filesz 时整页都在 .bss 范围里，copy_len 为 0。否则
         * 取"文件里还剩多少"和"一页装得下多少"的较小者——最后一页通常
         * 不是整页。 */
        if (off < ph->p_filesz) {
            uint64_t remain = ph->p_filesz - off;
            uint32_t copy_len = (remain < PAGE_SIZE) ? (uint32_t)remain : PAGE_SIZE;
            if (read_exact(inum, (uint32_t)(ph->p_offset + off), page_kva,
                           copy_len) < 0) {
                panic("exec: load_segment: 段内容短读（校验阶段本该拦住，"
                      "说明文件在校验之后被改了，或者校验有漏）");
            }
        }

        pagetable_map(p->pagetable, vaddr, (uintptr_t)page_phys, flags);
        uvm_track(p, vaddr, flags);
    }
}

/* 建立用户栈，并把暂存的 argv 按 System V 的初始栈布局压上去。
 *
 * 返回新进程的初始栈指针——也就是 crt0 的 _start 执行第一条指令时 rsp/sp
 * 应该等于的值。
 *
 * 在提交线之后调用，不允许失败。
 *
 * ── 要造出来的布局 ────────────────────────────────────────────────
 *
 * crt0_x86_64.S / crt0_riscv64.S 读的就是这个形状（两个架构完全一样，
 * 这是 System V ABI 规定的，不是本课程的约定）：
 *
 *     高地址  ┌─────────────────┐ USER_STACK_TOP
 *             │ "sh\0" "/initrc\0" │  ← 字符串区，argv[i] 指向这里
 *             ├─────────────────┤
 *             │ （对齐填充）      │
 *             ├─────────────────┤
 *             │ NULL            │  ← argv[argc]，结尾哨兵
 *             │ argv[argc-1]    │
 *             │ ...             │
 *             │ argv[0]         │
 *             ├─────────────────┤
 *             │ argc            │  ← 返回的 sp 指向这里
 *     低地址  └─────────────────┘
 *
 * 字符串放在最高处、指针数组在下面，是因为指针必须指向一个*已经确定的*
 * 地址：先把字符串摆好，它们的用户态地址就定了，然后才能填指针。反过来
 * 做需要两趟（先算总长、再回填），没有好处。
 *
 * ── 对齐 ──────────────────────────────────────────────────────────
 *
 * 最后返回的 sp 必须 16 字节对齐。这一点在 crt0_x86_64.S 的注释里详细
 * 讨论过：System V ABI 要求的是"函数*调用*时 rsp+8 是 16 的倍数"（因为
 * call 会压 8 字节返回地址），而进程*入口*的 sp 就是 16 的倍数——这两件
 * 事差 8 字节，搞混了会让 crt0 里多一条莫名其妙的 subq $8, %rsp。这里
 * 造的是后者：_start 不是被 call 进来的，没有返回地址。
 *
 * 不对齐的后果在 x86_64 上是真实的：编译器会假设栈对齐，对 16 字节的
 * SSE 访问用 movaps 这类要求对齐的指令。不过本 Lab 的用户程序用
 * -mgeneral-regs-only 编译，不会有 SSE 指令，所以*本课程范围内*不对齐
 * 大概不会炸——这恰恰是要按 ABI 做对的理由：一个"现在碰巧不炸"的
 * 违约，会在某天有人去掉那个编译选项时变成一个完全无法理解的崩溃。 */
static uintptr_t build_stack(struct proc *p)
{
    /* 先分配并映射栈页。从低地址往高地址循环，这样 upages[] 里记录的
     * 顺序是递增的（没有功能要求，但调试时打印出来更容易看）。 */
    uintptr_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uintptr_t vaddr = stack_bottom + (uintptr_t)i * PAGE_SIZE;

        void *page_phys = kalloc_page();
        if (page_phys == NULL) {
            panic("exec: build_stack: kalloc_page() 失败（已过提交线，无法回退）");
        }
        memset((void *)((uintptr_t)page_phys + KERNEL_VIRT_BASE), 0, PAGE_SIZE);

        uint32_t flags = PTE_FLAG_USER | PTE_FLAG_WRITABLE;
        pagetable_map(p->pagetable, vaddr, (uintptr_t)page_phys, flags);
        uvm_track(p, vaddr, flags);
    }

    /* 现在栈已经映射好了，但*还不能*直接往用户虚拟地址写——当前 CR3/satp
     * 是调用者的页表，而这些新映射是写进 p->pagetable 的；在 exec 自己
     * 这条路径上 p 就是当前进程，页表根没换、只是加了表项，理论上可以
     * 直接写。但依赖这一点会让代码只在"exec 作用于当前进程"时正确。
     *
     * 所以统一走"物理地址 + 内核偏移"这条路：要往某个用户虚拟地址写，
     * 先 pagetable_lookup() 查出它映射到哪个物理页，再通过内核偏移映射
     * 去写。多一次查表，换来的是这段代码对"p 是不是当前进程"完全不敏感。
     *
     * 这个辅助逻辑写成一个小的内联步骤而不是函数，因为它只在下面用两次,
     * 而且每次要写的东西类型不同（一次是字节串，一次是 uint64_t 数组）。 */

    /* 第一步：把字符串区放在栈顶往下。只保留 ubytes 字节——本次 argv
     * 实际用掉的长度，不是 UARGBYTES 上限（理由见 ubytes 的声明处）。
     *
     * 每个字符串在用户态的地址就是 argv_strings_base + uoff[i]，因为
     * ubuf 里的相对布局被原样搬到了栈上。这是用下标而不是指针暂存的
     * 回报：一次整体平移就完成了"内核缓冲区坐标"到"用户栈坐标"的换算。
     *
     * UARGBYTES 和栈大小之间的耦合也在这里：UARGBYTES 必须远小于
     * USER_STACK_PAGES * PAGE_SIZE，否则字符串区会把整个栈吃掉，栈指针
     * 会落到栈底之下没有映射的地方。512 对 8192 是 1/16，安全。 */
    uintptr_t sp = USER_STACK_TOP - (uintptr_t)ubytes;

    uintptr_t argv_strings_base = sp;
    for (uint32_t i = 0; i < ubytes; i++) {
        uintptr_t va = argv_strings_base + i;
        uintptr_t page = pagetable_lookup(p->pagetable, va & ~(uintptr_t)(PAGE_SIZE - 1));
        if (page == 0) {
            panic("exec: build_stack: 字符串区落在没有映射的地址上——"
                  "UARGBYTES 是不是超过了栈的大小？");
        }
        uint8_t *dst = (uint8_t *)(page + KERNEL_VIRT_BASE + (va & (PAGE_SIZE - 1)));
        *dst = (uint8_t)ubuf[i];
    }

    /* 第二步：指针数组 + argc。一共 1 + unarg + 1 个 8 字节的格子
     * （argc、argv[0..unarg-1]、结尾 NULL）。 */
    uintptr_t nslots = 1 + (uintptr_t)unarg + 1;
    sp -= nslots * 8;
    sp &= ~(uintptr_t)15;   /* 16 字节对齐，见函数头注释。 */

    for (uintptr_t i = 0; i < nslots; i++) {
        uintptr_t va = sp + i * 8;
        uintptr_t page = pagetable_lookup(p->pagetable, va & ~(uintptr_t)(PAGE_SIZE - 1));
        if (page == 0) {
            panic("exec: build_stack: 指针数组落在没有映射的地址上");
        }
        uint64_t *slot = (uint64_t *)(page + KERNEL_VIRT_BASE + (va & (PAGE_SIZE - 1)));

        if (i == 0) {
            *slot = (uint64_t)unarg;                       /* argc */
        } else if (i < nslots - 1) {
            *slot = argv_strings_base + uoff[i - 1];       /* argv[i-1] */
        } else {
            *slot = 0;                                     /* argv[argc] = NULL */
        }
    }

    return sp;
}

/* 把 n 向上取整到页边界。p_memsz 不是页的整数倍时，"这个段占几页"要算
 * 的是 ceil，少算一页的后果是段尾部那几个字节没有映射。 */
static uint64_t page_round_up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* 加载 path 指定的 ELF 可执行文件到 p 的地址空间，并按 argv 建好初始栈。
 *
 * 成功返回 0，并通过 entry_out/sp_out 交出新进程的入口地址和初始栈指针；
 * 失败返回 -1，此时 p 的地址空间*一个字节都没有动过*。
 *
 * ── 两趟遍历 ──────────────────────────────────────────────────────
 *
 * program header 表被读了两趟：第一趟只校验和统计，第二趟才真正加载。
 * 代价是每个段多读一次 56 字节，换来的是不需要 `struct Elf64_Phdr
 * phdrs[MAXPHDR]` 这个数组——那个数组会引入一个新的上限（"段太多"又是
 * 一种要处理的失败），而它存在的唯一理由只是避免重读几十个字节。
 *
 * 更重要的是两趟的划分正好落在提交线上：第一趟全部通过，才动手改地址
 * 空间。如果边校验边加载，一个"第三个段有问题"的 ELF 会让前两个段已经
 * 装进去、旧地址空间已经拆掉，然后失败——这时候既回不去也走不了，只能
 * panic。两趟是"exec 失败可以恢复"这个语义的实现方式，不是风格选择。
 *
 * ── 为什么 sh 依赖 exec 失败可恢复 ────────────────────────────────
 *
 * sh.c 的 run_cmd() 在 exec 返回之后会打 "command not found" 再 exit(1)。
 * 那句话能被打出来，前提是 exec 失败之后 sh 的代码、数据、栈全都还在。
 * 用户输一个不存在的命令是最常见的事，一次拼错就让 shell 死掉的话，这个
 * shell 没法用。 */
int exec_load(struct proc *p, const char *path, char *const argv[],
              uintptr_t *entry_out, uintptr_t *sp_out)
{
    /* ════════ 提交线之前：只读 ════════ */

    /* argv 必须在拆旧地址空间之前拷进内核——argv 本身、以及它指向的字符串，
     * 全都住在调用者的用户内存里（通常就在旧的用户栈上）。理由见 ubuf。 */
    if (stage_argv(argv) < 0) {
        kprintf("exec: %s: 参数太多或总长超过 %u 字节\n", path,
                (uint32_t)UARGBYTES);
        return -1;
    }

    uint32_t inum = fs_lookup(path);
    if (inum == 0) {
        /* 这条路径是最常走的失败：用户敲错了命令名。不打内核日志——sh
         * 会打 "command not found"，内核再打一遍只是噪音。上面那些
         * kprintf 报的是"文件在但是有问题"，那才是需要内核出声的情况。 */
        return -1;
    }
    uint32_t file_size = fs_size(inum);

    struct Elf64_Ehdr eh;
    if (read_exact(inum, 0, &eh, sizeof(eh)) < 0) {
        kprintf("exec: %s: 文件只有 %u 字节，装不下 ELF 头\n", path, file_size);
        return -1;
    }
    if (check_ehdr(&eh, path) < 0) {
        return -1;
    }
    if (eh.e_phoff > (uint64_t)file_size) {
        kprintf("exec: %s: e_phoff 指到了文件外面\n", path);
        return -1;
    }

    /* 第一趟：校验每个 PT_LOAD，累计页数，顺便确认 e_entry 落在某个可执行
     * 段里。三件事放在一个循环里，因为它们需要的都是同一份 phdr 数据。 */
    uint64_t npages = USER_STACK_PAGES;   /* 栈的页数先算进预算。 */
    uint64_t prev_end = 0;                /* 上一个段的页对齐结束地址。 */
    int entry_ok = 0;
    int nload = 0;

    for (uint32_t i = 0; i < eh.e_phnum; i++) {
        uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(struct Elf64_Phdr);
        struct Elf64_Phdr ph;
        if (read_exact(inum, (uint32_t)off, &ph, sizeof(ph)) < 0) {
            kprintf("exec: %s: 读第 %u 个 program header 失败\n", path, i);
            return -1;
        }
        if (ph.p_type != PT_LOAD) {
            /* 非 PT_LOAD 一律跳过。PT_GNU_STACK（描述栈权限)、PT_NOTE、
             * PT_PHDR 都会出现在我们自己链出来的 ELF 里,它们不需要加载。
             * 唯一需要警惕的是 PT_INTERP——那表示"这个程序需要动态链接器",
             * 我们没有动态链接器,但因为 e_type 已经挡掉了 ET_DYN,静态
             * 链接的产物不会带 PT_INTERP,所以这里不额外处理。 */
            continue;
        }
        if (check_phdr(&ph, path, file_size) < 0) {
            return -1;
        }

        /* 两个段不能共用同一页。ELF 规定 PT_LOAD 按 p_vaddr 升序排列,
         * 所以"这个段的起始 < 上一个段的页对齐结束"同时抓住了两种坏情况:
         * 段重叠、以及段没有按升序排（后者会让前者的检查失效)。
         *
         * 为什么必须查：load_segment() 对每一页都是"分配新页 + 清零 +
         * 建映射"。如果两个段落在同一页上,第二个段会为这一页分配*另一个*
         * 物理页并覆盖掉映射——第一个段刚写进去的内容就此消失,而它的物理
         * 页还挂在 upages[] 里,同一个虚拟地址被记了两次,退出时会被
         * kfree_page() 两次。一次静默的数据丢失加一次 double free。
         *
         * p_vaddr 页对齐（check_phdr 第 3 条）并不能替代这个检查：对齐
         * 管的是段的*开头*,而一个段的 p_memsz 可以让它的尾部延伸进下一个
         * 段的起始页。 */
        if (ph.p_vaddr < prev_end) {
            kprintf("exec: %s: 第 %u 个段与前一个段共用页面或未按地址升序"
                    "排列\n", path, i);
            return -1;
        }
        prev_end = ph.p_vaddr + page_round_up(ph.p_memsz);

        npages += page_round_up(ph.p_memsz) / PAGE_SIZE;
        nload++;

        /* e_entry 必须落在一个可执行段里。不查的话,一个 e_entry 指向
         * 未映射地址的 ELF 会被成功"加载",然后进程一进用户态就立刻在
         * 一个看不出来源的地址上取指失败——排查时人会去看加载器的映射
         * 逻辑,而错误其实在 ELF 头的一个字段里。在这里查,报错能直接
         * 说出"入口地址不在任何可执行段内"。
         *
         * 要求可执行而不只是"有映射",是因为入口落在数据段同样是坏的:
         * x86_64 上 NX 会让它变成一个缺页异常,riscv64 上没有 X 位的页
         * 取指也会 fault,症状和完全没映射一样难查。 */
        if ((ph.p_flags & PF_X) && eh.e_entry >= ph.p_vaddr &&
            eh.e_entry < ph.p_vaddr + ph.p_memsz) {
            entry_ok = 1;
        }
    }

    if (nload == 0) {
        kprintf("exec: %s: 没有 PT_LOAD 段\n", path);
        return -1;
    }
    if (!entry_ok) {
        kprintf("exec: %s: e_entry=%p 不在任何可执行段内\n", path,
                (uintptr_t)eh.e_entry);
        return -1;
    }
    if (npages > NUSERPAGE) {
        /* 整体性质，只能在这里查（见 check_phdr 末尾那段注释）。 */
        kprintf("exec: %s: 需要 %u 页用户内存，超过每进程上限 %u 页\n", path,
                (uint32_t)npages, (uint32_t)NUSERPAGE);
        return -1;
    }

    /* ════════════════════════ 提 交 线 ════════════════════════
     *
     * 到这里为止，所有能预先判断的失败都判断过了，而且什么都没改。下面
     * 第一条语句一执行，旧地址空间就没了，exec 再也不能"干净失败"。
     *
     * 所以下面每一处出错都是 panic()。这不是偷懒——是因为确实无路可走:
     * 旧程序的代码和数据已经被拆掉,没法返回 -1 让它继续跑;新程序也还
     * 没装完,没法跳进去。一个"回退失败"的 exec 需要在拆之前把旧地址
     * 空间完整地复制一份留底,那是 fork 级别的开销加上双倍的内存占用,
     * 真实内核也不这么做（Linux 的 execve 过了 flush_old_exec() 之后
     * 失败同样只能给进程发 SIGSEGV/SIGKILL,回不到调用者)。
     *
     * 提交线之后唯一还会失败的事是 kalloc_page() 返回 NULL。上面的页数
     * 预算检查只保证"不超过每进程上限",不保证"系统此刻还有这么多空闲
     * 物理页"——后者没法在这里可靠地预判（别的进程随时在分配),真要做到
     * 就得先把页全部预分配好再开始拆,那又是另一套复杂度。本课程选择
     * panic 并在 README 的挑战任务里点出这个缺口。 */

    /* 拆掉旧的用户映射。内核范围的映射不动——我们跑在内核栈上，而且
     * 紧接着还要通过内核偏移映射去写新分配的页。 */
    uvm_clear(p);

    /* 第二趟：真正加载。重读一遍 phdr——文件内容在这两趟之间不可能变
     * （本课程没有写文件的系统调用，而且 exec 全程不会被切走），所以
     * 第二趟读到的必然和第一趟校验过的是同一份数据。 */
    for (uint32_t i = 0; i < eh.e_phnum; i++) {
        uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(struct Elf64_Phdr);
        struct Elf64_Phdr ph;
        if (read_exact(inum, (uint32_t)off, &ph, sizeof(ph)) < 0) {
            panic("exec: 第二趟读 program header 失败（第一趟刚读成功过，"
                  "文件系统状态不一致）");
        }
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        load_segment(p, inum, &ph);
    }

    uintptr_t sp = build_stack(p);

    /* 刷 TLB。pagetable_unmap()/pagetable_map() 都只改内存里的页表项，
     * 不碰 TLB；旧地址空间的翻译此刻还缓存在里面，其中有些指向的物理页
     * 已经被 uvm_clear() 还给 kalloc 了。不刷的话，新程序访问自己的代码
     * 段可能命中一条旧的 TLB 项，读到的是上一个程序的内容，或者是一个
     * 已经被别的进程重新分配走的页。
     *
     * 用 pagetable_activate() 而不是逐页 invlpg：换根本身就隐式刷掉整个
     * TLB（x86_64 写 CR3 的架构行为；riscv64 那边 pagetable_activate()
     * 里有 sfence.vma），一条指令解决，而且两个架构的接口一致。p 的页表
     * 根没有变，这里是"用同一个值重新激活一次"，唯一目的就是那个副作用。
     *
     * Lab7 的 sys_exec() 没有这一步也能跑，是因为它只换两个固定的虚拟
     * 地址、而且换完立刻 kalloc 把刚释放的页原样拿回来用了，旧 TLB 项
     * 指向的物理页恰好还是对的。Lab9 的地址空间是按 ELF 变化的，这个
     * 巧合不再成立。 */
    pagetable_activate(p->pagetable);

    *entry_out = (uintptr_t)eh.e_entry;
    *sp_out = sp;
    return 0;
}
