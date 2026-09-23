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
 *     没有正确的退路了（见 exec_load() 提交线那段注释对"另一种做法"
 *     的讨论）。
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

/* TODO 1：实现 stage_argv(argv)。
 *
 * 返回值：成功返回 0，参数太多或总长超过 UARGBYTES 返回 -1。
 *
 * 必须在提交线之前调用（理由见 ubuf 上方注释）。这两种失败——参数太多、
 * 字符串总长超限——都是用户程序能触发的正常错误，返回 -1 让 exec 干净
 * 失败，不 panic。
 *
 * 步骤：
 *   1. unarg = 0，ubytes = 0，used = 0（一个局部变量，记录已经用掉的
 *      字节数）。
 *   2. argv == NULL 时直接返回 0——允许调用者不传任何参数。
 *   3. 否则遍历 argv[i]（i 从 0 开始，直到 argv[i] == NULL）：
 *        - i 达到 MAXARG 时返回 -1（参数太多）。
 *        - uoff[i] = used（记录这个字符串在 ubuf 里的起始位置）。
 *        - 逐字节把 argv[i] 拷进 ubuf[used]，used 每次 +1；拷之前先检查
 *          used 是否已经到 UARGBYTES，到了就返回 -1（不能先 strlen 再
 *          判断——见下面这段的完整论证）。拷到 '\0' 为止（这个 NUL 也要
 *          算进 used，一起拷进 ubuf）。
 *        - unarg = i + 1。
 *   4. 循环结束后 ubytes = used，返回 0。
 *
 * 关于"不能先 strlen 再判断"：必须边拷边判上限，而不是先对每个字符串调
 * strlen() 算出长度、再检查总和是否超限——因为 strlen 本身会先走完整个
 * 字符串；如果用户传进来的"字符串"没有结尾的 NUL（完全可能，内核不能
 * 假设用户数据格式正确），strlen 会一路读下去，直到撞进某个没有映射的
 * 页触发缺页。边拷边判上限，读到的字节数天然被 UARGBYTES 限住，坏数据
 * 最多让 exec 失败，不会让内核走进不可控的读取。
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
    (void)argv;
    unarg = 0;
    ubytes = 0;
    return -1;
}

/* 从文件里读*恰好* n 字节到 dst，少一个字节都算失败。
 *
 * 已经写好，不是 TODO：这个函数就是 fs_read 的一层薄包装，本身没有值得
 * 练习的逻辑；它*为什么*需要存在才是本 Lab 的知识点，值得读一下。
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

/* TODO 2：实现 check_ehdr(e, path)。
 *
 * 返回值：0 表示"这个文件看起来能加载"，-1 表示不能（并且已经用 kprintf
 * 说明了具体原因）。只读，不改任何状态。
 *
 * 依次检查（任何一项不满足就 kprintf 说明原因、返回 -1）：
 *   1. e_ident[EI_MAG0..EI_MAG3] 分别等于 ELFMAG0..ELFMAG3——魔数不对说明
 *      这根本不是 ELF 文件（比如路径指到了 `./motd.txt`）。
 *   2. e_ident[EI_CLASS] == ELFCLASS64——不是 64 位 ELF。
 *   3. e_ident[EI_DATA] == ELFDATA2LSB——不是小端 ELF。
 *   4. e_type == ET_EXEC——PIE 可执行文件是 ET_DYN，需要重定位，本课程
 *      不支持（elf.h 里 ET_DYN 那段注释解释了这个坑怎么被踩到）。
 *   5. e_machine == EXPECTED_EM——跨架构的 ELF，见文件顶部 EXPECTED_EM
 *      上方的说明。
 *   6. e_phentsize == sizeof(struct Elf64_Phdr)——phdr 表本身的一致性,
 *      见 elf.h 里 e_phentsize 那段注释。
 *   7. e_phnum != 0——没有任何段可加载的话，加载出来的进程一进用户态就
 *      在未映射的地址上取指。
 *
 * 每一项检查都对应一种真实会发生的错误输入，不是凑数——上面括号里写的
 * 就是对应的真实场景。exec 失败时用户只能看到一个 -1，"为什么失败"必须
 * 由内核打出来——否则唯一的线索是 shell 那句 "exec failed"，而失败原因
 * 可能是十来种里的任意一种。 */
static int check_ehdr(const struct Elf64_Ehdr *e, const char *path)
{
    (void)e;
    (void)path;
    return -1;
}

/* TODO 3：实现 check_phdr(ph, path, file_size)。
 *
 * 返回值：0 表示这个 PT_LOAD 段可以安全加载，-1 表示不能（已经用 kprintf
 * 说明原因）。只读。file_size 是文件的实际字节数，用来判断段声称的内容
 * 是否真的在文件里。
 *
 * 依次检查：
 *   1. ph->p_memsz >= ph->p_filesz——ELF 的约定是 memsz >= filesz（差额
 *      是 .bss)。反过来意味着"文件里的字节比内存里的位置还多"，后面按
 *      memsz 分配、按 filesz 拷贝就会写出界。
 *
 *   2. ph->p_offset + ph->p_filesz 不溢出，且不超过 file_size——段声称的
 *      内容必须真的在文件里。溢出要*单独*判断（不能只判断"和 > 文件大小"),
 *      因为两个 uint64 相加可以绕回一个很小的数，让"超过文件大小"这个
 *      检查通过。这是整个函数里最容易漏的一行。
 *
 *   3. ph->p_vaddr 页对齐（(p_vaddr & (PAGE_SIZE-1)) == 0）——本加载器
 *      按页建立映射，段起始不对齐的话"这一页里从哪个字节开始是段内容"
 *      就需要额外处理。user.ld 保证了这一点，所以这个限制对本课程的
 *      产物没有影响，而它把 load_segment() 简化了一整个维度。
 *
 *   4. ph->p_vaddr + ph->p_memsz 不溢出。
 *
 *   5. 地址范围 [p_vaddr, p_vaddr + p_memsz) 必须完全落在
 *      [USER_MIN_VADDR, USER_STACK_TOP - USER_STACK_PAGES*PAGE_SIZE) 内——
 *      这是最关键的一条。少了它，一个恶意构造的 ELF 可以声明 p_vaddr
 *      落在内核地址范围，pagetable_map() 会老老实实把用户可写的页映射
 *      到内核虚拟地址上去——用户程序于此获得了改写内核内存的能力。这是
 *      一条真正的权限边界检查，不是防手误。
 *
 * 有一项检查*不*在这里做：总页数是否超出 NUSERPAGE 的预算。那是所有段
 * 加上栈之后的整体性质，不是单个段的性质，由 exec_load() 在校验循环里
 * 累加着查——放在这里的话，每个段都"单独看着没超"但加起来超了的情况会
 * 漏过校验，到提交线之后才被发现，而那时已经没有退路了。 */
static int check_phdr(const struct Elf64_Phdr *ph, const char *path,
                      uint32_t file_size)
{
    (void)ph;
    (void)path;
    (void)file_size;
    return -1;
}

/* TODO 4：实现 flags_of(p_flags)。
 *
 * 返回值：把 ELF 的 p_flags（PF_R/PF_W/PF_X 的组合）翻译成本课程页表
 * 接口用的 PTE_FLAG_*（pagetable.h 里定义）组合。
 *
 * 三件事要记住：
 *
 *   1. PTE_FLAG_USER 无条件加上。这是用户程序的段，ring3/U 态必须能
 *      访问，不加的话一进用户态就是缺页/访问故障。
 *
 *   2. PF_R 没有对应的标志位，不需要翻译。本课程的页表接口里"可读"是
 *      隐含的——一个存在的映射天然可读，没有"映射了但不能读"这种状态
 *      （x86_64 的 PTE 里确实没有读权限位；riscv64 有 R 位，pagetable.c
 *      在建立映射时统一置上）。所以只需要翻译 W 和 X 两位。
 *
 *   3. 位编号是反的，必须逐位显式翻译，不能整体位运算糊过去。ELF 里
 *      X=1/W=2/R=4，本课程的 PTE_FLAG_WRITABLE/EXECUTABLE 是 bit 0/1。
 *      两套编号没有任何对应关系——elf.h 里 PF_X 那段注释说明了翻译错的
 *      两种后果，其中"数据段变成可执行"是不报错的那种。
 *
 * 具体做法：起始 f = PTE_FLAG_USER；p_flags 里 PF_W 位置了就 f 再或上
 * PTE_FLAG_WRITABLE；PF_X 位置了就 f 再或上 PTE_FLAG_EXECUTABLE；返回
 * f。 */
static uint32_t flags_of(uint32_t p_flags)
{
    (void)p_flags;
    return PTE_FLAG_USER;
}

/* TODO 5：实现 load_segment(p, inum, ph)。
 *
 * 加载一个 PT_LOAD 段：逐页分配物理内存、填内容、建立映射。在提交线
 * 之后调用，不允许失败——分配不到内存只能 panic（模式已经在 build_stack
 * 的注释里出现过一次，这里同理：`if (page_phys == NULL) panic(...)`）。
 *
 * 对 off 从 0 开始、每次加 PAGE_SIZE，直到 off >= ph->p_memsz 为止的每一
 * 页，依次做：
 *
 *   1. vaddr = ph->p_vaddr + off。
 *   2. kalloc_page() 拿一个新的物理页 page_phys；失败就 panic。
 *      page_kva = page_phys + KERNEL_VIRT_BASE（内核能解引用的虚拟地址）。
 *   3. memset(page_kva, 0, PAGE_SIZE)——整页清零。
 *   4. 如果 off < ph->p_filesz（这一页落在文件内容范围内），从文件读入
 *      重叠的那部分：remain = ph->p_filesz - off；copy_len = min(remain,
 *      PAGE_SIZE)；调用 read_exact(inum, ph->p_offset + off, page_kva,
 *      copy_len)，失败就 panic（校验阶段本该拦住这种情况）。
 *      如果 off >= ph->p_filesz，这一步整个跳过，页保持全零（.bss）。
 *   5. pagetable_map(p->pagetable, vaddr, page_phys, flags)，flags 由
 *      flags_of(ph->p_flags) 算出（在循环外算一次即可，值不随 off 变)。
 *   6. uvm_track(p, vaddr, flags)——把这个映射记进 p 的 upages[] 账本,
 *      exec 失败重来或者进程退出时靠这张账本知道要释放哪些页。
 *
 * 这三步（清零、按需拷贝、建映射）覆盖了 filesz/memsz 的全部三种形状,
 * 不需要为它们分别写分支：filesz == memsz 的段每页都走完整的清零+拷贝；
 * filesz == 0 的纯 .bss 段每页都只走清零（拷贝步骤的 copy_len 算出来是
 * 0，自然跳过）；0 < filesz < memsz 的段前面几页走清零+拷贝、后面几页
 * 只走清零，而跨界那一页走清零 + 部分拷贝。三种形状是同一段代码的三种
 * 取值，不是三条路径——这正是"先清零再覆盖"这个顺序的价值所在。
 *
 * 为什么先 memset 再 fs_read 而不是反过来：kalloc_page() 明确不保证
 * 返回零页（kalloc.h 里写了），拿到的页里是上一个使用者留下的内容。
 * 不清零的症状是"未初始化的全局变量里有垃圾"，而垃圾的具体内容取决于
 * 之前哪个进程用过这一页——同一个程序两次运行表现不同。 */
static void load_segment(struct proc *p, uint32_t inum,
                         const struct Elf64_Phdr *ph)
{
    (void)p;
    (void)inum;
    (void)ph;
    panic("exec: load_segment: TODO 5 未实现");
}

/* TODO 6：实现 build_stack(p)。
 *
 * 建立用户栈，并把 stage_argv() 暂存好的 argv 按 System V 的初始栈布局
 * 压上去。返回新进程的初始栈指针——也就是 crt0 的 _start 执行第一条指令
 * 时 rsp/sp 应该等于的值。在提交线之后调用，不允许失败。
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
 * 地址：先把字符串摆好，它们的用户态地址就定了，然后才能填指针。
 *
 * ── 步骤 ──────────────────────────────────────────────────────────
 *
 *   1. 分配并映射 USER_STACK_PAGES 页栈内存：stack_bottom = USER_STACK_TOP
 *      - USER_STACK_PAGES*PAGE_SIZE；对 i 从 0 到 USER_STACK_PAGES-1，
 *      vaddr = stack_bottom + i*PAGE_SIZE，kalloc_page() 拿物理页（失败
 *      panic），memset 清零，pagetable_map(p->pagetable, vaddr, phys,
 *      PTE_FLAG_USER | PTE_FLAG_WRITABLE)，然后 uvm_track(p, vaddr, flags)。
 *
 *   2. 往刚映射好的栈页里写数据时，不能直接把用户虚拟地址当指针解引用——
 *      当前页表根未必已经切到 p（exec 可能不是对"当前进程"调用的）。要写
 *      某个用户虚拟地址 va，统一走："pagetable_lookup(p->pagetable, va 向
 *      下取整到页) 查出物理页 page；实际写地址 = page + KERNEL_VIRT_BASE +
 *      (va 的页内偏移)"。查不到（page == 0）就 panic。
 *
 *   3. 字符串区：sp = USER_STACK_TOP - ubytes（ubytes 是 stage_argv 记录
 *      的实际字节数，不是 UARGBYTES 上限）。argv_strings_base = sp。
 *      对 i 从 0 到 ubytes-1，把 ubuf[i] 按步骤 2 的方法写到用户虚拟地址
 *      argv_strings_base + i。
 *
 *      每个字符串在用户态的地址就是 argv_strings_base + uoff[i]，因为
 *      ubuf 里的相对布局被原样搬到了栈上——这是用下标而不是指针暂存的
 *      回报：一次整体平移就完成了"内核缓冲区坐标"到"用户栈坐标"的换算。
 *
 *   4. 指针数组 + argc：一共 nslots = 1 + unarg + 1 个 8 字节格子
 *      （argc、argv[0..unarg-1]、结尾 NULL）。sp -= nslots * 8；然后
 *      sp &= ~15（16 字节对齐，见下面"对齐"一节）。对 i 从 0 到 nslots-1，
 *      按步骤 2 的方法把对应的值写到用户虚拟地址 sp + i*8：i==0 写
 *      unarg（这就是 argc）；0 < i < nslots-1 写
 *      argv_strings_base + uoff[i-1]（这是 argv[i-1]）；i == nslots-1
 *      写 0（argv[argc] 的 NULL 哨兵）。
 *
 *   5. 返回 sp。
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
    (void)p;
    panic("exec: build_stack: TODO 6 未实现");
}

/* 把 n 向上取整到页边界。已经写好，不是 TODO——纯算术，没有可练习的
 * 逻辑，check_phdr/load_segment 的 TODO 说明里已经把"为什么要按页处理"
 * 讲清楚了，这里只是那个逻辑用到的一个工具函数。 */
static uint64_t page_round_up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* TODO 7：实现 exec_load(p, path, argv, entry_out, sp_out)。
 *
 * 这是把上面六个 TODO 串起来的主流程，也是全文件里"提交线"这个设计
 * 真正落地的地方（完整论证见文件顶部）。返回 0 表示加载成功、entry_out/
 * sp_out 已经填好新程序的入口和初始栈指针；返回 -1 表示加载失败、
 * *p 的地址空间完全没有被改动过，调用者可以继续使用它原来的地址空间。
 *
 * ── 提交线之前：只读校验，任何一步失败都直接 return -1 ─────────────
 *
 *   1. fs_lookup(path) 找到 inum；找不到就 kprintf 说明、返回 -1
 *      （这是最常见的失败——shell 打错命令名就会走到这里）。
 *   2. fs_stat(inum, &file_size)（或本课程 fs.h 里对应的取文件大小的
 *      接口）拿到文件字节数，后面 check_phdr 要用。
 *   3. struct Elf64_Ehdr ehdr; read_exact(inum, 0, &ehdr, sizeof(ehdr))
 *      读文件头；读不满或者 check_ehdr(&ehdr, path) 不通过，返回 -1。
 *   4. total_pages = 0（一个局部累加器，见下面"整体页数预算"）。
 *   5. 对 i 从 0 到 ehdr.e_phnum - 1：
 *        a. struct Elf64_Phdr ph; 用 read_exact 从
 *           ehdr.e_phoff + i * ehdr.e_phentsize 读一个 phdr。
 *        b. ph.p_type != PT_LOAD 的段直接跳过本轮（PT_NOTE/PT_GNU_STACK
 *           这类段不需要加载，check_phdr 也没为它们设计）。
 *        c. check_phdr(&ph, path, file_size) 不通过，返回 -1。
 *        d. total_pages += page_round_up(ph.p_memsz) / PAGE_SIZE。
 *        e. total_pages + USER_STACK_PAGES 超过 NUSERPAGE，kprintf 说明
 *           "程序加起来页数太多"，返回 -1——这正是 check_phdr 那段
 *           TODO 说明里提到的"整体性质，单个段查不出来"的那条检查,
 *           必须在这个累加循环里做。
 *   6. stage_argv(argv) 失败（参数太多/太长），返回 -1。
 *
 * 走到这里，说明这个文件、这些段、这些参数全部合法——接下来的每一步
 * 都不会因为"用户给的数据有问题"而失败，只会因为"物理内存不够"而失败,
 * 而后者按本课程的约定是 panic 的资格（结构性资源耗尽，不是可恢复的
 * 用户错误）。
 *
 * ── 提交线：这一行之后不允许再 return 负数 ─────────────────────────
 *
 *   7. uvm_clear(p)——释放 *p 当前地址空间里的每一页（旧程序的代码、
 *      数据、栈），并把 p->upages/nupages 清空。这一步开始，"exec 前的
 *      那个地址空间"就不存在了。
 *
 * ── 提交线之后：重建，不允许失败 ──────────────────────────────────
 *
 *   8. 再走一遍 phdr 表（这是本函数唯一一处 program header 要读两遍的
 *      地方——第一遍在提交线前只读校验，不产生副作用；第二遍在提交线后
 *      真正加载。两遍分开、不合并成一遍，正是"验证"和"提交"分离这个
 *      设计的直接体现，参见文件顶部说明）。对每个 PT_LOAD 段调用
 *      load_segment(p, inum, &ph)。
 *   9. entry = build_stack(p) 建栈、压 argv，entry 变量名借用一下，
 *      实际存的是 sp——*sp_out = entry。
 *  10. *entry_out = ehdr.e_entry（ELF 头里记录的程序入口地址，不需要
 *      算，直接是 program header 表描述的某个地址）。
 *  11. 返回 0。
 *
 * 调用者（sys_exec，在 trap.c 里，属于另一组 TODO）拿到 0 之后要做的
 * 事：把 entry_out/sp_out 写进 trapframe 对应的 pc/sp 字段，让这次
 * ecall/syscall 的"返回"落进新程序的第一条指令,而不是原来 exec()
 * 调用之后的下一条指令——这正是 exec 这个系统调用"一去不回"的实现
 * 方式:它不是真的不返回，是返回到了一个被偷换过的位置。 */
int exec_load(struct proc *p, const char *path, char *const argv[],
              uintptr_t *entry_out, uintptr_t *sp_out)
{
    (void)p;
    (void)path;
    (void)argv;
    (void)entry_out;
    (void)sp_out;
    return -1;
}
