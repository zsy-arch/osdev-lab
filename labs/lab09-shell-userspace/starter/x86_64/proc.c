/* Lab7 x86_64：进程表 + 调度器 + fork/exec/wait/exit 的具体实现。
 *
 * 本文件把 proc.h 里定义的数据结构和 swtch.S/trap_entry.S 里的机制
 * 串起来，是本 Lab 真正"看得出多进程轮转调度"的地方。核心循环只有
 * 一句话：scheduler() 永久轮询 proc_table，找到一个 RUNNABLE 的进程
 * 就 swtch() 过去，等它（通过 yield()，即被定时器打断）swtch() 回来，
 * 再继续找下一个——这是最朴素的轮转（round-robin）调度，没有优先级、
 * 没有时间片长度的精细控制（时间片长度完全由 pit.c 的 PIT 频率决定，
 * 本 Lab 不额外调整）。
 *
 * 关键简化（ROADMAP 明确认可的教学取向,不是遗漏)：
 *   - exec 不是"加载任意程序"，是"把调用者自己的地址空间重新映射成
 *     内核镜像里那份 user_prog.bin 的一份新拷贝，重置 trapframe 到
 *     入口点"——本课程没有文件系统、没有 ELF 加载器（Lab8/Lab9 才有），
 *     "exec 一个不同的程序"这个 Unix 语义在本 Lab 的范围内无法真正
 *     表达，只能演示"exec 这个系统调用存在、会重置地址空间和执行流"
 *     这个机制本身。
 *   - fork 出的子进程页表不是"写时复制"（copy-on-write，真实内核的
 *     标准做法),是立即完整复制每一个已映射页的内容到新分配的物理页——
 *     COW 需要页表项里的"只读+缺页时复制"这套机制，属于本课程范围
 *     之外的优化，教学取向选择最直观但更慢的"fork 就是复制一份"。
 */
#include "types.h"
#include "proc.h"
#include "pipe.h"   /* Lab9: pipe_dup/pipe_close——fork 继承和 exit 释放管道端 */
#include "kalloc.h"
#include "pagetable.h"
#include "panic.h"
#include "console.h"
#include "string.h"

struct proc proc_table[NPROC];

/* 下一个分配的 pid，从 1 开始（0 留作"无效/未分配"的哨兵值，跟
 * kalloc 系统"第 0 物理页不可分配"是同一种"0 表示无效"的设计取向）。
 * 本 Lab 教学范围内 pid 只增不减、不回收（NPROC 只有 4，不会真的
 * 溢出到需要回收的地步），真实内核会有更复杂的 pid 分配/回收策略。 */
static int g_next_pid = 1;

/* 当前正在 RUNNING 的进程——scheduler()/yield()/sys_exit_proc() 等
 * 都需要知道"现在是谁在跑"，用一个模块内全局变量记录，而不是每次都
 * 遍历 proc_table 找 state==PROC_RUNNING 的那个（找到的应该只有一个，
 * 但没必要每次都扫一遍表来确认这件事)。scheduler() 进入某个进程之前
 * 设置它，那个进程通过 yield() 让出/或者 sys_exit_proc() 退出时清空。 */
static struct proc *g_current;

extern void swtch(struct context **old, struct context *new);
extern void trap_return(void);
extern void tss_set_rsp0(uintptr_t rsp0);
extern void syscall_set_kernel_rsp(uintptr_t rsp);

/* 跟 kernel_main.c/pagetable.c 同名常量必须保持一致（本课程一贯的
 * "没有单一数据源、需要人肉对齐"的手工契约，见那两个文件里对应的
 * 注释）——这里需要它是因为 kalloc_page() 返回的是物理地址，本文件
 * 需要先加上这个偏移才能把它当指针解引用（memcpy 用户页内容）。 */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull

/* Lab9：初始进程的程序路径。
 *
 * Lab7/Lab8 的初始进程来自内嵌在内核镜像里的一段二进制（user_blob.S 把
 * user_prog.bin 整个塞进 .rodata，proc.c 里 memcpy 到一页用户内存）。
 * Lab9 有了 ELF 加载器和文件系统，初始进程改成从磁盘加载 /init——这不只
 * 是"换个来源"，它去掉了本课程里最后一处"内核镜像里带着用户程序"的耦合。
 * 从这个 Lab 起，内核和用户程序是两个独立的产物，改用户程序不需要重新
 * 链接内核。
 *
 * 加载失败就 panic：这是启动路径，没有 /init 意味着根文件系统不对
 * （mkfs 没跑、fs.img 没挂上、或者 initrd 里漏了这个文件），继续跑下去
 * 没有任何意义。真实内核在这一步的行为完全一样，Linux 的那句
 * "No init found. Try passing init= option to kernel" 就是这个 panic。 */
#define INIT_PATH "/init"

void proc_init(void)
{
    for (int i = 0; i < NPROC; i++) {
        proc_table[i].state = PROC_UNUSED;
    }
    g_current = NULL;
}

struct proc *proc_current(void)
{
    return g_current;
}

/* ══════════ Lab9：用户地址空间的三个操作 ══════════
 *
 * 这一节替换掉了 Lab7/Lab8 的 map_user_prog() 和 fork_copy_page()。那两个
 * 函数里"用户地址空间 = USER_PROG_VADDR 那一页 + USER_STACK_VADDR 那一页"
 * 是写死的，一共两个常量、四处引用。Lab9 的地址空间由 ELF 文件决定，页数
 * 和虚拟地址都是运行时才知道的，那个写法没法继续。
 *
 * 换成 upages[] 之后，"这个进程有哪些用户页"只有一个来源，exec/fork/exit
 * 三条路径都只读这张表。接口设计见 proc.h 里这三个函数的声明处。 */

/* TODO 1（Lab9 新增，proc.h 已经声明）：记下"刚刚给这个进程映射了一页
 * 用户内存"——把 vaddr/flags 追加进 p->upages[]，p->nupages 自增。
 *
 * 这是 exec/fork/uvm_clear 三条路径唯一共同的数据来源：以前 map_user_
 * prog()/fork_copy_page() 靠两个写死的常量（USER_PROG_VADDR/USER_STACK_
 * VADDR）知道"这个进程有哪些用户页"，ELF 加载出来的页数和地址都是运行时
 * 才知道的，必须有一个显式记录。
 *
 * 溢出检查不能省：p->nupages 达到 NUSERPAGE 时不能再往 upages[] 里写，
 * 否则会写出数组边界。调用点都在 exec 的提交线之后，没有退路，只能
 * panic——exec_load() 已经在提交线*之前*把总页数跟 NUSERPAGE 比过了，
 * 真的走到这里说明那个预算算错了（比如 page_round_up() 少算一页，或者
 * 漏算了栈），是内核自己的 bug，不是用户程序能触发的情况。
 *
 * 提示：
 * if (p->nupages >= NUSERPAGE) {
 *     panic("uvm_track: ...");
 * }
 * p->upages[p->nupages].vaddr = vaddr;
 * p->upages[p->nupages].flags = flags;
 * p->nupages++;
 */
void uvm_track(struct proc *p, uintptr_t vaddr, uint32_t flags)
{
    (void)p;
    (void)vaddr;
    (void)flags;
    panic("uvm_track: TODO 1 未实现");
}

/* TODO 2（Lab9 新增，proc.h 已经声明）：拆掉这个进程*全部*用户页的映射，
 * 把物理页还给 kalloc，清空 upages[]。
 *
 * 两个调用点：exec 的提交线（拆旧地址空间给新程序腾地方）和 sys_wait
 * （回收僵尸进程）。
 *
 * 只拆用户范围，不碰内核范围——调用者此刻正跑在内核栈上、用内核代码，
 * 把内核映射拆了会在下一条指令就崩。这也是为什么这个函数叫 uvm_clear
 * （user virtual memory）而不是 pagetable_clear——它清的是用户那一半。
 *
 * 不刷 TLB——两个调用点都不需要：exec 那边紧接着会 pagetable_activate()
 * （换根隐式刷全表）；sys_wait 那边被清的是僵尸进程的页表，它永远不会
 * 再被 activate，它的 TLB 项也早就在别的进程被调度进来时被换根刷掉了。
 *
 * 中间层页表节点（PML4/PDPT/PD 那几级）不释放，只释放叶子页——释放了
 * 立刻又要重新分配（exec 路径），或者交给槠位复用（sys_wait 路径，见
 * proc_alloc_skeleton()）。
 *
 * pagetable_unmap() 返回 0 表示 upages[] 记录的虚拟地址在页表里查不到
 * 映射——两者不一致，说明有人绕过 uvm_track/uvm_clear 直接改了页表，
 * 或者同一个虚拟地址被 track 了两次（第二次 unmap 就会拿到 0），直接
 * panic。
 *
 * 提示：
 * for (int i = 0; i < p->nupages; i++) {
 *     uintptr_t phys = pagetable_unmap(p->pagetable, p->upages[i].vaddr);
 *     if (phys == 0) { panic(...); }
 *     kfree_page((void *)phys);
 * }
 * p->nupages = 0;
 */
void uvm_clear(struct proc *p)
{
    (void)p;
    panic("uvm_clear: TODO 2 未实现");
}

/* 在 proc_table 里找一个 PROC_UNUSED 的槽位，把它变成一个"页表/内核栈/
 * trapframe/context 都已经就位，但用户地址空间还完全没有映射任何东西"
 * 的半成品进程——proc_alloc()（全新进程，接下来要 map_user_prog()）
 * 和 sys_fork()（子进程，接下来要逐页拷贝父进程的映射内容）共用这段
 * 骨架，区别只在"用户地址空间里具体放什么"，这一步完全相同：
 *   - 独立页表（内核范围已经从主内核页表复制过来，见 pagetable_copy_
 *     kernel_range() 的调用）；
 *   - 独立内核栈（每个进程的 trap 现场必须落在自己的栈上，不能共享，
 *     否则一个进程的 trapframe 会被另一个进程覆盖）；
 *   - 初始化好的 context.rip = trap_return，指向 swtch.S 里那段统一
 *     "resume 一个进程"入口，见 struct context/struct trapframe 的
 *     proc.h 顶部注释和 swtch.S trap_return 那段注释。
 * trapframe 本身清零但暂不填 rip/rsp/cs/ss/rflags——那五个字段依赖
 * "用户地址空间里到底放了什么"（入口地址、栈顶地址），留给调用者在
 * 映射好用户地址空间之后自己填。
 *
 * 找不到空槽位（4 个都在用）返回 NULL，调用者负责处理（本 Lab 教学
 * 范围内直接返回错误码，不做等待/抢占已有进程这类更复杂的资源回收
 * 策略）。 */
static struct proc *proc_alloc_skeleton(void)
{
    struct proc *p = NULL;
    for (int i = 0; i < NPROC; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            p = &proc_table[i];
            break;
        }
    }
    if (p == NULL) {
        return NULL;
    }

    p->pid = g_next_pid++;
    p->parent_pid = -1;
    p->exit_code = 0;
    p->name[0] = '\0';

    /* TODO 3（Lab9 新增）：三件事：
     *
     *   1. p->nupages = 0——跟下面 ofile[] 一样，槽位是复用的，上一个
     *      进程的 upages[] 还在里面。不清的话新进程会以为自己拥有前任
     *      的那些页，第一次 uvm_clear() 就会去 unmap 一堆它其实没有
     *      映射的地址。
     *   2. memset(p->ofile, 0, sizeof(p->ofile))——Lab8 就有的老需求，
     *      清空打开文件表：槽位会被复用，不清的话新进程会继承上一个
     *      进程残留的 fd，是一类典型的信息泄露。
     *   3. 页表按进程表槽位复用，只在这个槽位第一次被使用时创建：
     *      if (p->pagetable == 0) {
     *          p->pagetable = pagetable_create();
     *          pagetable_copy_kernel_range(p->pagetable);
     *      }
     *      Lab7/Lab8 每次都 pagetable_create()，sys_wait() 只释放
     *      内核栈、不释放页表，每创建一个进程就漏掉页表的那几页——那
     *      两个 Lab 里这是明确接受的简化。Lab9 不能接受了：shell 每
     *      执行一条命令就 fork 一次，漏的量由用户敲了多少条命令决定，
     *      496 页的池子撑不住。
     *
     *      能这么做的前提是 sys_wait() 在把槽位标回 UNUSED 之前调用了
     *      uvm_clear()——用户页的叶子映射全部拆掉，剩下的只有一棵
     *      "内核范围映射还在、用户范围全空"的空树，跟 pagetable_
     *      create() + pagetable_copy_kernel_range() 的产物没有区别。
     */

    /* TODO 4（Lab9 新增）：内核栈从 1 页变成 PROC_KSTACK_PAGES 页（见
     * proc.h 里那个宏的注释：exec 那条调用链比 Lab7/Lab8 深得多）。
     * 必须用 kalloc_pages() 拿*连续*的页——栈是一段连续地址，两次
     * kalloc_page() 拿到的两页没有任何理由相邻。
     *
     * 提示：
     * p->kstack_phys = kalloc_pages(PROC_KSTACK_PAGES);
     * if (p->kstack_phys == NULL) { panic(...); }
     */
    p->kstack_phys = NULL;

    /* 内核栈布局（从高地址到低地址，即从栈顶往下）：
     *   [kstack_phys + PROC_KSTACK_PAGES * PAGE_SIZE]  <- 栈顶
     *   struct trapframe                          <- p->tf 指向这里
     *   struct context（rip 字段填 trap_return）   <- p->context 指向这里
     * trap_return 执行时 %rsp 停在 context 6 个 callee-saved 寄存器
     * 被 pop 完之后的位置，正好落在 trapframe 的起始地址——这是
     * swtch.S trap_return 那段注释里说的"紧邻"关系,这里是真正把它
     * 摆出来的地方。
     *
     * p->tf/p->context 存的是*内核虚拟地址*（kstack_phys+KERNEL_VIRT_
     * BASE 之后再算的偏移），不是物理地址——p->kstack_phys 本身仍然
     * 保留物理地址（swtch()/tss_set_rsp0() 之类只需要一个数值当栈顶
     * 使用时，物理地址和虚拟地址在数值上只差一个固定偏移，用哪个都
     * 行,但只要涉及*从 C 侧解引用*（这里的 memset/写字段）,必须先加上
     * 偏移，否则复用的是 kalloc.h 顶部注释、pagetable.c walk() 注释
     * 反复强调的那个 Lab6 已经踩过的坑：pagetable_activate() 之后物理
     * 地址不再是合法的可解引用地址。
     *
     * TODO 5（Lab9 新增）：把下面这段从"1 页"改成"PROC_KSTACK_PAGES
     * 页"——只是把 PAGE_SIZE 换成 PROC_KSTACK_PAGES * PAGE_SIZE，其余
     * 逻辑完全不变。
     *
     * 提示：
     * uintptr_t kstack_kva_top = (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE +
     *                            PROC_KSTACK_PAGES * PAGE_SIZE;
     */
    uintptr_t kstack_kva_top =
        (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
    p->tf = (struct trapframe *)(kstack_kva_top - sizeof(struct trapframe));
    p->context = (struct context *)((uintptr_t)p->tf - sizeof(struct context));

    memset(p->tf, 0, sizeof(struct trapframe));
    memset(p->context, 0, sizeof(struct context));
    p->context->rip = (uint64_t)trap_return;

    return p;
}

/* 创建初始进程：骨架 + 从磁盘加载 /init。
 *
 * 本 Lab 唯一的"从无到有"的进程创建入口，只在启动时被 kernel_main.c 调用
 * 一次。sys_fork() 不用它——子进程的地址空间内容来自父进程，不是从磁盘
 * 重新加载。
 *
 * TODO 6（Lab9，body 完全重写）：
 *   1. fd 0/1/2 → 串口。三个都是 FD_CONSOLE，区别只在 writable：stdin
 *      不可写，stdout/stderr 可写。此后所有进程的 fd 0/1/2 都是从这三个
 *      表项 fork 下来的，"每个进程一出生就有标准输入/输出/错误"这件事
 *      在整个系统里只发生一次，就在这里。
 *   2. 调 exec_load(p, INIT_PATH, NULL, &entry, &sp)（argv 给 NULL：
 *      初始进程没有命令行参数，init.c 的 main() 不看 argc/argv）从
 *      文件系统加载 /init，失败就 panic。
 *   3. 用 exec_load() 填出来的 entry/sp 初始化 p->tf->rip/rsp/cs/ss/
 *      rflags（cs/ss/rflags 的常量跟 Lab7 一致：0x28|3 / 0x20|3 / 0x202）。
 *   4. p->name 填成 "init"（memcpy(p->name, "init", 5)）。
 *
 * 提示：
 * p->ofile[0].type = FD_CONSOLE; p->ofile[0].writable = 0;
 * p->ofile[1].type = FD_CONSOLE; p->ofile[1].writable = 1;
 * p->ofile[2].type = FD_CONSOLE; p->ofile[2].writable = 1;
 *
 * uintptr_t entry = 0, sp = 0;
 * if (exec_load(p, INIT_PATH, NULL, &entry, &sp) < 0) {
 *     panic("proc_alloc: 加载 " INIT_PATH " 失败——...");
 * }
 * p->tf->rip = entry;
 * p->tf->rsp = sp;
 * p->tf->cs = 0x28 | 3;
 * p->tf->ss = 0x20 | 3;
 * p->tf->rflags = 0x202;
 * memcpy(p->name, "init", 5);
 */
struct proc *proc_alloc(void)
{
    struct proc *p = proc_alloc_skeleton();
    if (p == NULL) {
        return NULL;
    }

    (void)INIT_PATH;
    panic("proc_alloc: TODO 6 未实现");

    p->state = PROC_RUNNABLE;
    return p;
}

static struct context *g_scheduler_context;

/* 调度器主循环：永久轮询 proc_table，找到一个 RUNNABLE 的就切进去。
 *
 * "轮询"而不是维护一个就绪队列——NPROC 只有 4，维护队列的复杂度收益
 * 不成比例，教学取向选最直白的写法。真实内核（比如 Linux 的 CFS）会
 * 用红黑树按虚拟运行时间排序，属于本课程范围之外的优化。
 *
 * swtch() 是唯一的"切换点"：g_scheduler_context 是调度器自己的 context
 * （在哪个栈上、哪个 rip 上恢复"回到 scheduler() 这个 for 循环"），
 * p->context 是目标进程的。swtch() 保存前者、恢复后者，函数调用约定
 * 意义上的"当前函数"就此换了身份——这行代码执行完，CPU 实际在跑的是
 * 目标进程上次被切出去时留下的状态,直到那个进程自己再 swtch() 回来，
 * scheduler() 的 for 循环才会继续往下走。
 *
 * TODO 7（Lab9，surgical，只改 kstack_kva_top 那一行）：跟
 * proc_alloc_skeleton() 里 TODO 5 完全相同的换算——内核栈从 1 页变成
 * PROC_KSTACK_PAGES 页，这里也要跟着变，否则 tss_set_rsp0()/syscall_
 * set_kernel_rsp() 设置的栈顶会比实际栈顶低了 PROC_KSTACK_PAGES-1 页，
 * 下一次这个进程从用户态陷入内核态时,内核栈会从错误的位置开始往下长，
 * 直接踩进上一个进程的内核栈内容里。
 *
 * 提示：
 * uintptr_t kstack_kva_top = (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE +
 *                            PROC_KSTACK_PAGES * PAGE_SIZE;
 */
void scheduler(void)
{
    for (;;) {
        for (int i = 0; i < NPROC; i++) {
            struct proc *p = &proc_table[i];
            if (p->state != PROC_RUNNABLE) {
                continue;
            }

            p->state = PROC_RUNNING;
            g_current = p;

            uintptr_t kstack_kva_top =
                (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
            tss_set_rsp0(kstack_kva_top);
            syscall_set_kernel_rsp(kstack_kva_top);
            pagetable_activate(p->pagetable);

            swtch(&g_scheduler_context, p->context);

            g_current = NULL;
            if (p->state == PROC_RUNNING) {
                p->state = PROC_RUNNABLE;
            }
        }
    }
}

/* 主动让出 CPU：把自己标回 RUNNABLE，swtch() 回调度器。
 *
 * 定时器中断打断一个正在跑的进程时，trap_entry.S 里的中断处理路径最终
 * 会调这个函数——本 Lab 唯一的"抢占点"。跟 sys_exit_proc() 的区别只在
 * 状态：yield() 留 RUNNABLE（还能再被调度到），sys_exit_proc() 改
 * ZOMBIE（永远不会再被调度，等父进程 sys_wait() 回收）。
 *
 * 跟 Lab7/Lab8 完全一致，没有任何改动。 */
void yield(void)
{
    struct proc *p = g_current;
    if (p == NULL) {
        return;
    }

    p->state = PROC_RUNNABLE;
    swtch(&p->context, g_scheduler_context);
}

/* 进程退出：标记 ZOMBIE，交给父进程的 sys_wait() 回收资源，自己再也
 * 不会被调度到。
 *
 * "自己不回收，交给父进程"是标准 Unix 语义（wait()/waitpid() 拿到的
 * exit code 必须来自某个地方，进程自己都没了，退出码得先存在某个还
 * 活着的实体上——这个实体就是它的 proc_table 表项本身,直到父进程读走
 * exit_code 为止,表项都不能被释放复用）。
 *
 * TODO 8（Lab9 新增）：退出时要把这个进程 ofile[] 里所有还开着的 fd
 * 关掉——POSIX 语义，进程退出隐式关闭所有 fd。Lab7/Lab8 没有这一步是
 * 因为那两个 Lab 没有管道，"退出时不关 fd"唯一会影响的资源是打开文件
 * 表项本身,而那张表在进程被回收时随着整个 proc 结构体一起消失，不需要
 * 单独处理。
 *
 * Lab9 有了管道之后不能再这么简单：管道读端靠"写端引用计数归零"判断
 * EOF（pipe_read() 的逻辑，见 pipe.c），如果写端进程退出时不主动
 * pipe_close()，读端的引用计数永远不会归零，`cat file | grep x` 这类
 * 命令里 grep 会永久阻塞在读管道上——cat 已经死了,但从管道的角度看,
 * 它的写端"还开着"，因为没人告诉管道"这个写端不再存在了"。
 *
 * 提示：
 * for (int fd = 0; fd < NOFILE; fd++) {
 *     if (p->ofile[fd].type == FD_PIPE) {
 *         pipe_close(p->ofile[fd].pipe, p->ofile[fd].writable);
 *     }
 *     p->ofile[fd].type = FD_NONE;
 * }
 */
void sys_exit_proc(int64_t code)
{
    struct proc *p = g_current;
    if (p == NULL) {
        panic("sys_exit_proc: called with no current process");
    }

    p->exit_code = code;
    p->state = PROC_ZOMBIE;

    swtch(&p->context, g_scheduler_context);

    panic("sys_exit_proc: swtch() returned into a ZOMBIE process, should be unreachable");
}

/* TODO 9（Lab9 新增，proc.h 已经声明）：把 src 的整个用户地址空间逐页
 * 复制到 dst——这是 fork() "子进程拿到父进程地址空间的一份完整拷贝"
 * 这条语义唯一的实现位置。
 *
 * 每一页：分配一块新物理页，把 src 那一页的*内容*复制过去，在 dst 的
 * 页表里用*相同的虚拟地址、相同的 flags*建立映射，再 uvm_track() 登记
 * 到 dst->upages[]。
 *
 * 复制内容不能用用户指针读写——这一步执行时 dst 的映射还没建立好、
 * 而且当前活跃的页表是 src 或者更前一个进程的，没有任何"用户指针"
 * 同时对两侧都合法。必须用内核虚拟地址（物理地址 + KERNEL_VIRT_BASE）
 * 去访问：源页是"该物理页对应的内核虚拟地址"，目的页是"新分配那块
 * 物理页对应的内核虚拟地址"——kalloc_page() 本来就固定返回物理地址，
 * 这也是为什么本文件反复出现"物理地址 + KERNEL_VIRT_BASE 才能解引用"
 * 这个模式（跟 proc_alloc_skeleton() 里 p->tf/p->context 的算法完全
 * 一样的道理）。
 *
 * src 的代码页可能以只读方式映射（如果 exec_load() 是这么设的），
 * 但这不影响这里的复制——用内核虚拟地址读取时走的是内核范围的映射
 * 权限，不受用户范围那个页表项的只读标志影响。
 *
 * kalloc_page() 失败的处理：不能把已经复制过的那几页留在 dst 里就
 * 直接返回错误——调用者 sys_fork() 后续还要继续用 dst（清理 proc_
 * table 槽位),留下一堆"部分映射、部分不映射"的用户地址空间比"完全
 * 没有映射"更危险。直接调用 uvm_clear(dst) 清掉已经复制的那些，再
 * 返回 -1，让调用者不需要关心"复制到一半失败"这种中间状态。
 *
 * 提示：
 * for (int i = 0; i < src->nupages; i++) {
 *     uintptr_t vaddr = src->upages[i].vaddr;
 *     uint32_t flags = src->upages[i].flags;
 *
 *     uintptr_t src_phys = pagetable_lookup(src->pagetable, vaddr);
 *     if (src_phys == 0) {
 *         panic("uvm_copy: upages[] 和页表不一致");
 *     }
 *
 *     void *dst_phys = kalloc_page();
 *     if (dst_phys == NULL) {
 *         uvm_clear(dst);
 *         return -1;
 *     }
 *
 *     memcpy((void *)((uintptr_t)dst_phys + KERNEL_VIRT_BASE),
 *            (void *)(src_phys + KERNEL_VIRT_BASE), PAGE_SIZE);
 *     pagetable_map(dst->pagetable, vaddr, (uintptr_t)dst_phys, flags);
 *     uvm_track(dst, vaddr, flags);
 * }
 * return 0;
 */
int uvm_copy(struct proc *dst, struct proc *src)
{
    (void)dst;
    (void)src;
    panic("uvm_copy: TODO 9 未实现");
}

/* sys_fork()：复制调用者的整个地址空间，创建一个子进程。
 *
 * TODO 10（Lab9，签名和 body 都变了）：
 *   1. uvm_copy(child, parent) 复制用户地址空间。失败时要单独释放
 *      proc_alloc_skeleton() 分配的内核栈（uvm_copy 只管用户页,不管
 *      内核栈)，再把槽位标回 UNUSED：
 *      if (uvm_copy(child, parent) < 0) {
 *          kfree_pages(child->kstack_phys, PROC_KSTACK_PAGES);
 *          child->kstack_phys = NULL;
 *          child->state = PROC_UNUSED;
 *          return -1;
 *      }
 *   2. 用 user_rip_slot/user_rsp_slot 填子进程 trapframe 的 rip/rsp，
 *      cs/ss/rflags 用跟 proc_alloc() 一样的常量：
 *      child->tf->rip = *user_rip_slot;
 *      child->tf->rsp = *user_rsp_slot;
 *      child->tf->cs = 0x28 | 3;
 *      child->tf->ss = 0x20 | 3;
 *      child->tf->rflags = 0x202;
 *
 *      ---- 这一步是 Lab9 补上的，之前完全没有，是一个真实 bug ----
 *
 *      如果不做第 3 步（下面），子进程 trapframe 的 rbp/rbx/r12-r15
 *      全部保持 proc_alloc_skeleton() memset 出来的 0。这对 Lab7/Lab8
 *      手写汇编的 user_prog.S 没问题（那段代码从不用 rbp 当帧指针），
 *      但 Lab9 引入 GCC 编译的 init.c/sh.c 之后就会炸：C 调用约定要求
 *      rbp 之类 callee-saved 寄存器跨函数调用保持不变，fork() 没理由
 *      是特例。子进程的用户栈内容是父进程栈的逐页拷贝（第 1 步），栈上
 *      数据的摆放位置是按父进程的 rbp 算出来的；子进程如果带着 rbp=0
 *      恢复执行，fork() 返回后的第一条指令（典型的是编译器生成的
 *      `mov %eax, -N(%rbp)`，把返回值存进局部变量）就会用一个跟栈内容
 *      完全对不上的地址去写——会在子进程里立刻缺页，故障地址接近 0。
 *   3. 用 gpr_snapshot 填子进程 trapframe 的 rbx/rbp/r12-r15（跟父进程
 *      恰好保持一致，让子进程"接着父进程调用 fork() 的那个栈帧"继续跑
 *      下去）：
 *      child->tf->rbx = gpr_snapshot->rbx;
 *      child->tf->rbp = gpr_snapshot->rbp;
 *      child->tf->r12 = gpr_snapshot->r12;
 *      child->tf->r13 = gpr_snapshot->r13;
 *      child->tf->r14 = gpr_snapshot->r14;
 *      child->tf->r15 = gpr_snapshot->r15;
 *
 *      不需要保存/恢复 rax/rdi/rsi/rdx/rcx/r8-r11——这些是调用约定里
 *      caller-saved 的寄存器，C 语言"调用 fork()"这件事本身就已经允许
 *      它们被破坏。
 *   4. 子进程 trapframe 里的 rax 保持 proc_alloc_skeleton() memset 出
 *      来的 0，不用再显式清一次——这正是 fork() 在子进程里返回 0、在
 *      父进程里返回子进程 pid 这条 Unix 经典语义的全部实现：父进程这次
 *      系统调用通过正常返回值机制拿到子进程 pid（本函数的返回值），
 *      子进程将来被调度器恢复、看到的 rax（fork() 的返回值寄存器）就是
 *      这里从未被写过的 0。
 *   5. Lab8 就有的 fd 表拷贝（memcpy(child->ofile, parent->ofile, ...)），
 *      加上 Lab9 新增的管道引用计数修正：memcpy 只拷了指针，没有让管道
 *      自己知道多了一个引用者，必须显式补上：
 *      memcpy(child->ofile, parent->ofile, sizeof(child->ofile));
 *      for (int fd = 0; fd < NOFILE; fd++) {
 *          if (child->ofile[fd].type == FD_PIPE) {
 *              pipe_dup(child->ofile[fd].pipe, child->ofile[fd].writable);
 *          }
 *      }
 *   6. child->parent_pid = parent->pid; child->state = PROC_RUNNABLE;
 *      return child->pid;
 *
 * 找不到空闲进程表槽位（proc_alloc_skeleton() 已经在调用前返回 NULL）
 * 返回 -1，不 panic——这是用户程序完全可能触发的正常情况（连续 fork
 * 到表满），必须能通过系统调用返回值告知调用者。 */
int sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
             struct context *gpr_snapshot)
{
    struct proc *parent = g_current;
    if (parent == NULL) {
        panic("sys_fork: called with no current process");
    }

    struct proc *child = proc_alloc_skeleton();
    if (child == NULL) {
        return -1;
    }

    (void)user_rip_slot;
    (void)user_rsp_slot;
    (void)gpr_snapshot;
    panic("sys_fork: TODO 10 未实现");
}

/* sys_exec()：把调用者的地址空间换成 path 指定的那个程序。
 *
 * TODO 11（Lab9，签名和 body 都变了）：
 *   1. path == NULL 直接返回 -1。
 *   2. 在调用 exec_load() *之前*，从 path 取 basename 存进一个内核局部
 *      变量 name_buf——不能等 exec_load() 返回之后再用 path，一旦
 *      exec_load() 越过它自己文件头注释说的"提交线"，path 指向的旧
 *      地址空间已经被拆掉，是悬空指针（这是一个真实踩过的 bug：原来的
 *      写法在 exec_load() 成功之后才用 path 取 basename，第一条命令
 *      能跑是因为凑巧没人在提交线之后碰它，下一次真正触发就是内核态
 *      page fault，故障地址落在刚刚被拆掉映射的旧地址空间里）。
 *      提示：
 *      char name_buf[sizeof(p->name)];
 *      {
 *          const char *base = path;
 *          for (const char *s = path; *s != '\0'; s++) {
 *              if (*s == '/') { base = s + 1; }
 *          }
 *          size_t i = 0;
 *          while (base[i] != '\0' && i + 1 < sizeof(name_buf)) {
 *              name_buf[i] = base[i]; i++;
 *          }
 *          name_buf[i] = '\0';
 *      }
 *   3. uintptr_t entry = 0, sp = 0;
 *      if (exec_load(p, path, argv, &entry, &sp) < 0) { return -1; }
 *      失败时地址空间没有被动过，直接返回 -1 让调用者（sh）继续跑，
 *      打"command not found"，不能杀掉整个进程。
 *   4. 把 name_buf 拷进 p->name（exec_load() 已经成功提交，p 是内核
 *      结构体，不受地址空间切换影响）。
 *   5. *user_rip_slot = entry; *user_rsp_slot = sp;——必须在 exec_load()
 *      成功*之后*才写，顺序反了会让一次失败的 exec 把返回地址指向一个
 *      不存在的入口。
 *
 * 跟 sys_fork() 一样，user_rip_slot/user_rsp_slot 是地址而不是值——
 * SYSCALL 陷入路径上没有 struct trapframe 可改，要改变"系统调用返回后
 * 从哪里继续执行"，唯一手段是改写 trap_entry.S 待会儿会读的那两个栈帧
 * 位置。 */
int sys_exec(const char *path, char *const argv[], uintptr_t *user_rip_slot,
             uintptr_t *user_rsp_slot)
{
    struct proc *p = g_current;
    if (p == NULL) {
        panic("sys_exec: called with no current process");
    }

    (void)path;
    (void)argv;
    (void)user_rip_slot;
    (void)user_rsp_slot;
    return -1;
}

/* sys_wait()：等一个子进程退出，回收它，返回它的 pid。
 *
 * TODO 12（Lab9，body 从"立即返回"变成"阻塞等待"）：
 *
 * 一、变成阻塞的。Lab7/Lab8 找不到 ZOMBIE 子进程就立刻返回 -1；Lab9
 * 必须阻塞，因为 shell 的主循环是 fork → exec → wait，如果 wait 在子
 * 进程还在跑的时候就返回，shell 会立刻打下一个提示符，上一条命令的
 * 输出还在往外冒。实现方式是 yield 循环：没找到就 yield()，被调度回来
 * 再找一遍（忙等，不是真正的睡眠/唤醒，本课程不引入等待队列）。
 *
 * 循环之前必须先确认"我到底有没有子进程"——没有子进程的话 yield 循环
 * 永远等不到东西，是死锁，不是阻塞。POSIX 在这种情况下返回 ECHILD，
 * 这里返回 -1。
 *
 * 二、回收用户页。Lab7/Lab8 只释放内核栈；Lab9 必须调用 uvm_clear(p)
 * 把用户页还回去（页表交给槽位复用），内核栈释放也要从 kfree_page()
 * 改成 kfree_pages(p->kstack_phys, PROC_KSTACK_PAGES)。
 *
 * 提示：
 * for (;;) {
 *     int have_child = 0;
 *     for (int i = 0; i < NPROC; i++) {
 *         struct proc *p = &proc_table[i];
 *         if (p->parent_pid != caller->pid || p->state == PROC_UNUSED) {
 *             continue;
 *         }
 *         have_child = 1;
 *         if (p->state != PROC_ZOMBIE) { continue; }
 *
 *         if (exit_code_out != NULL) { *exit_code_out = p->exit_code; }
 *         int pid = p->pid;
 *         uvm_clear(p);
 *         kfree_pages(p->kstack_phys, PROC_KSTACK_PAGES);
 *         p->kstack_phys = NULL;
 *         p->state = PROC_UNUSED;
 *         return pid;
 *     }
 *     if (!have_child) { return -1; }
 *     yield();
 * }
 */
int sys_wait(int64_t *exit_code_out)
{
    struct proc *caller = g_current;
    if (caller == NULL) {
        panic("sys_wait: called with no current process");
    }

    (void)exit_code_out;
    return -1;
}
