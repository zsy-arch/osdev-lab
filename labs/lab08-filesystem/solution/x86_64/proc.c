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

#define USER_PROG_VADDR   0x400000ull
#define USER_STACK_VADDR  (USER_PROG_VADDR + 0x2000ull)

/* 跟 kernel_main.c/pagetable.c 同名常量必须保持一致（本课程一贯的
 * "没有单一数据源、需要人肉对齐"的手工契约，见那两个文件里对应的
 * 注释）——这里需要它是因为 kalloc_page() 返回的是物理地址，本文件
 * 需要先加上这个偏移才能把它当指针解引用（memset/memcpy 用户程序/
 * 栈页内容）。 */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull

extern char __user_prog_start[];
extern char __user_prog_end[];

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

/* 把内嵌的 user_prog.bin 拷贝进一个新分配的物理页、映射到 pagetable
 * 里的 USER_PROG_VADDR，再分配+映射一页用户栈——proc_alloc()（创建
 * 全新进程）和 sys_exec()（重置已有进程的地址空间）共用这段逻辑，
 * 是 kernel_main.c（Lab6 版本）里同一段代码的搬移，行为完全不变，
 * 只是从"内联在 kernel_boot() 里、只会跑一次"变成"一个可以被多次
 * 调用的函数"。
 *
 * 返回值是新映射好的用户栈顶（USER_STACK_VADDR）——调用者拿它去初始
 * 化 trapframe.rsp，不需要重新计算这个常量。 */
static uintptr_t map_user_prog(uintptr_t pagetable)
{
    void *prog_page_phys = kalloc_page();
    if (prog_page_phys == NULL) {
        panic("map_user_prog: kalloc_page() failed for program page");
    }
    uint8_t *prog_page_kva =
        (uint8_t *)((uintptr_t)prog_page_phys + KERNEL_VIRT_BASE);

    size_t prog_len = (size_t)(__user_prog_end - __user_prog_start);
    if (prog_len > PAGE_SIZE) {
        panic("map_user_prog: embedded user program does not fit in one page");
    }
    memset(prog_page_kva, 0, PAGE_SIZE);
    memcpy(prog_page_kva, __user_prog_start, prog_len);

    pagetable_map(pagetable, USER_PROG_VADDR, (uintptr_t)prog_page_phys,
                  PTE_FLAG_USER | PTE_FLAG_EXECUTABLE);

    void *stack_page_phys = kalloc_page();
    if (stack_page_phys == NULL) {
        panic("map_user_prog: kalloc_page() failed for stack page");
    }
    uint8_t *stack_page_kva =
        (uint8_t *)((uintptr_t)stack_page_phys + KERNEL_VIRT_BASE);
    memset(stack_page_kva, 0, PAGE_SIZE);

    pagetable_map(pagetable, USER_STACK_VADDR - PAGE_SIZE,
                  (uintptr_t)stack_page_phys,
                  PTE_FLAG_USER | PTE_FLAG_WRITABLE);

    return USER_STACK_VADDR;
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

    /* Lab8：清空打开文件表。
     *
     * 必须显式清：proc_table 是全局数组（首次进入时确实是全 0），但槽位
     * 会被*复用*——一个进程 exit 变成 ZOMBIE、被 wait 回收成 UNUSED 之后，
     * 同一个槽位会分给下一个新进程。如果不清，新进程会继承上一个进程
     * 残留的 ofile[] 内容，凭空拿到几个"已经打开"的 fd，指向上一个进程
     * 打开过的文件。这是内核里一类非常典型的信息泄露：新进程不该看到
     * 前任留下的任何状态。 */
    memset(p->ofile, 0, sizeof(p->ofile));

    p->pagetable = pagetable_create();
    pagetable_copy_kernel_range(p->pagetable);

    p->kstack_phys = kalloc_page();
    if (p->kstack_phys == NULL) {
        panic("proc_alloc_skeleton: kalloc_page() failed for kernel stack");
    }

    /* 内核栈布局（从高地址到低地址，即从栈顶往下）：
     *   [kstack_phys + PAGE_SIZE]                <- 栈顶
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
    uintptr_t kstack_kva_top =
        (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
    p->tf = (struct trapframe *)(kstack_kva_top - sizeof(struct trapframe));
    p->context = (struct context *)((uintptr_t)p->tf - sizeof(struct context));

    memset(p->tf, 0, sizeof(struct trapframe));
    memset(p->context, 0, sizeof(struct context));
    p->context->rip = (uint64_t)trap_return;

    return p;
}

/* 创建一个全新进程：骨架 + 内嵌 user_prog.bin 的一份全新拷贝，作为
 * 本 Lab 唯一的"从无到有"的进程创建入口（kernel_main.c 用它创建初始
 * 进程；sys_fork() 不用它，因为子进程的用户地址空间内容来自父进程，
 * 不是内嵌镜像的新拷贝，见 sys_fork() 自己的实现）。 */
struct proc *proc_alloc(void)
{
    struct proc *p = proc_alloc_skeleton();
    if (p == NULL) {
        return NULL;
    }

    uintptr_t user_stack_top = map_user_prog(p->pagetable);

    p->tf->rip = USER_PROG_VADDR;
    p->tf->rsp = user_stack_top;
    p->tf->cs = 0x28 | 3;   /* GDT_SEL_USER_CS_RPL3，见 trap.c。 */
    p->tf->ss = 0x20 | 3;   /* GDT_SEL_USER_SS_RPL3，见 trap.c。 */
    p->tf->rflags = 0x202; /* IF=1 + 保留位，跟 enter_user_mode 一致。 */

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

    p->exit_code = code;
    p->state = PROC_ZOMBIE;

    swtch(&p->context, g_scheduler_context);

    panic("sys_exit_proc: swtch() returned into a ZOMBIE process, should be unreachable");
}

/* 给子进程分配一页物理内存、把父进程在 parent_vaddr 处那一页的内容
 * 整页拷过去、按 flags 映射到子进程页表的同一个虚拟地址——"同一个
 * 虚拟地址"是这个简化 fork 能工作的关键：子进程的 trapframe（下面
 * sys_fork() 里整份拷贝自父进程）里 rip/rsp 存的还是父进程的虚拟
 * 地址值，如果子进程页表里把这些内容映射到不同的虚拟地址，子进程
 * 恢复执行时会读到完全无关的内存。
 *
 * pagetable_lookup() 查父进程页表在 parent_vaddr 处映射到的物理页——
 * 本 Lab 的 fork() 只会在两个固定虚拟地址上调用这个函数（USER_PROG_
 * VADDR 和 USER_STACK_VADDR-PAGE_SIZE，map_user_prog() 建立的那两页），
 * 查不到（返回 0，pagetable_lookup() 自己文档里说明的"没有映射"哨兵值）
 * 说明父进程本身没有正确建立映射，属于内部不变量被破坏，直接 panic，
 * 不是需要向用户程序传播的正常错误。 */
static void fork_copy_page(uintptr_t child_pagetable, uintptr_t parent_pagetable,
                            uintptr_t vaddr, uint32_t flags)
{
    uintptr_t parent_phys = pagetable_lookup(parent_pagetable, vaddr);
    if (parent_phys == 0) {
        panic("fork_copy_page: parent has no mapping at expected vaddr");
    }

    void *child_phys = kalloc_page();
    if (child_phys == NULL) {
        panic("fork_copy_page: kalloc_page() failed");
    }

    memcpy((void *)((uintptr_t)child_phys + KERNEL_VIRT_BASE),
           (void *)(parent_phys + KERNEL_VIRT_BASE), PAGE_SIZE);

    pagetable_map(child_pagetable, vaddr, (uintptr_t)child_phys, flags);
}

/* sys_fork()：user_rip_slot/user_rsp_slot 是"父进程这次 SYSCALL 返回后
 * 该恢复到哪个 rip/用哪个用户栈指针"这两个值的*地址*——在 SYSCALL 这
 * 条陷入路径上,它们分别是 syscall_entry（trap_entry.S）栈帧里被 push
 * 过的 rcx 槽位,以及 syscall_saved_user_rsp 这个全局变量本身,不是
 * proc.h 意义上的 struct trapframe（那是 IDT 路径专用的现场格式,
 * SYSCALL 完全不碰 p->tf,见 proc.h 里这两个函数声明处的大注释)。
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
 *      sysretq 的返回路径",这两个值就是父进程那份现场里真正有意义
 *      的部分（本课程的用户态代码只依赖 rax/rdi/rsi 在系统调用前后
 *      保持约定关系,见 user_prog.S,不需要完整寄存器现场)。cs/ss/
 *      rflags 用跟 proc_alloc() 里一样的常量（子进程终归要以 ring3
 *      身份恢复执行,这三个字段的值不依赖父进程具体是谁,是"回到用户
 *      态"这件事本身固定的)。
 *   3. 子进程 trapframe 里的 rax 保持 proc_alloc_skeleton() memset 出
 *      来的 0,不用像旧设计那样再显式清一次——这正是 fork() 在子进程
 *      里返回 0、在父进程里返回子进程 pid 这条 Unix 经典语义的全部
 *      实现：父进程这次系统调用本身通过正常的 syscall_dispatch 返回值
 *      机制拿到子进程 pid（本函数的返回值),子进程将来被调度器 swtch()
 *      进去、trap_return 恢复这份 trapframe、sysretq 回到用户态时,
 *      看到的 rax（fork() 的返回值寄存器）就是这里从未被写过的 0。
 *   4. parent_pid 记录血缘关系,state 设成 RUNNABLE 交给调度器。
 *
 * 找不到空闲进程表槽位（NPROC 个槽位全在用）返回 -1，不 panic——这
 * 是用户程序完全可能触发的正常情况（连续 fork 到表满),必须能通过
 * 系统调用返回值告知调用者,不是内部不变量被破坏。 */
int sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot)
{
    struct proc *parent = g_current;
    if (parent == NULL) {
        panic("sys_fork: called with no current process");
    }

    struct proc *child = proc_alloc_skeleton();
    if (child == NULL) {
        return -1;
    }

    fork_copy_page(child->pagetable, parent->pagetable, USER_PROG_VADDR,
                   PTE_FLAG_USER | PTE_FLAG_EXECUTABLE);
    fork_copy_page(child->pagetable, parent->pagetable,
                   USER_STACK_VADDR - PAGE_SIZE,
                   PTE_FLAG_USER | PTE_FLAG_WRITABLE);

    child->tf->rip = *user_rip_slot;
    child->tf->rsp = *user_rsp_slot;
    child->tf->cs = 0x28 | 3;   /* GDT_SEL_USER_CS_RPL3，见 trap.c。 */
    child->tf->ss = 0x20 | 3;   /* GDT_SEL_USER_SS_RPL3，见 trap.c。 */
    child->tf->rflags = 0x202; /* IF=1 + 保留位，跟 proc_alloc() 一致。 */

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

    child->parent_pid = parent->pid;
    child->state = PROC_RUNNABLE;

    return child->pid;
}

/* sys_exec()：本 Lab 范围内的简化语义（见本文件顶部模块注释）——把
 * 调用者自己的地址空间重新映射成内嵌 user_prog.bin 的一份全新拷贝，
 * 通过*写*user_rip_slot/user_rsp_slot（跟 sys_fork() 一样,这两个参数
 * 是地址而不是值,见 sys_fork() 的注释和 proc.h 声明处的大注释)把"这次
 * SYSCALL 返回后该恢复到哪里"重定向到新程序的入口/新栈顶——这就是
 * exec() 在没有 struct trapframe 可用的 SYSCALL 路径上,唯一能够真正
 * 改变"系统调用之后恢复执行的位置"的手段：trap_entry.S 的 syscall_
 * entry 在 call syscall_dispatch 返回之后,会 pop 那个被 sys_exec()
 * 改写过的 rcx 槽位、重新读一次 syscall_saved_user_rsp,这两步读到的
 * 就是这里刚写进去的新值。
 *
 * 调用者旧的两个映射（程序页/栈页)必须先 pagetable_unmap() 撤销、
 * 拿到旧物理页地址后 kfree_page() 释放,再调 map_user_prog() 重新
 * 映射同样的虚拟地址——这是本 Lab 早期版本没有做的一步（早期版本
 * 直接 panic,理由是"pagetable_map() 对同一个虚拟地址映射两次会造成
 * 双重映射",但那其实是在说"应该先 unmap 再 map",不是"exec 本身
 * 不可实现")：pagetable_unmap()（Lab7 新增,pagetable.h 有完整注释)
 * 补上了这一步,让 exec 可以在任何已经跑起来的进程上真正调用,不再
 * 只能停留在"代码里写了但从来不会被真正执行,因为一调就 panic"这种
 * 名不副实的状态——proc_alloc()/sys_fork() 创建的每一个进程,从出生
 * 那一刻起 USER_PROG_VADDR 就已经有映射（map_user_prog()/fork_copy_
 * page() 各自负责),如果这里还坚持"已经有映射就 panic",exec 会变成
 * 一个永远不可能被调用一次而不崩溃的系统调用,不满足 ROADMAP 明确
 * 要求的"实现 fork/exec/wait/exit"这个目标里 exec 那一项。
 *
 * 不释放/回收调用者页表本身的中间层节点——跟 sys_wait() 里"页表本身
 * 不回收"是同一个教学取向（那边注释已经说明,NPROC 只有 4,不值得为了
 * 这点内存实现完整的页表节点回收遍历),这里只回收两片叶子物理页
 * （程序页+栈页),中间层的 PDPT/PD/PT 节点保留、原地复用（map_user_
 * prog() 内部 pagetable_map() -> walk() 发现中间层已经存在就不会
 * 重新分配,只是最后一级叶子项被覆写成新物理页的地址)。 */
int sys_exec(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot)
{
    struct proc *p = g_current;
    if (p == NULL) {
        panic("sys_exec: called with no current process");
    }

    uintptr_t old_prog_phys =
        pagetable_unmap(p->pagetable, USER_PROG_VADDR);
    if (old_prog_phys != 0) {
        kfree_page((void *)old_prog_phys);
    }

    uintptr_t old_stack_phys =
        pagetable_unmap(p->pagetable, USER_STACK_VADDR - PAGE_SIZE);
    if (old_stack_phys != 0) {
        kfree_page((void *)old_stack_phys);
    }

    uintptr_t user_stack_top = map_user_prog(p->pagetable);

    *user_rip_slot = USER_PROG_VADDR;
    *user_rsp_slot = user_stack_top;

    return 0;
}

/* sys_wait()：在 proc_table 里找一个"调用者的 ZOMBIE 子进程"——本 Lab
 * 范围内的简化：找不到就直接返回 -1（不阻塞/不 yield 循环等待），因为
 * 本 Lab 的教学目标是演示"fork/exit/wait 这套机制怎么串起来"，不是
 * 实现完整的阻塞式 wait()语义（真实内核的 wait() 找不到 ZOMBIE 子
 * 进程时会让调用者进程睡眠,直到某个子进程 exit 时被唤醒,这需要一套
 * 睡眠/唤醒机制,本课程到 Lab7 为止还没有引入)。
 *
 * 找到之后：读出 exit_code、回收资源（内核栈物理页——页表本身本 Lab
 * 不回收，教学取向：pagetable_create() 分配的页表节点数量不大，NPROC
 * 只有 4，不值得为了这一点内存实现完整的页表销毁遍历,这是本 Lab
 * 明确接受的简化,不是遗漏)、把槽位标回 PROC_UNUSED 供以后复用。 */
int sys_wait(int64_t *exit_code_out)
{
    struct proc *caller = g_current;
    if (caller == NULL) {
        panic("sys_wait: called with no current process");
    }

    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state == PROC_ZOMBIE && p->parent_pid == caller->pid) {
            if (exit_code_out != NULL) {
                *exit_code_out = p->exit_code;
            }
            int pid = p->pid;

            kfree_page(p->kstack_phys);
            p->state = PROC_UNUSED;

            return pid;
        }
    }

    return -1;
}
