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

/* 记下"刚刚给这个进程映射了一页用户内存"。 */
void uvm_track(struct proc *p, uintptr_t vaddr, uint32_t flags)
{
    if (p->nupages >= NUSERPAGE) {
        /* 调用点都在 exec 的提交线之后，没有退路，只能 panic。但这不该
         * 发生：exec_load() 在提交线*之前*已经把总页数跟 NUSERPAGE 比过
         * 了。真的走到这里说明那个预算算错了（比如 page_round_up() 少算
         * 一页，或者漏算了栈），是内核自己的 bug，不是用户程序能触发的
         * 情况——所以 panic 的消息直接指向那个预算检查。 */
        panic("uvm_track: 用户页数超过 NUSERPAGE，exec_load() 的页数预算"
              "检查漏了什么");
    }
    p->upages[p->nupages].vaddr = vaddr;
    p->upages[p->nupages].flags = flags;
    p->nupages++;
}

/* 拆掉这个进程所有用户页的映射，把物理页还给 kalloc，清空 upages[]。
 *
 * 两个调用点：exec 的提交线（拆旧地址空间给新程序腾地方）和 sys_wait
 * （回收僵尸进程）。
 *
 * 不碰内核范围的映射。调用者此刻正跑在内核栈上、用内核代码，把内核映射
 * 拆了会在下一条指令就崩。这也是为什么这个函数叫 uvm_clear（user virtual
 * memory）而不是 pagetable_clear——它清的是用户那一半。
 *
 * 不刷 TLB。两个调用点都不需要：exec 那边紧接着会 pagetable_activate()
 * （换根隐式刷全表，见 exec.c 里那段注释）；sys_wait 那边被清的是僵尸
 * 进程的页表，它永远不会再被 activate，它的 TLB 项也早就在别的进程被
 * 调度进来时被换根刷掉了。把刷 TLB 留给调用者，是因为"什么时候刷"取决于
 * 调用者接下来要干什么，这个函数看不到。
 *
 * 中间层页表节点（PML4/PDPT/PD 那几级）不释放，只释放叶子页。对 exec
 * 那条路径这是对的——新地址空间马上要用同样的那几级节点，释放了立刻
 * 又要重新分配。对 sys_wait 那条路径这会漏几页，解决办法不是在这里递归
 * 销毁页表树，而是让进程表槽位复用页表，见 proc_alloc_skeleton()。 */
void uvm_clear(struct proc *p)
{
    for (int i = 0; i < p->nupages; i++) {
        uintptr_t phys = pagetable_unmap(p->pagetable, p->upages[i].vaddr);
        if (phys == 0) {
            /* upages[] 说这里有一页，页表说没有。两者不一致意味着有人
             * 绕过 uvm_track/uvm_clear 直接改了页表，或者同一个虚拟地址
             * 被 track 了两次（第二次 unmap 就会拿到 0）。后者正是
             * exec_load() 里"两个段不能共用同一页"那个检查在防的事。 */
            panic("uvm_clear: upages[] 里记录的虚拟地址在页表里没有映射，"
                  "两者不一致");
        }
        kfree_page((void *)phys);
    }
    p->nupages = 0;
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

    /* Lab9：用户页清单清空。跟下面 ofile[] 一样，必须显式清——槽位是
     * 复用的，上一个进程的 upages[] 还在里面。不清的话新进程会以为自己
     * 拥有前任的那些页，第一次 uvm_clear() 就会去 unmap 一堆它其实没有
     * 映射的地址。 */
    p->nupages = 0;

    /* Lab8：清空打开文件表。
     *
     * 必须显式清：proc_table 是全局数组（首次进入时确实是全 0），但槽位
     * 会被*复用*——一个进程 exit 变成 ZOMBIE、被 wait 回收成 UNUSED 之后，
     * 同一个槽位会分给下一个新进程。如果不清，新进程会继承上一个进程
     * 残留的 ofile[] 内容，凭空拿到几个"已经打开"的 fd，指向上一个进程
     * 打开过的文件。这是内核里一类非常典型的信息泄露：新进程不该看到
     * 前任留下的任何状态。 */
    memset(p->ofile, 0, sizeof(p->ofile));

    /* Lab9：页表按进程表槽位复用，只在这个槽位第一次被使用时创建。
     *
     * Lab7/Lab8 每次都 pagetable_create()，而 sys_wait() 只释放内核栈、
     * 不释放页表——每创建一个进程就漏掉页表的那几页（根 + 中间层节点）。
     * 那两个 Lab 里这是明确接受的简化：一共只 fork 几次，漏几页无所谓。
     *
     * Lab9 不能接受了，原因是 shell：它的主循环每执行一条命令就 fork 一次，
     * 用户输一百条命令就 fork 一百次。漏的量不再由内核代码决定，而是由用户
     * 敲了多少条命令决定——496 页的池子撑不住。
     *
     * 标准解法是写一个 pagetable_destroy()，递归遍历页表树把每一级节点都
     * 释放掉。这里不这么做，选了一个更简单的办法：槽位复用页表。
     *
     * 能这么做的前提是 sys_wait() 在把槽位标回 UNUSED 之前调用了
     * uvm_clear()——用户页的叶子映射全部拆掉、物理页全部还回去，剩下的
     * 只有一棵"内核范围映射还在、用户范围全空"的空树。而这正是
     * proc_alloc_skeleton() 想要的初始状态，跟 pagetable_create() +
     * pagetable_copy_kernel_range() 的产物没有区别。下一个占用这个槽位的
     * 进程直接接着用。
     *
     * 代价：页表总量被钉在 NPROC × 每棵树的节点数（约 8 × 5 = 40 页），
     * 一分配就不再归还，即使所有进程都退出了。换来的是完全不泄漏，以及
     * 不需要写那个递归销毁函数。递归销毁页表树是 README 的挑战任务之一，
     * 它真正麻烦的地方在于"哪些节点是内核范围共享的、不能释放"——
     * pagetable_copy_kernel_range() 复制的是顶级页表项本身，内核那几级
     * 节点是所有进程共享的同一份，递归下去必须认出来并跳过，否则第一个
     * 退出的进程就会把内核自己的页表节点释放掉。 */
    if (p->pagetable == 0) {
        p->pagetable = pagetable_create();
        pagetable_copy_kernel_range(p->pagetable);
    }

    /* Lab9：内核栈从 1 页变成 PROC_KSTACK_PAGES 页（见 proc.h 里那个宏
     * 的注释：exec 那条调用链比 Lab7/Lab8 深得多）。必须用 kalloc_pages()
     * 拿*连续*的页——栈是一段连续地址，两次 kalloc_page() 拿到的两页没有
     * 任何理由相邻。 */
    p->kstack_phys = kalloc_pages(PROC_KSTACK_PAGES);
    if (p->kstack_phys == NULL) {
        panic("proc_alloc_skeleton: kalloc_pages() failed for kernel stack");
    }

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
     * 地址不再是合法的可解引用地址。 */
    uintptr_t kstack_kva_top = (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE +
                               PROC_KSTACK_PAGES * PAGE_SIZE;
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
 * Lab9 的变化：原来这里调 map_user_prog() 把内嵌的 user_prog.bin 拷进
 * 一页，现在调 exec_load() 从文件系统加载 /init。fd 0/1/2 也是在这里
 * 第一次被接到串口上——此后所有进程的 fd 0/1/2 都是从这三个表项 fork
 * 下来的，"每个进程一出生就有标准输入/输出/错误"这件事在整个系统里
 * 只发生一次，就在这里。 */
struct proc *proc_alloc(void)
{
    struct proc *p = proc_alloc_skeleton();
    if (p == NULL) {
        return NULL;
    }

    /* fd 0/1/2 → 串口。三个都是 FD_CONSOLE，区别只在 writable：
     * stdin 不可写，stdout/stderr 可写。这个字段对 FD_CONSOLE 其实没
     * 被 sys_read/sys_write 检查（串口两个方向都能用），填上是为了跟
     * FD_PIPE 保持同一套语义——"一个 fd 知道自己能不能写"不该因为类型
     * 不同而有的有、有的没有。
     *
     * 为什么是 0/1/2 而不是别的号：这是 POSIX 的规定，也是所有用户程序
     * 的隐含前提。ulib.c 里 puts() 直接写 fd 1，cat 直接读 fd 0，没有
     * 任何一处先问一句"标准输出是哪个 fd"。 */
    p->ofile[0].type = FD_CONSOLE;
    p->ofile[0].writable = 0;
    p->ofile[1].type = FD_CONSOLE;
    p->ofile[1].writable = 1;
    p->ofile[2].type = FD_CONSOLE;
    p->ofile[2].writable = 1;

    uintptr_t entry = 0;
    uintptr_t sp = 0;
    /* argv 给 NULL：初始进程没有命令行参数。init.c 的 main() 不看 argc/
     * argv，crt0 会从栈上读到 argc=0。 */
    if (exec_load(p, INIT_PATH, NULL, &entry, &sp) < 0) {
        panic("proc_alloc: 加载 " INIT_PATH " 失败——根文件系统里没有这个"
              "程序，或者它不是一个本架构的静态链接 ELF");
    }

    p->tf->rip = entry;
    p->tf->rsp = sp;
    p->tf->cs = 0x28 | 3;   /* GDT_SEL_USER_CS_RPL3，见 trap.c。 */
    p->tf->ss = 0x20 | 3;   /* GDT_SEL_USER_SS_RPL3，见 trap.c。 */
    p->tf->rflags = 0x202; /* IF=1 + 保留位，跟 enter_user_mode 一致。 */

    memcpy(p->name, "init", 5);

    p->state = PROC_RUNNABLE;
    return p;
}

/* scheduler() 自己也是一条"内核执行流"，只是它从不被换出到某个进程
 * 的 proc_table 槠位里——它是唯一一条永远存在、代表"CPU 没在跑任何
 * 用户进程时应该在哪里"的流，需要一个地方保存"当从某个进程 swtch()
 * 回来之后，应该恢复到 for 循环里的哪一点"。
 *
 * 类型必须是 struct context *（跟 struct proc 里 p->context 完全
 * 一样的类型），不能是 struct context 本身——swtch() 的签名是
 * void swtch(struct context **old, struct context *new)，
 * g_scheduler_context 在两个不同调用点分别扮演 old 和 new 两种角色：
 *   - scheduler() 里 swtch(&g_scheduler_context, p->context)：
 *     g_scheduler_context 当 old，传的是 &g_scheduler_context，
 *     也就是"g_scheduler_context 这个变量本身的地址"——swtch() 要
 *     把调度器这条执行流刚保存下来的新栈顶写回*old，也就是写回
 *     g_scheduler_context 这个变量，供 yield()/sys_exit_proc() 里
 *     下次 swtch() 回调度器时用。
 *   - yield()/sys_exit_proc() 里 swtch(&p->context, g_scheduler_context)：
 *     g_scheduler_context 当 new，直接传值（不取地址）——这时它已经
 *     存了 scheduler() 上次 swtch() 出去之前保存的栈顶，swtch() 要
 *     切换过去恢复的正是这个值指向的现场。
 * 两种用法都要求 g_scheduler_context 本身的类型是 struct context *：
 * 当 old 用时取它的地址得到 struct context **；当 new 用时直接
 * 传值得到 struct context *。如果声明成 struct context（没有
 * 星号），&g_scheduler_context 就只是 struct context *，传给 old
 * 参数会被编译器直接拒绝（-Wincompatible-pointer-types）——这正是
 * 本 Lab 第一次实际编译这个文件时被 GCC 抓到的真实类型错误，不是
 * 纯粹的教学假设。 */
static struct context *g_scheduler_context;

/* scheduler()：本 Lab 唯一的"内核主循环"，noreturn——kernel_main.c
 * 里 proc_init() + 创建初始进程之后，最后一步就是调用它，此后控制权
 * 永远在"扫描 proc_table 找 RUNNABLE 的槠位、swtch() 进去、等它
 * swtch() 回来、继续扫描"这个循环里，不会再回到 kernel_main.c。
 *
 * 每次找到一个 RUNNABLE 的进程，进去之前必须：
 *   1. 把它标成 RUNNING（否则调度器下一轮扫描会把它自己也当成候选,
 *      RUNNABLE 状态本来就表示"还没在跑,可以被选中")。
 *   2. tss_set_rsp0() 指向它自己的内核栈顶——这是本文件顶部模块注释、
 *      trap.c 顶部那段 TSS 注释反复强调的关键一步：不做这一步,这个
 *      进程在用户态被定时器打断时,硬件会用*上一个*进程的 RSP0（或者
 *      压根没设过的垃圾值),直接复现本 Lab 开发过程中在 Lab6 上实测
 *      触发过的那个三重故障。传的必须是*内核虚拟地址*（跟 proc_alloc()
 *      里 p->tf/p->context 用的是同一个换算方式)，不是 kstack_phys
 *      本身——RSP0 会在中断发生的那一刻被硬件直接当成内存地址使用,
 *      而那一刻 CR3 已经是这个进程自己的页表（第 3 步换的),这份页表
 *      里能翻译的是虚拟地址,不是物理地址。
 *   3. syscall_set_kernel_rsp() 跟 tss_set_rsp0() 是同一件事的另一半——
 *      前者管的是*硬件*在特权级切换时自动加载的栈顶（IDT 中断/异常路径),
 *      后者管的是 trap_entry.S 的 syscall_entry *手动*切的栈顶（SYSCALL
 *      快速路径,x86_64 的 syscall 指令本身不会自动换栈,这是它跟中断门
 *      的根本区别，trap.c 顶部模块注释有详细说明)。本 Lab 实测调试中
 *      发现过：这一步如果漏掉，syscall_entry 会一直切到 __stack_top
 *      这个固定地址——也就是 scheduler() 自己这个函数运行、并且通过
 *      swtch() 把自己的执行状态挂起在上面的那个共享栈。只要某个进程的
 *      系统调用处理函数自己又调了 swtch()（比如 sys_exit_proc()：
 *      syscall_dispatch -> sys_exit_proc -> swtch()),这条调用链在共享栈
 *      上往下压栈的深度,一旦超过 scheduler() 挂起状态存放的位置，就会
 *      直接覆盖 scheduler() 保存的返回地址,导致之后 swtch() 切回调度器
 *      时 ret 到一个被覆盖的垃圾值——实测现象是 RIP=0/CR2=0 的取指
 *      缺页（QEMU -d int 可以直接看到 pc=0x0),不是随机崩溃,是这个
 *      具体机制的必然结果。传的值跟 tss_set_rsp0() 完全一样
 *      （kstack_kva_top),因为道理也完全一样——这个进程的系统调用处理
 *      代码需要的是*它自己*的内核栈,不是任何其它进程或者 scheduler()
 *      本身在用的栈。
 *   4. pagetable_activate() 切到它自己的页表——这个进程接下来无论是
 *      在用户态执行,还是被打断陷入内核态执行 trap 处理代码,都需要
 *      通过它自己这份页表来翻译地址（内核范围因为 pagetable_copy_
 *      kernel_range() 是共享的,能正确翻译到同一份内核代码；用户范围
 *      是这个进程独有的映射)。
 *   5. g_current 指向它,供 yield()/sys_exit_proc() 等函数使用。
 *
 * swtch(&g_scheduler_context, p->context) 换过去之后，这一行"看起来"
 * 会一直阻塞到这个进程重新把控制权交回来（通过 yield()，或者进程自己
 * 触发的下一次系统调用/异常,但本 Lab 目前只有 yield() 这一条路径会
 * 主动切回调度器)——swtch() 返回之后（也就是这个进程通过 yield() 把
 * 控制权还给调度器之后)，把它从 RUNNING 改回 RUNNABLE（如果只是被
 * 抢占；ZOMBIE 已经在 sys_exit_proc() 里设置过,这里不用管），再继续
 * for 循环找下一个。 */
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

            /* 回到这里时 g_current 未必还是 p——sys_exit_proc() 会把
             * 一个进程标成 ZOMBIE 之后直接切回调度器（走的也是这条
             * swtch() 路径,只是 exit 那一侧不会再把自己改回 RUNNABLE），
             * 这里统一用 p（这次循环迭代本来要跑的那个进程）判断，
             * 不用 g_current，避免依赖"g_current 在 swtch() 返回之后
             * 还没被其它路径改写"这个脆弱的假设。 */
            g_current = NULL;
            if (p->state == PROC_RUNNING) {
                p->state = PROC_RUNNABLE;
            }
        }
    }
}

/* yield()：一个进程主动/被动让出 CPU 的唯一入口。目前只有一个调用点
 * （pit.c 的 timer_interrupt_handler()，在定时器 tick 时抢占当前
 * RUNNING 的进程,实现"轮转调度"里"轮"这个动作的触发源），但函数本身
 * 不关心是谁调用它,只关心"当前必须存在一个 RUNNING 的进程"这个前提
 * （如果定时器 tick 落在调度器自己的 for 循环里，也就是没有任何进程
 * 在跑的时候，g_current 是 NULL，这种情况下不应该也不能 yield，见
 * timer_interrupt_handler 里的判断)。
 *
 * 只把状态改成 RUNNABLE、不改成别的——从"被打断"这件事本身，无法
 * 判断这个进程是否还应该继续跑（本 Lab 没有阻塞/睡眠语义，被打断的
 * 进程永远只是"暂时让出，下次轮到它还接着跑"），改成 RUNNABLE 之后
 * scheduler() 的下一轮扫描会重新选中它——这正是轮转调度的"轮"字。 */
void yield(void)
{
    struct proc *p = g_current;
    if (p == NULL) {
        return;
    }

    p->state = PROC_RUNNABLE;
    swtch(&p->context, g_scheduler_context);
}

/* sys_exit_proc()：SYS_EXIT 系统调用最终落到的地方（trap.c
 * syscall_dispatch 会把 SYS_EXIT 路由到这里，取代 Lab6 那个只打印
 * 退出码、不真正结束进程的占位版本）。
 *
 * 标成 ZOMBIE 而不是直接清空 PROC_UNUSED、回收资源——本 Lab 的
 * sys_wait() 需要能读到子进程的 exit_code（真实 Unix 语义：父进程
 * wait() 之前，子进程的退出状态必须保留下来，"僵尸进程"这个名字正是
 * 描述这个"已经不再执行、但资源还没被完全回收"的中间状态）。资源
 * 何时真正释放（页表/内核栈占用的物理页）是 sys_wait() 的责任，本
 * 函数不做——一个正在 exit 的进程不能自己释放自己正在使用的内核栈
 * （它接下来还要靠这个栈执行 swtch() 切走），必须交给别人（父进程
 * wait 的时候，或者本课程教学范围内更简单的处理方式）在这个进程
 * 自己的执行流已经彻底停止之后才能做。
 *
 * exit 之后不会返回调用者（跟真实 Unix 的 exit() 语义一致——它是
 * noreturn 的，但这里没有标 __attribute__((noreturn))，因为调用点
 * syscall_dispatch 仍然是一个走到底会 return 的函数，形式上允许
 * "调用了 sys_exit_proc 之后接着走"，只是实际执行不会到达那里，
 * proc.h 里也没有把这个函数声明成 noreturn——这是教学取向的选择：
 * 精确的 noreturn 标注需要调用点配合处理"之后的代码永远不会执行"，
 * 本 Lab 认为这个精确性不值得为此改变 syscall_dispatch 的控制流
 * 结构，直接让 scheduler() 的 for 循环重新拿到控制权即可）。 */
void sys_exit_proc(int64_t code)
{
    struct proc *p = g_current;
    if (p == NULL) {
        panic("sys_exit_proc: called with no current process");
    }

    /* Lab9：进程退出时，它打开的管道端必须被释放。
     *
     * 这不是"顺手清理一下资源"这种可有可无的礼貌，它是管道语义的必要组成
     * 部分。`cat /motd.txt | grep lab` 里 grep 的 read 什么时候返回 EOF？
     * 等到写端计数归零。而 cat 从来不显式 close 自己的 fd 1——它写完就
     * exit 了，绝大多数 Unix 程序都这样。如果内核不在这里替它关，写端计数
     * 永远不会归零，grep 永远挂在 read 上，整条管道命令卡死。
     *
     * 也就是说 exit 隐含了"关闭所有 fd"。POSIX 明确规定了这一点，而它真正
     * 的分量在于：进程退出是一个*可以到处发生*的事件（正常返回、调用 exit、
     * 将来还有被信号杀死），所以资源释放必须挂在这个统一的出口上，不能
     * 指望每个程序自己记得关。
     *
     * 只处理 FD_PIPE，理由跟 sys_fork 里那个循环一样：另两种类型在本 Lab
     * 没有需要释放的东西。
     *
     * 位置在 swtch() *之前*——swtch() 之后这个函数再也不会被执行到。 */
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd].type == FD_PIPE) {
            pipe_close(p->ofile[fd].pipe, p->ofile[fd].writable);
        }
        /* 整张表清空。ZOMBIE 进程的 fd 表没人会再看，清它是为了
         * proc_alloc() 复用这个槽位时不必依赖"上一个进程清干净了"——
         * 不过那边的 memset 也做了同一件事，两处都做是刻意的冗余：
         * 释放资源的地方把状态归零，分配资源的地方也不假定拿到的是
         * 干净的。 */
        p->ofile[fd].type = FD_NONE;
        p->ofile[fd].inum = 0;
        p->ofile[fd].off = 0;
        p->ofile[fd].pipe = NULL;
        p->ofile[fd].writable = 0;
    }

    p->exit_code = code;
    p->state = PROC_ZOMBIE;

    swtch(&p->context, g_scheduler_context);

    panic("sys_exit_proc: swtch() returned into a ZOMBIE process, should be unreachable");
}

/* 把 src 的每一个用户页复制一份给 dst：新分配物理页、整页拷内容、用
 * *相同的虚拟地址和相同的权限*建立映射。成功返回 0，物理内存不够返回 -1。
 *
 * "相同的虚拟地址"是这个简化 fork 能工作的关键：子进程的 trapframe
 * （sys_fork() 里整份拷贝自父进程）里 rip/rsp 存的还是父进程的虚拟地址
 * 值，如果子进程页表把这些内容映射到别的虚拟地址，子进程恢复执行时会
 * 在一个自己完全没有内容的地址上取指。
 *
 * "相同的权限"同样不能含糊：代码段只读可执行、数据段可写不可执行。抄错
 * 的两种后果分别是"子进程能改自己的代码段"（安全边界丢了）和"子进程一
 * 写全局变量就崩"。flags 是 uvm_track() 当初记下来的，这里原样用。
 *
 * 内容通过内核偏移映射拷，不通过用户虚拟地址——两个原因。一是此刻活跃
 * 的页表是父进程的，子进程那些新映射还没生效，写不进去。二是就算能写，
 * 子进程的代码段页是只读的，通过用户映射写会触发缺页。内核偏移映射绕过
 * 这两个问题：物理页在内核眼里永远是可读可写的普通内存。
 *
 * 分配失败时先把 dst 已经拷好的页清掉再返回 -1。这让调用者（sys_fork）
 * 的错误处理是一句 return -1，不需要自己收拾半成品——"谁制造的中间状态
 * 谁负责清理"，否则 fork 失败会留下一个页表里挂着几个页、upages[] 里
 * 记着这几个页、但进程表槽位即将被标回 UNUSED 的进程，那几页就永久漏了。 */
int uvm_copy(struct proc *dst, struct proc *src)
{
    for (int i = 0; i < src->nupages; i++) {
        uintptr_t vaddr = src->upages[i].vaddr;
        uint32_t flags = src->upages[i].flags;

        uintptr_t src_phys = pagetable_lookup(src->pagetable, vaddr);
        if (src_phys == 0) {
            /* 跟 uvm_clear() 里同一个不变量：upages[] 和页表必须一致。 */
            panic("uvm_copy: 源进程 upages[] 里记录的虚拟地址在页表里没有"
                  "映射，两者不一致");
        }

        void *dst_phys = kalloc_page();
        if (dst_phys == NULL) {
            uvm_clear(dst);
            return -1;
        }

        memcpy((void *)((uintptr_t)dst_phys + KERNEL_VIRT_BASE),
               (void *)(src_phys + KERNEL_VIRT_BASE), PAGE_SIZE);

        pagetable_map(dst->pagetable, vaddr, (uintptr_t)dst_phys, flags);
        uvm_track(dst, vaddr, flags);
    }
    return 0;
}

/* sys_fork()：user_rip_slot/user_rsp_slot 是"父进程这次 SYSCALL 返回后
 * 该恢复到哪个 rip/用哪个用户栈指针"这两个值的*地址*——在 SYSCALL 这
 * 条陷入路径上,它们分别是 syscall_entry（trap_entry.S）栈帧里被 push
 * 过的 rcx 槽位,以及同一个栈帧里被 push 过的用户 RSP 槽位,不是
 * proc.h 意义上的 struct trapframe（那是 IDT 路径专用的现场格式,
 * SYSCALL 完全不碰 p->tf,见 proc.h 里这两个函数声明处的大注释)。
 *
 * gpr_snapshot 是 Lab9 为了修一个真实 bug 才加的第三个参数,指向
 * syscall_entry 在换栈之后立刻存下的一份 rbx/rbp/r12-r15 快照——这
 * 是父进程从用户态陷入这一刻,这六个 callee-saved 寄存器*原本*的值。
 *
 * 子进程通过 proc_alloc_skeleton() 拿到一份骨架（独立页表/内核栈/
 * 待填的 trapframe），然后：
 *   1. 逐页拷贝父进程已映射的两页（程序页+栈页）到子进程自己的物理页,
 *      映射到子进程页表里*同样的虚拟地址*——这就是"fork 出的子进程
 *      拥有和父进程一样的地址空间内容"这个语义在本 Lab 简化范围内的
 *      落地方式（完整拷贝，不是 COW，见本文件顶部模块注释）。
 *   2. 用 *user_rip_slot 和 *user_rsp_slot 填子进程 trapframe 的 rip/rsp
 *      ——子进程"从 fork() 系统调用返回"这个动作,本质上就是"在跟父
 *      进程这次系统调用一样的 rip/rsp 现场下,走一遍 trap_return→
 *      sysretq 的返回路径"。cs/ss/rflags 用跟 proc_alloc() 里一样的
 *      常量（子进程终归要以 ring3 身份恢复执行,这三个字段的值不依赖
 *      父进程具体是谁,是"回到用户态"这件事本身固定的)。
 *   3. 用 gpr_snapshot 填子进程 trapframe 的 rbp/rbx/r12-r15。
 *
 *      ---- 这一步是 Lab9 补上的，之前完全没有，是一个真实 bug ----
 *
 *      旧版本这里没有这一步，子进程 trapframe 的 rbp/rbx/r12-r15 全部
 *      保持 proc_alloc_skeleton() memset 出来的 0。旧注释曾经写"本课程
 *      的用户态代码只依赖 rax/rdi/rsi 在系统调用前后保持约定关系,不
 *      需要完整寄存器现场"——这对 Lab7/Lab8 手写汇编的 user_prog.S 是
 *      对的（那段代码从不用 rbp 当帧指针），但从 Lab9 引入 GCC 编译的
 *      init.c/sh.c 开始就不成立了：C 调用约定要求 rbp 之类的 callee-
 *      saved 寄存器跨越任何函数调用保持不变，fork() 没有理由是特例。
 *      子进程的用户栈内容是父进程栈的逐页拷贝（上面第 1 步），栈上数据
 *      的摆放位置是按父进程的 rbp 算出来的；子进程如果带着 rbp=0 恢复
 *      执行，fork() 返回后的第一条指令（典型的是编译器生成的
 *      `mov %eax, -N(%rbp)`,把返回值存进局部变量)就会用一个跟栈内容
 *      完全对不上的地址去写——已经在 QEMU+GDB 下复现过：/init 第一次
 *      fork() 之后子进程立刻缺页，故障地址 0xfffffffffffffffc，正好是
 *      0 - 4（-N 这里的 N 具体是 4）。完整的复现过程和根因推导见
 *      trap_entry.S 顶部那段"Lab9 修复：fork() 丢失父进程 callee-saved
 *      寄存器的 bug"模块注释。
 *
 *      没有一并保存/恢复 rax/rdi/rsi/rdx/rcx/r8-r11：这些是调用约定里
 *      caller-saved 的寄存器,C 语言"调用 fork()"这件事本身就已经允许
 *      它们被破坏,子进程作为"fork() 的另一种返回方式"遵守同一条约定
 *      即可（真实 Linux 的 fork() 语义也是如此,只有 rax/返回值有意义)。
 *   4. 子进程 trapframe 里的 rax 保持 proc_alloc_skeleton() memset 出
 *      来的 0,不用像旧设计那样再显式清一次——这正是 fork() 在子进程
 *      里返回 0、在父进程里返回子进程 pid 这条 Unix 经典语义的全部
 *      实现：父进程这次系统调用本身通过正常的 syscall_dispatch 返回值
 *      机制拿到子进程 pid（本函数的返回值),子进程将来被调度器 swtch()
 *      进去、trap_return 恢复这份 trapframe、sysretq 回到用户态时,
 *      看到的 rax（fork() 的返回值寄存器）就是这里从未被写过的 0。
 *   5. parent_pid 记录血缘关系,state 设成 RUNNABLE 交给调度器。
 *
 * 找不到空闲进程表槽位（NPROC 个槽位全在用）返回 -1，不 panic——这
 * 是用户程序完全可能触发的正常情况（连续 fork 到表满),必须能通过
 * 系统调用返回值告知调用者,不是内部不变量被破坏。 */
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

    /* Lab9：复制父进程的*全部*用户页，页数和虚拟地址都从父进程的
     * upages[] 来。Lab7/Lab8 这里是两行写死的 fork_copy_page()，
     * 对应"程序页 + 栈页"那个固定布局。
     *
     * uvm_copy() 失败（物理内存不够）时它自己已经把已拷好的用户页清掉
     * 了，但 proc_alloc_skeleton() 分配的*内核栈*还在——那不是 uvm_copy
     * 的东西，它不该碰。所以这里要还：内核栈释放掉，槽位标回 UNUSED。
     *
     * 漏掉 kfree_pages() 的后果不是"少了几页"这么简单：这个槽位下次被
     * proc_alloc_skeleton() 用到时会分配一个新内核栈、直接覆盖
     * p->kstack_phys，旧的那段连续页再也没有任何指针指向它，彻底漏掉。
     * 而"fork 因为内存不够而失败"这件事本身就发生在内存紧张的时候，每
     * 失败一次再漏两页，会把系统推向更紧张的状态。这类"错误处理路径
     * 自己制造资源泄漏"的 bug 在真实内核里是很常见的一类，因为错误
     * 路径几乎从不被测试到。
     *
     * 页表不释放，留给槽位复用（见 proc_alloc_skeleton() 那段注释）。 */
    if (uvm_copy(child, parent) < 0) {
        kfree_pages(child->kstack_phys, PROC_KSTACK_PAGES);
        child->kstack_phys = NULL;
        child->state = PROC_UNUSED;
        return -1;
    }

    child->tf->rip = *user_rip_slot;
    child->tf->rsp = *user_rsp_slot;
    child->tf->cs = 0x28 | 3;   /* GDT_SEL_USER_CS_RPL3，见 trap.c。 */
    child->tf->ss = 0x20 | 3;   /* GDT_SEL_USER_SS_RPL3，见 trap.c。 */
    child->tf->rflags = 0x202; /* IF=1 + 保留位，跟 proc_alloc() 一致。 */

    /* Lab9 修复 fork() 寄存器丢失 bug 的核心一步：见上方函数头注释
     * "这一步是 Lab9 补上的"那一段，以及 trap_entry.S 顶部的完整推导。
     * gpr_snapshot 的字段顺序跟 struct context 声明一致（rbx/rbp/
     * r12-r15），直接逐字段拷进子进程 trapframe 对应的字段——两个
     * 结构体字段名恰好相同，不存在顺序换算。 */
    child->tf->rbx = gpr_snapshot->rbx;
    child->tf->rbp = gpr_snapshot->rbp;
    child->tf->r12 = gpr_snapshot->r12;
    child->tf->r13 = gpr_snapshot->r13;
    child->tf->r14 = gpr_snapshot->r14;
    child->tf->r15 = gpr_snapshot->r15;

    /* Lab8：把父进程的打开文件表整份拷给子进程。
     *
     * 这一行就是 fork() 的"文件描述符继承"语义——子进程一出生就拥有跟
     * 父进程一样的 fd 集合，fd 号也一样。shell 的重定向就靠这个：父进程
     * fork 之后、exec 之前把 fd 换掉，新程序启动时看到的就是换过的 fd。
     *
     * 但本 Lab 的继承只是*拷贝*，不是真正的 Unix 语义。真正的 fork()
     * 让父子共享同一个打开文件（共享偏移）：父进程读了 10 字节，子进程
     * 接着读会从第 10 字节开始。这里因为 struct file 直接嵌在 PCB 里，
     * 拷贝之后两份偏移彼此独立，父子各从自己的位置读。
     *
     * 差别是能观察到的，不是理论上的：Lab8 的挑战任务之一就是写一个
     * "父进程读一半、fork、看子进程从哪里继续读"的程序，先在当前实现上
     * 看到"子进程重新从一半的位置读"（拷贝语义），再改成全局 file 表 +
     * 引用计数，看到"子进程接着往下读"（共享语义）。共享语义需要在
     * sys_exit_proc() 里递减引用计数，这是它真正麻烦的地方——"谁来关
     * 最后一个引用"这个问题会一路牵扯到进程退出路径。 */
    memcpy(child->ofile, parent->ofile, sizeof(child->ofile));

    /* Lab9：管道是上面那个"拷贝语义"的例外，而且必须是例外。
     *
     * memcpy 把 pipe 指针也拷过去了，父子于是指向*同一个* struct pipe——
     * 这正是需要的，管道的全部意义就是两个进程共享一个缓冲区。于是本 Lab
     * 的 fd 表是半共享的：inum/off 各一份，管道本体共享（见 proc.h 里
     * struct file 的注释）。
     *
     * 但引用计数不会自己跟着涨。memcpy 只复制了"指向管道的指针"，管道
     * 自己并不知道多了一个用户。必须在这里显式加——漏掉的症状很隐蔽：
     * 代码看着已经对了（子进程确实能读写那个管道），直到子进程 close，
     * 计数被减到比实际引用数还少，父进程手上那一端突然看到 EOF 或者 -1。
     * 出错的地方离原因很远，是本 Lab 最难查的一类 bug。
     *
     * 只有 FD_PIPE 需要这一步：FD_CONSOLE 没有生命周期（串口一直在），
     * FD_INODE 在本 Lab 也没有（只读文件系统，inode 号是个不需要管理
     * 生命周期的纯数值，见 fs.h 顶部注释）。管道是本课程第一个真正需要
     * 引用计数的内核对象。 */
    for (int fd = 0; fd < NOFILE; fd++) {
        if (child->ofile[fd].type == FD_PIPE) {
            pipe_dup(child->ofile[fd].pipe, child->ofile[fd].writable);
        }
    }

    child->parent_pid = parent->pid;
    child->state = PROC_RUNNABLE;

    return child->pid;
}

/* sys_exec()：把调用者的地址空间换成 path 指定的那个程序。
 *
 * Lab9 才第一次名副其实。Lab7 的 sys_exec() 没有 path 参数——它把调用者
 * 的地址空间重新映射成内嵌 user_prog.bin 的一份*新拷贝*，也就是"exec
 * 自己"。那时候没有文件系统也没有 ELF 加载器，"exec 一个不同的程序"这个
 * Unix 语义在那个 Lab 的范围内根本无法表达，只能演示机制本身。现在两样
 * 都有了，这个函数终于可以做它名字说的那件事。
 *
 * 真正的加载工作全在 exec_load() 里（exec.c，两个架构共用）。本函数只
 * 负责三件架构相关的事：取参数、调加载器、把"这次 syscall 返回后恢复到
 * 哪里"改写成新程序的入口。
 *
 * ── 为什么靠写 user_rip_slot/user_rsp_slot ────────────────────────
 *
 * 跟 sys_fork() 一样，这两个参数是*地址*而不是值（proc.h 声明处有完整
 * 说明）。SYSCALL 这条陷入路径上没有 struct trapframe 可改——p->tf 是
 * IDT 路径专用的，SYSCALL 完全不碰它。要改变"系统调用返回后从哪里继续
 * 执行"，唯一的手段就是改写 syscall_entry 待会儿真的会去读的那两个位置：
 * 栈帧里被 push 过的 rcx 槽位，以及保存用户栈指针的那个位置。
 * trap_entry.S 在 call syscall_dispatch 返回之后会 pop rcx、恢复 rsp，
 * 读到的就是这里刚写进去的新值，然后 sysretq 到新程序的入口。
 *
 * ── 失败必须可恢复 ────────────────────────────────────────────────
 *
 * exec_load() 返回 -1 时调用者的地址空间一个字节都没动过，所以这里直接
 * return -1 就行，用户程序会从 exec() 调用点继续往下执行。sh 靠这个打
 * "command not found"，没有它一次拼错命令就会杀掉 shell。exec.c 顶部
 * 那段"提交线"注释是这个性质的完整论证。
 *
 * 注意本函数的两个 slot 是在 exec_load() *成功之后*才写的。顺序反了
 * （先写 slot 再加载）会让一次失败的 exec 把返回地址指向一个不存在的
 * 入口，用户程序收到 -1 但已经回不去了。 */
int sys_exec(const char *path, char *const argv[], uintptr_t *user_rip_slot,
             uintptr_t *user_rsp_slot)
{
    struct proc *p = g_current;
    if (p == NULL) {
        panic("sys_exec: called with no current process");
    }
    if (path == NULL) {
        return -1;
    }

    /* 进程名跟着程序走——ps 之类的东西靠它，本 Lab 里它出现在 panic 和
     * 调试输出里。取 path 的最后一段（去掉目录部分），跟 Unix 的
     * comm 字段一样。
     *
     * 必须在调用 exec_load() 之前做这件事，并把结果存进一个内核局部
     * 变量（name_buf），而不是留着 base 指向 path 本身、等 exec_load()
     * 返回之后再用。path 是调用者传进来的用户指针，指向*旧*地址空间。
     * exec_load() 一旦越过它自己文件头注释里说的"提交线"，就会拆掉旧
     * 映射、换上新程序的页表——这之后 path 指向的内存已经不再被映射，
     * 是一个悬空指针。
     *
     * ---- 这里也曾经踩过一个真实 bug，留档 ----
     * 原来的写法是先调 exec_load(path, ...)，成功之后才走下面这个循环
     * 用 path 取 basename。exec_load() 自己在提交线*之前*用 path 做
     * fs_lookup 之类的只读操作，那些调用当然没事——问题是 sys_exec()
     * 自己在 exec_load() 返回*之后*又把同一个 path 指针解引用了一次。
     * exec_load() 内部早就越过了提交线（uvm_clear() 拆了调用者的旧
     * 映射，装上了新程序的页表），所以这一次解引用踩在一片已经不再
     * 映射的地址上，触发内核态 page fault（不是 user 态，因为这段
     * 循环是内核代码本身在跑，只是它读的地址失效了）。
     *
     * 现象是 sh 执行 `echo` 这种第一条命令时一切正常（那次 exec 走完
     * 整个函数没人在提交线之后还碰 path），但只要执行完 exec、进程名
     * 赋值逻辑本身也需要 path，就会在下一次真正触发问题——GDB 断在
     * page fault handler 上，取出硬件压的 IRET 帧确认是内核态、CS 是
     * 内核段选择子，再用 info symbol 定位到 sys_exec 内部这个循环,
     * 才看出问题不在 exec_load() 里面，而在它返回之后的这几行。
     * 教训：调用者手上的用户指针，一旦调用的函数可能会让"调用者自己的
     * 地址空间"失效，就不能指望这个指针在调用之后还能用——不管调用
     * 本身是否成功返回。 */
    char name_buf[sizeof(p->name)];
    {
        const char *base = path;
        for (const char *s = path; *s != '\0'; s++) {
            if (*s == '/') {
                base = s + 1;
            }
        }
        size_t i = 0;
        while (base[i] != '\0' && i + 1 < sizeof(name_buf)) {
            name_buf[i] = base[i];
            i++;
        }
        name_buf[i] = '\0';
    }

    uintptr_t entry = 0;
    uintptr_t sp = 0;
    if (exec_load(p, path, argv, &entry, &sp) < 0) {
        return -1;
    }

    /* exec_load() 已经成功提交，p 是内核自己的结构体，不受地址空间
     * 切换影响，这里只是把之前算好的 name_buf 拷进去。 */
    size_t i = 0;
    while (name_buf[i] != '\0' && i + 1 < sizeof(p->name)) {
        p->name[i] = name_buf[i];
        i++;
    }
    p->name[i] = '\0';

    *user_rip_slot = entry;
    *user_rsp_slot = sp;

    return 0;
}

/* sys_wait()：等一个子进程退出，回收它，返回它的 pid。
 *
 * ── Lab9 的两个变化 ──────────────────────────────────────────────
 *
 * 一、变成阻塞的。Lab7/Lab8 的版本找不到 ZOMBIE 子进程就立刻返回 -1，
 * 理由是"没有睡眠/唤醒机制"。Lab9 必须阻塞，因为 shell 的主循环是
 * fork → exec → wait：如果 wait 在子进程还在跑的时候就返回 -1，shell
 * 会立刻打下一个提示符、读下一条命令，而上一条命令的输出还在往外冒。
 * 交互式 shell 的基本行为——"命令跑完了才给我下一个提示符"——就是
 * wait 阻塞语义的直接体现。
 *
 * 实现方式是 yield 循环：没找到就 yield()，被调度回来再找一遍。这不是
 * 真正的睡眠/唤醒（那需要一个"等待队列"和"exit 时唤醒父进程"的配对
 * 机制，本课程不引入），而是忙等——每次被调度到就检查一次。代价是
 * 浪费时间片：一个在 wait 的进程每个时间片都会被唤醒、扫一遍进程表、
 * 再让出去。真实内核不会这么做（Linux 的 wait4 走 wait_queue），但在
 * 只有几个进程、时间片由 PIT 驱动的教学内核里，这个浪费看不出来，
 * 而它省掉的是一整套等待队列机制。README 挑战任务里有"改成真正的
 * 睡眠/唤醒"这一项。
 *
 * 循环之前必须先确认"我到底有没有子进程"。没有子进程的话 yield 循环
 * 永远等不到东西，进程就永久卡住了——这不是阻塞，是死锁。POSIX 的
 * wait() 在这种情况下返回 ECHILD，这里返回 -1。
 *
 * 二、回收用户页。Lab7/Lab8 只释放内核栈，用户页和页表都漏着，明确
 * 接受的简化（NPROC=4，一共只 fork 几次）。Lab9 不能这样：shell 每
 * 执行一条命令 fork 一次，漏的量由用户敲了多少条命令决定。uvm_clear()
 * 把用户页全部还回去，页表交给槽位复用（见 proc_alloc_skeleton()）。 */
int sys_wait(int64_t *exit_code_out)
{
    struct proc *caller = g_current;
    if (caller == NULL) {
        panic("sys_wait: called with no current process");
    }

    for (;;) {
        int have_child = 0;

        for (int i = 0; i < NPROC; i++) {
            struct proc *p = &proc_table[i];
            if (p->parent_pid != caller->pid || p->state == PROC_UNUSED) {
                continue;
            }
            have_child = 1;

            if (p->state != PROC_ZOMBIE) {
                continue;
            }

            if (exit_code_out != NULL) {
                *exit_code_out = p->exit_code;
            }
            int pid = p->pid;

            /* 用户页先还，再还内核栈。顺序其实无关——两者没有依赖——
             * 但 uvm_clear() 要用 p->pagetable，而页表是留着给槽位复用
             * 的，不会被释放，所以这里没有"先释放了页表再去用它"的风险。 */
            uvm_clear(p);
            kfree_pages(p->kstack_phys, PROC_KSTACK_PAGES);
            p->kstack_phys = NULL;
            p->state = PROC_UNUSED;

            return pid;
        }

        if (!have_child) {
            /* 没有任何子进程，等下去只会永远等。POSIX 在这里是 ECHILD。 */
            return -1;
        }

        yield();
    }
}
