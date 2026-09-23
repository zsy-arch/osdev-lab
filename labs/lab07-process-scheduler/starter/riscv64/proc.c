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

/* TODO 1：实现 read_sepc/write_sepc。
 *
 * 为什么：sepc 是 CSR，不是内存位置，读写它必须经过 csrr/csrw 指令，
 * C 代码不能像访问全局变量一样直接读写——这两个函数把这层内联汇编包起
 * 来，让 proc.c 剩下的部分可以用普通函数调用的方式处理 sepc。write_sepc()
 * 具体为什么在本 Lab 是必须的（不仅仅是"顺手包一层"），见本函数定义处
 * 原本的大段注释——sys_exec() 依赖它直接写 CSR，不能只写 g_current->tf。
 *
 * 提示：
 * static uintptr_t read_sepc(void)
 * {
 *     uintptr_t value;
 *     __asm__ volatile("csrr %0, sepc" : "=r"(value));
 *     return value;
 * }
 *
 * static void write_sepc(uintptr_t value)
 * {
 *     __asm__ volatile("csrw sepc, %0" : : "r"(value));
 * }
 */

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
 * USER_PROG_VADDR 重新开始执行,用户栈是全新映射的那一页"。（这段注释
 * 解释的是 write_sepc() 为什么必须存在、必须被 sys_exec() 调用——具体
 * 函数体见上面 TODO 1。） */

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

/* TODO 2：实现 proc_init/proc_current。
 *
 * 为什么：proc_init() 把整张进程表清成 PROC_UNUSED（内核启动时调用一次，
 * kernel_main.c 里在第一次 proc_alloc() 之前),g_current 初始化成 NULL
 * （此刻还没有任何进程在跑,scheduler() 还没开始),proc_current() 是
 * 给别的文件（目前只有 trap.c 的 timer_interrupt_handler())用的只读
 * 访问器,避免让 g_current 这个 static 变量直接 extern 出去。
 *
 * 提示：
 * void proc_init(void)
 * {
 *     for (int i = 0; i < NPROC; i++) {
 *         proc_table[i].state = PROC_UNUSED;
 *     }
 *     g_current = NULL;
 * }
 *
 * struct proc *proc_current(void)
 * {
 *     return g_current;
 * }
 */

/* 把内嵌的 user_prog.bin 拷贝进一个新分配的物理页、映射到 pagetable
 * 里的 USER_PROG_VADDR，再分配+映射一页用户栈——proc_alloc()（创建
 * 全新进程）和 sys_exec()（重置已有进程的地址空间）共用这段逻辑，跟
 * x86_64 版本同名函数完全一样的复用关系，不重复展开。
 *
 * 返回值是新映射好的用户栈顶（USER_STACK_VADDR）。 */
/* TODO 3：实现 map_user_prog(pagetable)。
 *
 * 为什么：proc_alloc()（全新进程）和 sys_exec()（重置已有进程的地址
 * 空间）都需要"分配一页物理内存、拷入内嵌的 user_prog.bin、映射到
 * USER_PROG_VADDR，再分配+映射一页用户栈"这套完全一样的步骤——写成
 * 共用函数避免两处重复。返回值是新映射好的用户栈顶（USER_STACK_VADDR），
 * 调用者用它设置 trapframe.sp。
 *
 * 提示：
 * static uintptr_t map_user_prog(uintptr_t pagetable)
 * {
 *     void *prog_page_phys = kalloc_page();
 *     if (prog_page_phys == NULL) {
 *         panic("map_user_prog: kalloc_page() failed for program page");
 *     }
 *     uint8_t *prog_page_kva =
 *         (uint8_t *)((uintptr_t)prog_page_phys + KERNEL_VIRT_BASE);
 *
 *     size_t prog_len = (size_t)(__user_prog_end - __user_prog_start);
 *     if (prog_len > PAGE_SIZE) {
 *         panic("map_user_prog: embedded user program does not fit in one page");
 *     }
 *     memset(prog_page_kva, 0, PAGE_SIZE);
 *     memcpy(prog_page_kva, __user_prog_start, prog_len);
 *
 *     pagetable_map(pagetable, USER_PROG_VADDR, (uintptr_t)prog_page_phys,
 *                   PTE_FLAG_USER | PTE_FLAG_EXECUTABLE);
 *
 *     void *stack_page_phys = kalloc_page();
 *     if (stack_page_phys == NULL) {
 *         panic("map_user_prog: kalloc_page() failed for stack page");
 *     }
 *     uint8_t *stack_page_kva =
 *         (uint8_t *)((uintptr_t)stack_page_phys + KERNEL_VIRT_BASE);
 *     memset(stack_page_kva, 0, PAGE_SIZE);
 *
 *     pagetable_map(pagetable, USER_STACK_VADDR - PAGE_SIZE,
 *                   (uintptr_t)stack_page_phys,
 *                   PTE_FLAG_USER | PTE_FLAG_WRITABLE);
 *
 *     return USER_STACK_VADDR;
 * }
 */

/* 在 proc_table 里找一个 PROC_UNUSED 的槽位，把它变成一个"页表/内核栈/
 * trapframe/context 都已经就位，但用户地址空间还完全没有映射任何东西"
 * 的半成品进程——proc_alloc()（全新进程)和 sys_fork()（子进程)共用这段
 * 骨架,跟 x86_64 版本同名函数同一个复用关系,不重复展开。
 *
 * 找不到空槽位返回 NULL,调用者负责处理。 */
/* TODO 4：实现 proc_alloc_skeleton。
 *
 * 为什么：proc_alloc()（全新进程）和 sys_fork()（子进程）都需要"在
 * proc_table 里找一个空槽位，分配好页表/内核栈/trapframe/context 指针，
 * 但用户地址空间还完全没有映射任何东西"这个共同的骨架——两者的差异
 * 只在"骨架之后往用户地址空间里放什么"（proc_alloc() 放全新的
 * user_prog.bin；sys_fork() 逐页拷贝父进程已映射的内容），骨架本身
 * 完全一样，写成共用函数避免重复。
 *
 * 内核栈布局（从高地址到低地址）：
 *   [kstack_phys + PAGE_SIZE]                    <- 栈顶
 *   struct trapframe                              <- p->tf 指向这里
 *   struct context（context->ra 填 trap_return）  <- p->context 指向这里
 * trap_return（swtch.S）执行时 sp 停在 context 13 个字段被 ld 完之后的
 * 位置，正好落在 trapframe 的起始地址——两者紧邻，不是巧合，是
 * proc_alloc_skeleton() 这里手工摆的。p->tf/p->context 存的是*内核虚拟
 * 地址*（kstack_phys 加上 KERNEL_VIRT_BASE 偏移），不是物理地址——
 * pagetable_activate() 切换页表之后，物理地址不再是合法的可解引用地址，
 * 必须先换算成内核虚拟地址才能真正 memset/访问字段。
 *
 * 找不到空槽位返回 NULL，调用者负责处理（sys_fork() 遇到这种情况返回
 * -1，不 panic——用户程序完全可能触发的正常情况）。
 *
 * 提示：
 * static struct proc *proc_alloc_skeleton(void)
 * {
 *     struct proc *p = NULL;
 *     for (int i = 0; i < NPROC; i++) {
 *         if (proc_table[i].state == PROC_UNUSED) {
 *             p = &proc_table[i];
 *             break;
 *         }
 *     }
 *     if (p == NULL) {
 *         return NULL;
 *     }
 *
 *     p->pid = g_next_pid++;
 *     p->parent_pid = -1;
 *     p->exit_code = 0;
 *     p->name[0] = '\0';
 *
 *     p->pagetable = pagetable_create();
 *     pagetable_copy_kernel_range(p->pagetable);
 *
 *     p->kstack_phys = kalloc_page();
 *     if (p->kstack_phys == NULL) {
 *         panic("proc_alloc_skeleton: kalloc_page() failed for kernel stack");
 *     }
 *
 *     uintptr_t kstack_kva_top =
 *         (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
 *     p->tf = (struct trapframe *)(kstack_kva_top - sizeof(struct trapframe));
 *     p->context = (struct context *)((uintptr_t)p->tf - sizeof(struct context));
 *
 *     memset(p->tf, 0, sizeof(struct trapframe));
 *     memset(p->context, 0, sizeof(struct context));
 *     p->context->ra = (uint64_t)trap_return;
 *
 *     return p;
 * }
 */

/* 创建一个全新进程：骨架 + 内嵌 user_prog.bin 的一份全新拷贝——跟
 * x86_64 版本同名函数同一个角色(kernel_main.c 用它创建初始进程;
 * sys_fork() 不用它,理由跟 x86_64 版本一致,不重复展开)。 */
/* TODO 5：实现 proc_alloc。
 *
 * 为什么：骨架（proc_alloc_skeleton())只分配好页表/内核栈/trapframe/
 * context，用户地址空间还完全没映射任何东西——proc_alloc() 补上
 * "调 map_user_prog() 把内嵌 user_prog.bin 映射进去，再把 trapframe
 * 的 sepc/sp/sstatus 设成用户程序第一次进入时应有的值"这一步，让
 * kernel_main.c 拿到的进程真的可以被 scheduler() 跑起来。sstatus 用
 * INITIAL_SSTATUS 常量（不是读当前 CSR）——因为这是进程*第一次*进入
 * 用户态，还没有"当前 sstatus"可以继承，必须显式指定 SPP=0（sret 后
 * 降到 U 模式）等位。
 *
 * 提示：
 * struct proc *proc_alloc(void)
 * {
 *     struct proc *p = proc_alloc_skeleton();
 *     if (p == NULL) {
 *         return NULL;
 *     }
 *
 *     uintptr_t user_stack_top = map_user_prog(p->pagetable);
 *
 *     p->tf->sepc = USER_PROG_VADDR;
 *     p->tf->sp = user_stack_top;
 *     p->tf->sstatus = INITIAL_SSTATUS;
 *
 *     p->state = PROC_RUNNABLE;
 *     return p;
 * }
 */

/* scheduler() 自己也是一条"内核执行流"——跟 x86_64 版本 g_scheduler_
 * context 同一个存在理由、同一个类型要求（必须是 struct context *,
 * 不能是 struct context 本身,理由见 x86_64 版本对应注释里那段
 * -Wincompatible-pointer-types 的真实编译错误叙述,riscv64 这边是
 * 同一个 swtch() 签名、同一个类型约束,不重复展开)。 */
static struct context *g_scheduler_context;

/* TODO 6：实现 scheduler。
 *
 * 为什么：这是本 Lab 唯一的"内核主循环"，noreturn。每次找到一个
 * RUNNABLE 的进程，进去之前必须按顺序做四件事：
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
 * sys_exit_proc() 里设置过),再继续 for 循环找下一个。回到这里时
 * g_current 未必还是 p（sys_exit_proc() 会把一个进程标成 ZOMBIE 之后
 * 直接切回调度器),这里统一用 p 判断,不用 g_current。
 *
 * 提示：
 * void scheduler(void)
 * {
 *     for (;;) {
 *         for (int i = 0; i < NPROC; i++) {
 *             struct proc *p = &proc_table[i];
 *             if (p->state != PROC_RUNNABLE) {
 *                 continue;
 *             }
 *
 *             p->state = PROC_RUNNING;
 *             g_current = p;
 *
 *             uintptr_t kstack_kva_top =
 *                 (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE + PAGE_SIZE;
 *             trap_kernel_sp_top = kstack_kva_top;
 *             pagetable_activate(p->pagetable);
 *
 *             swtch(&g_scheduler_context, p->context);
 *
 *             g_current = NULL;
 *             if (p->state == PROC_RUNNING) {
 *                 p->state = PROC_RUNNABLE;
 *             }
 *         }
 *     }
 * }
 */

/* TODO 7：实现 yield。
 *
 * 为什么：这是一个进程主动/被动让出 CPU 的唯一入口——trap.c 的
 * timer_interrupt_handler() 在定时器中断里调它实现抢占式调度。
 * g_current 可能是 NULL（内核自己的执行流,不是任何进程,被定时器
 * 打断时不需要也不能 yield())，必须先判断。真正的让出动作只有两步：
 * 标回 RUNNABLE（不是 ZOMBIE、不是继续 RUNNING——调度器下一轮扫描
 * 到它时要能重新选中它),然后 swtch(&p->context, g_scheduler_context)
 * 切回 scheduler() 当时调 swtch(&g_scheduler_context, p->context) 之后
 * 的那个点（scheduler() for 循环体的中间),不是重新进入 scheduler()
 * 函数本身——这正是 swtch() 保存/恢复 ra 的意义。
 *
 * 提示：
 * void yield(void)
 * {
 *     struct proc *p = g_current;
 *     if (p == NULL) {
 *         return;
 *     }
 *
 *     p->state = PROC_RUNNABLE;
 *     swtch(&p->context, g_scheduler_context);
 * }
 */

/* TODO 8：实现 sys_exit_proc。
 *
 * 为什么：SYS_EXIT 系统调用最终落到的地方。标成 ZOMBIE 而不是直接
 * 回收（state/exit_code)——sys_wait() 需要在进程真的从 proc_table 里
 * 消失之前,先读到它的退出码,回收工作（kfree_page(kstack_phys))延后
 * 到 sys_wait() 里做,不在这里做。swtch() 之后紧跟一个 panic()：
 * ZOMBIE 状态的进程永远不会被 scheduler() 的 for 循环重新选中（它
 * 只挑 PROC_RUNNABLE),所以这次 swtch() 调用理论上不应该再返回——
 * 如果真的返回了,说明调度器逻辑本身出了错,应该立刻可见地崩溃,而不是
 * 静默地继续执行一段"不应该存在"的代码路径。
 *
 * 提示：
 * void sys_exit_proc(int64_t code)
 * {
 *     struct proc *p = g_current;
 *     if (p == NULL) {
 *         panic("sys_exit_proc: called with no current process");
 *     }
 *
 *     p->exit_code = code;
 *     p->state = PROC_ZOMBIE;
 *
 *     swtch(&p->context, g_scheduler_context);
 *
 *     panic("sys_exit_proc: swtch() returned into a ZOMBIE process, should be unreachable");
 * }
 */

/* TODO 9：实现 fork_copy_page。
 *
 * 为什么：本 Lab 的 fork() 是"立即完整复制"，不是写时复制（COW)——
 * 给子进程分配一页*全新*物理内存,把父进程在 vaddr 处那一页的内容
 * 整页拷过去,再按 flags 映射到子进程页表的*同一个*虚拟地址（不能是
 * 别的地址——子进程的 user_prog.bin 代码里所有绝对地址引用都是按
 * USER_PROG_VADDR/USER_STACK_VADDR 编译的,挪到别的虚拟地址会直接
 * 访问错误的内存)。父子进程读写各自的物理页互不影响,这正是 fork()
 * 语义"父子进程从此各自独立"的具体实现方式。kalloc_page()/pagetable_
 * lookup() 返回的是*物理地址*,要先加上 KERNEL_VIRT_BASE 换算成内核
 * 能直接解引用的虚拟地址才能 memcpy——跟本文件其它地方处理物理地址
 * 的方式一致。
 *
 * 提示：
 * static void fork_copy_page(uintptr_t child_pagetable, uintptr_t parent_pagetable,
 *                             uintptr_t vaddr, uint32_t flags)
 * {
 *     uintptr_t parent_phys = pagetable_lookup(parent_pagetable, vaddr);
 *     if (parent_phys == 0) {
 *         panic("fork_copy_page: parent has no mapping at expected vaddr");
 *     }
 *
 *     void *child_phys = kalloc_page();
 *     if (child_phys == NULL) {
 *         panic("fork_copy_page: kalloc_page() failed");
 *     }
 *
 *     memcpy((void *)((uintptr_t)child_phys + KERNEL_VIRT_BASE),
 *            (void *)(parent_phys + KERNEL_VIRT_BASE), PAGE_SIZE);
 *
 *     pagetable_map(child_pagetable, vaddr, (uintptr_t)child_phys, flags);
 * }
 */

/* TODO 10：实现 sys_fork。
 *
 * 为什么：跟 x86_64 版本不同的签名/取值来源,完整原因见 proc.h
 * sys_fork/sys_exec 声明处那段大注释,这里只写具体步骤：
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
 * 找不到空闲进程表槽位返回 -1，不 panic——用户程序完全可能触发的
 * 正常情况。
 *
 * 提示：
 * int sys_fork(void)
 * {
 *     struct proc *parent = g_current;
 *     if (parent == NULL) {
 *         panic("sys_fork: called with no current process");
 *     }
 *
 *     struct proc *child = proc_alloc_skeleton();
 *     if (child == NULL) {
 *         return -1;
 *     }
 *
 *     fork_copy_page(child->pagetable, parent->pagetable, USER_PROG_VADDR,
 *                    PTE_FLAG_USER | PTE_FLAG_EXECUTABLE);
 *     fork_copy_page(child->pagetable, parent->pagetable,
 *                    USER_STACK_VADDR - PAGE_SIZE,
 *                    PTE_FLAG_USER | PTE_FLAG_WRITABLE);
 *
 *     child->tf->sepc = read_sepc();
 *     child->tf->sp = trap_saved_user_sp;
 *     child->tf->sstatus = INITIAL_SSTATUS;
 *
 *     child->parent_pid = parent->pid;
 *     child->state = PROC_RUNNABLE;
 *
 *     return child->pid;
 * }
 */

/* TODO 11：实现 sys_exec。
 *
 * 为什么：本 Lab 范围内的简化语义（见本文件顶部模块注释)——把
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
 * 旧物理页,再调 map_user_prog() 重新映射同样的虚拟地址——避免同一个
 * 虚拟地址被映射两次造成的不一致状态和内存泄漏。
 *
 * 提示：
 * int sys_exec(void)
 * {
 *     struct proc *p = g_current;
 *     if (p == NULL) {
 *         panic("sys_exec: called with no current process");
 *     }
 *
 *     uintptr_t old_prog_phys =
 *         pagetable_unmap(p->pagetable, USER_PROG_VADDR);
 *     if (old_prog_phys != 0) {
 *         kfree_page((void *)old_prog_phys);
 *     }
 *
 *     uintptr_t old_stack_phys =
 *         pagetable_unmap(p->pagetable, USER_STACK_VADDR - PAGE_SIZE);
 *     if (old_stack_phys != 0) {
 *         kfree_page((void *)old_stack_phys);
 *     }
 *
 *     uintptr_t user_stack_top = map_user_prog(p->pagetable);
 *
 *     p->tf->sepc = USER_PROG_VADDR;
 *     p->tf->sp = user_stack_top;
 *
 *     write_sepc(USER_PROG_VADDR);
 *     trap_saved_user_sp = user_stack_top;
 *
 *     return 0;
 * }
 */

/* TODO 12：实现 sys_wait。
 *
 * 为什么：在 proc_table 里找一个"调用者的 ZOMBIE 子进程"。本 Lab 范围
 * 内的简化：找不到就直接返回 -1,不阻塞（不把调用者挂起等子进程退出
 * ——真正的阻塞式 wait() 需要调度器支持"等待队列",超出本 Lab 范围)。
 * 资源回收范围也简化：只回收内核栈物理页（kfree_page(p->kstack_
 * phys))，用户地址空间的两页（程序页+栈页)、页表本身都不回收——
 * 这些内存永久泄漏是本 Lab 明确接受的简化,不是遗漏（真实内核需要
 * proc_alloc_skeleton() 对称的完整拆卸逻辑)。state 改回 PROC_
 * UNUSED,让这个槽位可以被将来的 proc_alloc()/sys_fork() 重新使用。
 *
 * 提示：
 * int sys_wait(int64_t *exit_code_out)
 * {
 *     struct proc *caller = g_current;
 *     if (caller == NULL) {
 *         panic("sys_wait: called with no current process");
 *     }
 *
 *     for (int i = 0; i < NPROC; i++) {
 *         struct proc *p = &proc_table[i];
 *         if (p->state == PROC_ZOMBIE && p->parent_pid == caller->pid) {
 *             if (exit_code_out != NULL) {
 *                 *exit_code_out = p->exit_code;
 *             }
 *             int pid = p->pid;
 *
 *             kfree_page(p->kstack_phys);
 *             p->state = PROC_UNUSED;
 *
 *             return pid;
 *         }
 *     }
 *
 *     return -1;
 * }
 */
