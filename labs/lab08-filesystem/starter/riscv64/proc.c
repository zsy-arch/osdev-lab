/* Lab7 riscv64：进程表 + 调度器 + fork/exec/wait/exit 的具体实现。跟
 * x86_64 版本（../x86_64/proc.c）同一份整体骨架（同样的 proc_table 线性
 * 扫描、同样朴素的轮转调度、同样"完整复制不做 COW"的 fork、同样"重新
 * 映射内嵌镜像"的 exec、同样"找不到 ZOMBIE 就返回 -1 不阻塞"的 wait)，
 * 设计动机跟 x86_64 版本完全一致的部分不重复展开，只写 riscv64 寄存器/
 * CSR 相关的具体差异——见本文件各处注释跟 x86_64 版本对应位置的对比。
 *
 * 关键简化（ROADMAP 明确认可的教学取向,不是遗漏,跟 x86_64 版本同一份
 * 模块顶部注释）：
 *   - exec 不是"加载任意程序"，是"把调用者自己的地址空间重新映射成
 *     内核镜像里那份 user_prog.bin 的一份新拷贝，重置 trapframe 到
 *     入口点"。
 *   - fork 出的子进程页表不是"写时复制"，是立即完整复制每一个已映射页
 *     的内容到新分配的物理页。
 */
#include "types.h"
#include "proc.h"
#include "kalloc.h"
#include "pagetable.h"
#include "panic.h"
#include "console.h"
#include "string.h"

struct proc proc_table[NPROC];

/* 下一个分配的 pid，从 1 开始——跟 x86_64 版本同一个理由（0 留作
 * "无效/未分配"的哨兵值),不重复展开。 */
static int g_next_pid = 1;

/* 当前正在 RUNNING 的进程——跟 x86_64 版本同一个理由，不重复展开。 */
static struct proc *g_current;

extern void swtch(struct context **old, struct context *new);
extern void trap_return(void);

/* trap_kernel_sp_top/trap_saved_user_sp：定义在 trap.c（只被
 * trap_entry.S 用汇编 `la`/`sd`/`ld` 直接读写),proc.c 需要从 C 侧
 * 读写它们——scheduler() 要写 trap_kernel_sp_top（每次 swtch() 进某个
 * 进程之前),sys_fork() 要读 trap_saved_user_sp（子进程 trapframe.sp
 * 的来源),sys_exec() 要*写* trap_saved_user_sp（下面 write_sepc()
 * 上方注释详细解释了为什么这一步是必须的,不是可选的),都需要一个
 * extern 声明。 */
extern uint64_t trap_kernel_sp_top;
extern uint64_t trap_saved_user_sp;

static uintptr_t read_sepc(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, sepc" : "=r"(value));
    return value;
}

/* write_sepc()：真实调试出来的教训,不是提前设计好的——最早的 sys_exec()
 * 实现只写了 g_current->tf->sepc/tf->sp（跟 sys_fork() 写子进程
 * trapframe 那两行看起来对称),结果是 exec 之后的目标进程"卡在自己
 * 那条 ecall 指令的下一条指令处死循环"（user_prog.S 里 SYS_EXEC 那次
 * ecall 后面紧跟的 halt_loop 兜底死循环),完全没有跳到新程序入口——
 * 用 `qemu-system-riscv64 -d int` 抓出来的现象是同一个 epc/tval 无限
 * 重复,横跨定时器中断和下一次 ecall,两者 epc 完全相同,说明这个进程
 * 每次被恢复执行,落地的 PC 都不是 sys_exec() 写过的那个新值。
 *
 * 根因：g_current->tf（struct trapframe)只在"这个进程第一次被 swtch()
 * 进去"这一种场景下才会被读取——proc_alloc_skeleton() 把 context.ra
 * 设成 swtch.S 的 trap_return,*只有*那一次 ret 落进 trap_return 才会
 * 真正去读 tf->sepc/tf->sp/tf->sstatus（proc.h/swtch.S 顶部注释已经
 * 展开过这条路径)。sys_exec() 是从一次*正在进行中*的 ecall 里调用的
 * ——调用者此刻正走 trap_entry.S 的轻量栈帧路径,即将执行的是那条
 * `sret`,而 `sret` 直接读硬件 CSR `sepc`（这一刻还是 ecall_handler
 * 分发之前 sepc+=4 之后的旧值,即"SYS_EXEC 这条 ecall 指令的下一条
 * 指令"),根本不会去看 g_current->tf 里的任何字段——两条路径读写的是
 * 两份完全不同的存储位置,写一份对另一份毫无影响,跟 sys_fork() 里
 * "子进程从未运行过,所以它必然会经过 tf 那条路径"是完全不同的场景，
 * 不能类比。
 *
 * 修法：sys_exec() 除了写 tf（保留下来是为了文档意义上的对称、以及
 * 防止将来这个进程真的经过一次 trap_return 路径的边界情况),必须*额外*
 * 直接改写这次即将 sret 所依赖的两个真实存储位置——sepc 这个 CSR（用
 * 这个 write_sepc(),对应 trap.c 顶部同名 static 函数,riscv64 CSR 访问
 * 没有跨文件共享的语法限制,两份文件各自定义一份是因为 trap.c 那份是
 * static,不能直接 extern 过来)和 trap_saved_user_sp 这个全局变量
 * （trap_entry.S 的 from_user 分支马上要从这里 ld 回 sp,见该文件"SPP=0"
 * 分支的完整注释)。改完这两处之后,ecall 返回路径走的还是原来那条
 * "ld 恢复 16 个寄存器->sret"逻辑,不需要新增分支,sret 那一刻硬件读到
 * 的 sepc/sp 已经是 sys_exec() 刚写的新值,效果上等价于"这个进程从
 * USER_PROG_VADDR 重新开始执行,用户栈是全新映射的那一页"。 */
static void write_sepc(uintptr_t value)
{
    __asm__ volatile("csrw sepc, %0" : : "r"(value));
}

#define USER_PROG_VADDR   0x400000ull
#define USER_STACK_VADDR  (USER_PROG_VADDR + 0x2000ull)

/* 跟 kernel_main.c/pagetable.c 同名常量必须保持一致（本课程一贯的
 * "没有单一数据源、需要人肉对齐"的手工契约,见那两个文件里对应的
 * 注释）——这里需要它是因为 kalloc_page() 返回的是物理地址，本文件
 * 需要先加上这个偏移才能把它当指针解引用（memset/memcpy 用户程序/
 * 栈页内容,以及 p->tf/p->context 的换算)。 */
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull

/* tf->sstatus 的常量值：SPP=0（回到 U-mode）/ SPIE=1（bit 5，sret 之后
 * 全局中断使能恢复)/ SUM=1（bit 18，允许 S-mode 代码访问 U-mode 页
 * ——本 Lab 的 sys_write() 需要直接解引用用户传入的指针，没有这一位
 * 会触发 load/store page fault,Lab6 enter_user_mode 开发时期真实踩过
 * 这个坑，完整narrative见 Lab6 trap_entry.S 对应注释)——这三个具体
 * 数值跟 Lab6 已删除的 enter_user_mode 曾经硬编码的常量完全相同,只是
 * 那个函数已经在 Lab7 被 trap_return（swtch.S)统一取代,这个常量需要
 * 一个新家,搬到这里：proc_alloc()/sys_fork() 是本 Lab 唯一"从零构造
 * 一份 trapframe 交给一个即将第一次运行的进程"的两个地方,天然是这个
 * 常量该住的地方。 */
#define INITIAL_SSTATUS ((1ull << 5) | (1ull << 18))

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
 * 全新进程）和 sys_exec()（重置已有进程的地址空间）共用这段逻辑，跟
 * x86_64 版本同名函数完全一样的复用关系，不重复展开。
 *
 * 返回值是新映射好的用户栈顶（USER_STACK_VADDR）。 */
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
 * 的半成品进程——proc_alloc()（全新进程)和 sys_fork()（子进程)共用这段
 * 骨架,跟 x86_64 版本同名函数同一个复用关系,不重复展开。
 *
 * 找不到空槽位返回 NULL,调用者负责处理。 */
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

    /* TODO 1（Lab8 新增，本文件其余部分是 Lab7 的成果，已经写好）：
     * 清空这个进程的打开文件表 p->ofile[]。一行 memset。
     *
     * 为什么不能省这一行：proc_table 是全局数组，首次进入时确实是全 0，
     * 但**槽位会被复用**——一个进程 exit 变成 ZOMBIE、被 wait 回收成
     * UNUSED 之后，同一个槽位会分给下一个新进程。不清的话，新进程会继承
     * 上一个进程残留的 ofile[] 内容，凭空拿到几个"已经打开"的 fd，指向
     * 上一个进程打开过的文件。
     *
     * 这是内核里一类非常典型的信息泄露：新进程不该看到前任留下的任何
     * 状态。值得留意的是这个 bug 在本 Lab 的测试里**不一定会暴露**——
     * NPROC 只有 4，测试程序也不见得会让槽位复用发生在打开过文件之后。
     * "测试没抓到"和"代码是对的"是两件事。
     *
     * 提示：
     * memset(p->ofile, 0, sizeof(p->ofile));
     */

    p->pagetable = pagetable_create();
    pagetable_copy_kernel_range(p->pagetable);

    p->kstack_phys = kalloc_page();
    if (p->kstack_phys == NULL) {
        panic("proc_alloc_skeleton: kalloc_page() failed for kernel stack");
    }

    /* 内核栈布局，跟 x86_64 版本同一个"从高地址到低地址"摆法：
     *   [kstack_phys + PAGE_SIZE]                 <- 栈顶
     *   struct trapframe                           <- p->tf 指向这里
     *   struct context（context->ra 填 trap_return） <- p->context 指向这里
     * trap_return（swtch.S)执行时 sp 停在 context 13 个字段被 ld 完之后
     * 的位置，正好落在 trapframe 的起始地址——见 swtch.S trap_return
     * 那段注释里"紧邻"关系的具体落点。
     *
     * p->tf/p->context 存的是*内核虚拟地址*（kstack_phys+KERNEL_VIRT_
     * BASE 之后再算的偏移),不是物理地址——跟 x86_64 版本同一个理由
     * （pagetable_activate() 之后物理地址不再是合法的可解引用地址),
     * 不重复展开。 */
    uintptr_t kstack_kva_top =
        (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
    p->tf = (struct trapframe *)(kstack_kva_top - sizeof(struct trapframe));
    p->context = (struct context *)((uintptr_t)p->tf - sizeof(struct context));

    memset(p->tf, 0, sizeof(struct trapframe));
    memset(p->context, 0, sizeof(struct context));
    p->context->ra = (uint64_t)trap_return;

    return p;
}

/* 创建一个全新进程：骨架 + 内嵌 user_prog.bin 的一份全新拷贝——跟
 * x86_64 版本同名函数同一个角色(kernel_main.c 用它创建初始进程;
 * sys_fork() 不用它,理由跟 x86_64 版本一致,不重复展开)。 */
struct proc *proc_alloc(void)
{
    struct proc *p = proc_alloc_skeleton();
    if (p == NULL) {
        return NULL;
    }

    uintptr_t user_stack_top = map_user_prog(p->pagetable);

    p->tf->sepc = USER_PROG_VADDR;
    p->tf->sp = user_stack_top;
    p->tf->sstatus = INITIAL_SSTATUS;

    p->state = PROC_RUNNABLE;
    return p;
}

/* scheduler() 自己也是一条"内核执行流"——跟 x86_64 版本 g_scheduler_
 * context 同一个存在理由、同一个类型要求（必须是 struct context *,
 * 不能是 struct context 本身,理由见 x86_64 版本对应注释里那段
 * -Wincompatible-pointer-types 的真实编译错误叙述,riscv64 这边是
 * 同一个 swtch() 签名、同一个类型约束,不重复展开)。 */
static struct context *g_scheduler_context;

/* scheduler()：本 Lab 唯一的"内核主循环"，noreturn——跟 x86_64 版本
 * 同一个角色。每次找到一个 RUNNABLE 的进程，进去之前必须：
 *   1. 标成 RUNNING。
 *   2. trap_kernel_sp_top 指向它自己的内核栈顶——这是 trap_entry.S
 *      顶部模块注释、本文件顶部反复强调的关键一步：不做这一步,这个
 *      进程在用户态被定时器/ecall 打断时,trap_entry.S 的 from_user
 *      分支会换到*上一个*进程的内核栈（或者压根没设过的垃圾值),直接
 *      复现 x86_64 版本 scheduler() 注释里详细描述过的那个"RIP=0/
 *      CR2=0"式故障的 riscv64 版本——riscv64 没有 TSS,不需要像
 *      x86_64 那样分两步（tss_set_rsp0 + syscall_set_kernel_rsp),
 *      因为 riscv64 的 ecall/page fault/定时器从 Lab6 起就共享同一个
 *      stvec 入口、同一套"读 trap_kernel_sp_top 决定要不要切栈"的
 *      逻辑（trap_entry.S),只有一条硬件路径需要这个值,不像 x86_64
 *      的中断门（IDT)和 SYSCALL 快速路径是两条独立硬件机制、各自有
 *      各自的栈切换手段,必须分别设置。传的必须是*内核虚拟地址*，跟
 *      p->tf/p->context 用的是同一个换算方式。
 *   3. pagetable_activate() 切到它自己的页表。
 *   4. g_current 指向它。
 *
 * swtch(&g_scheduler_context, p->context) 换过去之后，swtch() 返回时
 * （这个进程通过 yield()/sys_exit_proc() 把控制权还给调度器之后)，
 * 把它从 RUNNING 改回 RUNNABLE（如果只是被抢占；ZOMBIE 已经在
 * sys_exit_proc() 里设置过),再继续 for 循环找下一个。 */
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
            trap_kernel_sp_top = kstack_kva_top;
            pagetable_activate(p->pagetable);

            swtch(&g_scheduler_context, p->context);

            /* 回到这里时 g_current 未必还是 p——理由跟 x86_64 版本
             * 完全一致（sys_exit_proc() 会把一个进程标成 ZOMBIE 之后
             * 直接切回调度器,这里统一用 p 判断,不用 g_current),不
             * 重复展开。 */
            g_current = NULL;
            if (p->state == PROC_RUNNING) {
                p->state = PROC_RUNNABLE;
            }
        }
    }
}

/* yield()：一个进程主动/被动让出 CPU 的唯一入口——跟 x86_64 版本同一个
 * 角色、同一个前提检查（g_current 可能是 NULL,见 trap.c timer_
 * interrupt_handler 里的判断),不重复展开。 */
void yield(void)
{
    struct proc *p = g_current;
    if (p == NULL) {
        return;
    }

    p->state = PROC_RUNNABLE;
    swtch(&p->context, g_scheduler_context);
}

/* sys_exit_proc()：SYS_EXIT 系统调用最终落到的地方——跟 x86_64 版本
 * 同一个角色（标成 ZOMBIE 而不是直接回收,理由跟 x86_64 版本 sys_
 * exit_proc() 顶部注释一致,不重复展开)。 */
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
 * 整页拷过去、按 flags 映射到子进程页表的同一个虚拟地址——跟 x86_64
 * 版本同名函数同一个角色、同一个"必须是同一个虚拟地址"的理由,不
 * 重复展开。 */
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

/* sys_fork()：跟 x86_64 版本不同的签名/取值来源,完整原因见 proc.h
 * sys_fork/sys_exec 声明处那段（已经纠正过的）大注释,这里不重复
 * 展开,只写具体步骤：
 *   1. 骨架 + 逐页拷贝父进程已映射的两页（程序页+栈页)到子进程自己的
 *      物理页,映射到*同样的虚拟地址*。
 *   2. tf->sepc/tf->sp 分别设成 read_sepc()（父进程这次 ecall 陷入,
 *      sepc+4 之后的当前值)和 trap_saved_user_sp（父进程这次陷入时
 *      的用户栈顶)——子进程"从 fork() 系统调用返回"，本质上就是
 *      "在跟父进程这次系统调用一样的 sepc/sp 现场下,走一遍 trap_
 *      return->sret 的返回路径"。tf->sstatus 用 INITIAL_SSTATUS
 *      常量,不读当前 sstatus——理由见 proc.h 对应注释。
 *   3. 子进程 trapframe 的 a0（fork() 返回值寄存器)保持骨架 memset
 *      清零时的 0,不用额外清——这正是 fork() 在子进程里返回 0、在
 *      父进程里返回子进程 pid 这条 Unix 经典语义的全部实现：父进程
 *      这次系统调用本身通过正常的 syscall_dispatch 返回值机制拿到
 *      子进程 pid（本函数的返回值),子进程将来被 swtch() 进去、
 *      trap_return 恢复这份 trapframe、sret 回到用户态时,看到的
 *      a0 就是这里从未被写过的 0。
 *   4. parent_pid 记录血缘关系,state 设成 RUNNABLE。
 *
 * 找不到空闲进程表槽位返回 -1，不 panic——跟 x86_64 版本同一个理由
 * （用户程序完全可能触发的正常情况),不重复展开。 */
int sys_fork(void)
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

    child->tf->sepc = read_sepc();
    child->tf->sp = trap_saved_user_sp;
    child->tf->sstatus = INITIAL_SSTATUS;

    /* TODO 2（Lab8 新增）：把父进程的打开文件表整份拷给子进程。一行 memcpy。
     *
     * 这一行就是 fork() 的"文件描述符继承"语义——子进程一出生就拥有跟
     * 父进程一样的 fd 集合，fd 号也一样。shell 的重定向就靠这个：父进程
     * fork 之后、exec 之前把 fd 换掉，新程序启动时看到的就是换过的 fd。
     *
     * 本 Lab 的继承只是**拷贝**而不是真正的 Unix 共享语义，这个差别是能
     * 观察到的、不是理论上的——见 ../x86_64/proc.c 同一处 TODO 的说明和
     * README 的挑战任务。
     *
     * 提示：
     * memcpy(child->ofile, parent->ofile, sizeof(child->ofile));
     */

    child->parent_pid = parent->pid;
    child->state = PROC_RUNNABLE;

    return child->pid;
}

/* sys_exec()：本 Lab 范围内的简化语义（见本文件顶部模块注释)——把
 * 调用者自己的地址空间重新映射成内嵌 user_prog.bin 的一份全新拷贝。
 *
 * 必须同时写两处,不能只写 g_current->tf：
 *   1. p->tf->sepc/tf->sp——文档意义上的对称（跟 sys_fork() 写子进程
 *      trapframe 那两行看起来一致),覆盖"这个进程将来真的经过一次
 *      trap_return"的边界情况,但*不是*这次 ecall 返回依赖的路径。
 *   2. sepc 这个 CSR（write_sepc())和 trap_saved_user_sp 这个全局
 *      变量——这次 ecall 马上要走的 trap_entry.S "ld 恢复寄存器->
 *      sret"路径*只*依赖这两处,不读 tf 的任何字段。
 * 完整推导（包括最早漏掉步骤 2 时用 `qemu-system-riscv64 -d int`
 * 抓出来的真实故障现象)见上面 write_sepc() 定义处的大段注释,这里
 * 不重复。
 *
 * 调用者旧的两个映射必须先 pagetable_unmap() 撤销、kfree_page() 释放
 * 旧物理页,再调 map_user_prog() 重新映射同样的虚拟地址——跟 x86_64
 * 版本同一个理由（避免同一个虚拟地址被映射两次造成的不一致状态和
 * 内存泄漏),不重复展开。 */
int sys_exec(void)
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

    p->tf->sepc = USER_PROG_VADDR;
    p->tf->sp = user_stack_top;

    write_sepc(USER_PROG_VADDR);
    trap_saved_user_sp = user_stack_top;

    return 0;
}

/* sys_wait()：在 proc_table 里找一个"调用者的 ZOMBIE 子进程"——跟
 * x86_64 版本同一个简化（找不到就直接返回 -1,不阻塞),同一个资源
 * 回收范围（只回收内核栈物理页,页表本身不回收),不重复展开。 */
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
