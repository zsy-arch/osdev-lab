/* Lab7-9 riscv64：进程表 + 调度器 + fork/exec/wait/exit 的具体实现。跟
 * x86_64 版本（../x86_64/proc.c）同一份整体骨架（同样的 proc_table 线性
 * 扫描、同样朴素的轮转调度、同样"fork 就是整页复制不做 COW"、同样"从
 * 磁盘加载 ELF"的 exec、同样"忙等 + yield 循环"实现的阻塞 wait)，设计
 * 动机跟 x86_64 版本完全一致的部分不重复展开，只写 riscv64 寄存器/CSR
 * 相关的具体差异——见本文件各处注释跟 x86_64 版本对应位置的对比。
 *
 * Lab9 之前（Lab7/Lab8）这里是"exec 只能重新加载内嵌 user_prog.bin 自己"
 * 那个简化版本，靠 read_sepc()/write_sepc() 和 trap_saved_user_sp 全局
 * 变量直接改写 CPU 全局存储。那个设计已经被 Lab9 推翻——完整原因见 proc.h
 * sys_fork/sys_exec 声明处的大注释和 trap_entry.S 顶部模块注释：sepc/
 * 用户 sp 现在活在每个进程自己内核栈上的 TF_SEPC/TF_SP 两个槽位里，
 * 不再是全局可寻址的单一存储，sys_fork()/sys_exec() 因此必须像 x86_64
 * 一样接收指向这两个槽位的指针，不能再直接读写 CSR/全局变量。
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

/* 下一个分配的 pid，从 1 开始——跟 x86_64 版本同一个理由（0 留作
 * "无效/未分配"的哨兵值),不重复展开。 */
static int g_next_pid = 1;

/* 当前正在 RUNNING 的进程——跟 x86_64 版本同一个理由，不重复展开。 */
static struct proc *g_current;

extern void swtch(struct context **old, struct context *new);
extern void trap_return(void);

/* trap_kernel_sp_top：定义在 trap.c（只被 trap_entry.S 用汇编 `la`/`ld`
 * 直接读),scheduler() 每次 swtch() 进某个进程之前要写它,所以需要一个
 * extern 声明——跟 Lab7/Lab8 同一个理由，不重复展开。
 *
 * 这里*不再*有 trap_saved_user_sp 的 extern——Lab9 把它从 trap.c 删掉了
 * （原来的用户 sp 现在活在每个进程自己的 TF_SP 槽位里，不再是一个全系统
 * 共享的全局变量),proc.c 也就不再需要直接碰它,sys_fork()/sys_exec() 改
 * 成通过 user_rip_slot/user_rsp_slot 两个参数间接读写,见下面两个函数。 */
extern uint64_t trap_kernel_sp_top;

/* 跟 kernel_main.c/pagetable.c 同名常量必须保持一致（本课程一贯的
 * "没有单一数据源、需要人肉对齐"的手工契约,见那两个文件里对应的
 * 注释）——这里需要它是因为 kalloc_page()/kalloc_pages() 返回的是物理
 * 地址，本文件需要先加上这个偏移才能把它当指针解引用（memcpy 用户页/
 * 内核栈内容)。 */
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull

/* Lab9：初始进程的程序路径。跟 x86_64 版本同一个理由（去掉"内核镜像里
 * 带着用户程序"这个耦合，改从磁盘加载 /init），不重复展开。 */
#define INIT_PATH "/init"

/* tf->sstatus 的常量值：SPP=0（回到 U-mode）/ SPIE=1（bit 5，sret 之后
 * 全局中断使能恢复)/ SUM=1（bit 18，允许 S-mode 代码访问 U-mode 页
 * ——本 Lab 的 sys_write() 需要直接解引用用户传入的指针，没有这一位
 * 会触发 load/store page fault,Lab6 enter_user_mode 开发时期真实踩过
 * 这个坑，完整 narrative 见 Lab6 trap_entry.S 对应注释)——这三个具体
 * 数值跟 Lab6 已删除的 enter_user_mode 曾经硬编码的常量完全相同,只是
 * 那个函数已经在 Lab7 被 trap_return（swtch.S)统一取代,这个常量需要
 * 一个新家,搬到这里：proc_alloc()/sys_fork() 是本 Lab 唯一"从零构造
 * 一份 trapframe 交给一个即将第一次运行的进程"的两个地方,天然是这个
 * 常量该住的地方。 */
#define INITIAL_SSTATUS ((1ull << 5) | (1ull << 18))

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
 * 这一节替换掉了 Lab7/Lab8 的 map_user_prog() 和 fork_copy_page()——那两个
 * 函数里"用户地址空间 = USER_PROG_VADDR 那一页 + USER_STACK_VADDR 那一页"
 * 是写死的。Lab9 的地址空间由 ELF 文件决定，页数和虚拟地址都是运行时才
 * 知道的，那个写法没法继续。跟 x86_64 版本同一份设计、同一份实现（除了
 * KERNEL_VIRT_BASE 的具体数值），不重复展开设计动机，接口说明见 proc.h。 */

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
 * 真的走到这里说明那个预算算错了，是内核自己的 bug。
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
 * 把内核映射拆了会在下一条指令就崩。
 *
 * 不刷 TLB——两个调用点都不需要：exec 那边紧接着会 pagetable_activate()
 * （换根隐式刷全表）；sys_wait 那边被清的是僵尸进程的页表，它永远不会
 * 再被 activate。
 *
 * pagetable_unmap() 返回 0 表示 upages[] 记录的虚拟地址在页表里查不到
 * 映射——两者不一致，说明有人绕过 uvm_track/uvm_clear 直接改了页表，
 * 或者同一个虚拟地址被 track 了两次，直接 panic。
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

    /* TODO 3（Lab9 新增）：三件事，跟 x86_64 版本同一处 TODO 完全对应，
     * 完整理由见那边，这里不重复：
     *
     *   1. p->nupages = 0——槽位复用，上一个进程的 upages[] 还在里面，
     *      不清的话新进程会以为自己拥有前任的那些页。
     *   2. memset(p->ofile, 0, sizeof(p->ofile))——Lab8 就有的老需求，
     *      清空打开文件表，理由跟之前完全一样（信息泄露）。
     *   3. 页表按槽位复用，只在第一次使用这个槽位时创建：
     *      if (p->pagetable == 0) {
     *          p->pagetable = pagetable_create();
     *          pagetable_copy_kernel_range(p->pagetable);
     *      }
     *      能这么做的前提是 sys_wait() 在把槽位标回 UNUSED 之前调用了
     *      uvm_clear()——用户页全部拆掉，剩下的是一棵"内核范围映射还在、
     *      用户范围全空"的空树，正是这里想要的初始状态。
     */

    /* TODO 4（Lab9 新增，跟 x86_64 版本同一份理由）：内核栈从 1 页变成
     * PROC_KSTACK_PAGES 页——exec 那条调用链比 Lab7/Lab8 深得多，见
     * proc.h 里那个宏的注释。必须用 kalloc_pages() 拿*连续*的页——栈是
     * 一段连续地址，两次 kalloc_page() 拿到的两页没有任何理由相邻。
     *
     * 提示：
     * p->kstack_phys = kalloc_pages(PROC_KSTACK_PAGES);
     * if (p->kstack_phys == NULL) { panic(...); }
     */
    p->kstack_phys = NULL;

    /* 内核栈布局，跟 x86_64 版本同一个"从高地址到低地址"摆法：
     *   [kstack_phys + PROC_KSTACK_PAGES * PAGE_SIZE]  <- 栈顶
     *   struct trapframe                          <- p->tf 指向这里
     *   struct context（context->ra 填 trap_return） <- p->context 指向这里
     * trap_return（swtch.S)执行时 sp 停在 context 13 个字段被 ld 完之后
     * 的位置，正好落在 trapframe 的起始地址——见 swtch.S trap_return
     * 那段注释里"紧邻"关系的具体落点。
     *
     * p->tf/p->context 存的是*内核虚拟地址*（kstack_phys+KERNEL_VIRT_
     * BASE 之后再算的偏移),不是物理地址——跟 x86_64 版本同一个理由
     * （pagetable_activate() 之后物理地址不再是合法的可解引用地址),
     * 不重复展开。
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
    p->context->ra = (uint64_t)trap_return;

    return p;
}

/* 创建初始进程：骨架 + 从磁盘加载 /init——跟 x86_64 版本同一个角色（本
 * Lab 唯一的"从无到有"的进程创建入口，只在启动时被 kernel_main.c 调用
 * 一次;sys_fork() 不用它,理由跟 x86_64 版本一致),不重复展开。
 *
 * TODO 6（Lab9 新增，body 完全重写，跟 x86_64 版本同一份实现）：
 *   1. fd 0/1/2 接到串口——p->ofile[0].type = FD_CONSOLE，writable=0；
 *      p->ofile[1]/[2] 同样 FD_CONSOLE，writable=1。
 *   2. 调 exec_load(p, INIT_PATH, NULL, &entry, &sp)（argv 给 NULL：
 *      初始进程没有命令行参数）加载 /init，失败就 panic——这是启动
 *      路径，没有 /init 意味着根文件系统不对，继续跑下去没有任何意义。
 *   3. 用 exec_load() 填出来的 entry/sp（不再是写死的 USER_PROG_VADDR/
 *      map_user_prog() 返回值）初始化 p->tf->sepc/sp/sstatus。
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
 * p->tf->sepc = entry;
 * p->tf->sp = sp;
 * p->tf->sstatus = INITIAL_SSTATUS;
 * memcpy(p->name, "init", 5);
 */
struct proc *proc_alloc(void)
{
    struct proc *p = proc_alloc_skeleton();
    if (p == NULL) {
        return NULL;
    }

    (void)INIT_PATH;
    (void)INITIAL_SSTATUS;
    panic("proc_alloc: TODO 6 未实现");

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

            /* TODO 7（Lab9，跟 proc_alloc_skeleton() 里 TODO 5 同一处
             * 修改：内核栈从 1 页变成 PROC_KSTACK_PAGES 页，这里的栈顶
             * 换算也要跟着改，否则 trap_kernel_sp_top 会指向栈中间而
             * 不是栈顶）：
             *
             * uintptr_t kstack_kva_top = (uintptr_t)p->kstack_phys +
             *                            KERNEL_VIRT_BASE +
             *                            PROC_KSTACK_PAGES * PAGE_SIZE;
             */
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
 * exit_proc() 顶部注释一致,不重复展开)。
 *
 * TODO 8（Lab9 新增，跟 x86_64 版本同一份实现、同一个理由）：进程退出时
 * 释放它打开的管道端——exit 隐含"关闭所有 fd"，否则管道另一端会永久卡
 * 在 read() 上等一个再也不会归零的写端计数（比如 `cat x | grep y` 里
 * cat 从来不显式 close 自己的 fd 1，靠这里替它关）。
 *
 * 位置必须在 swtch() *之前*——swtch() 之后这个函数再也不会被执行到。
 *
 * 提示：
 * for (int fd = 0; fd < NOFILE; fd++) {
 *     if (p->ofile[fd].type == FD_PIPE) {
 *         pipe_close(p->ofile[fd].pipe, p->ofile[fd].writable);
 *     }
 *     p->ofile[fd].type = FD_NONE;
 *     p->ofile[fd].inum = 0;
 *     p->ofile[fd].off = 0;
 *     p->ofile[fd].pipe = NULL;
 *     p->ofile[fd].writable = 0;
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

/* TODO 9（Lab9 新增，proc.h 已经声明）：把 src 的每一个用户页复制一份给
 * dst——新分配物理页、整页拷内容、用*相同的虚拟地址和相同的权限*建立
 * 映射,再调 uvm_track() 登记到 dst 的 upages[]。成功返回 0,物理内存
 * 不够返回 -1（此时要先 uvm_clear(dst) 清掉已经拷好的页,让调用者的
 * 错误处理只需要一句 return -1,不用自己收拾半成品）。
 *
 * 内容通过内核偏移映射拷贝（src_phys/dst_phys 各自加 KERNEL_VIRT_BASE），
 * 不通过用户虚拟地址——此刻活跃的页表是父进程的，子进程那些新映射还没
 * 生效；就算能写，子进程的代码段页也可能是只读的。
 *
 * pagetable_lookup() 返回 0 表示 src->upages[] 记录的虚拟地址在页表里
 * 查不到——跟 uvm_clear() 同一个不变量,直接 panic。
 *
 * 提示：
 * for (int i = 0; i < src->nupages; i++) {
 *     uintptr_t vaddr = src->upages[i].vaddr;
 *     uint32_t flags = src->upages[i].flags;
 *     uintptr_t src_phys = pagetable_lookup(src->pagetable, vaddr);
 *     if (src_phys == 0) { panic(...); }
 *     void *dst_phys = kalloc_page();
 *     if (dst_phys == NULL) { uvm_clear(dst); return -1; }
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

/* TODO 10（Lab9，signature 和 body 都跟 Lab7/8 完全不同,完整原因见
 * proc.h sys_fork/sys_exec 声明处那段大注释,这里不重复展开,只写具体
 * 步骤）：
 *
 * sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
 *          uintptr_t *caller_frame)——三个参数都是"指向父进程这次 ecall
 * 陷入时开在它自己内核栈上那份轻量帧"的指针（trap.c ecall_handler 传
 * 进来的），不是 struct trapframe。
 *
 *   1. proc_alloc_skeleton() 拿一份骨架，然后 uvm_copy(child, parent)
 *      逐页拷贝父进程*全部*已映射的用户页到子进程——失败要还内核栈：
 *          if (uvm_copy(child, parent) < 0) {
 *              kfree_pages(child->kstack_phys, PROC_KSTACK_PAGES);
 *              child->kstack_phys = NULL;
 *              child->state = PROC_UNUSED;
 *              return -1;
 *          }
 *   2. 用 *user_rip_slot / *user_rsp_slot 填 child->tf->sepc / sp，
 *      sstatus 用 INITIAL_SSTATUS。
 *   3. 把 caller_frame 里 ra/t0-t6/a1-a7/s0-s11/gp/tp 这些字段原样抄进
 *      child->tf 对应字段——下标跟 trap_entry.S TF_RA/TF_T0.../TF_S0...
 *      （除以 8 之后）是同一份轻量帧布局的另一份手工镜像，必须保持一致。
 *      a0（下标 8）故意跳过——child->tf->a0 保持 memset 出来的 0，这正
 *      是 fork() 在子进程里返回 0 的全部实现。具体下标：
 *        ra=[0] t0=[1] t1=[2] t2=[3] t3=[4] t4=[5] t5=[6] t6=[7]
 *        a1=[9] a2=[10] a3=[11] a4=[12] a5=[13] a6=[14] a7=[15]
 *        s0=[18] s1=[19] s2=[20] s3=[21] s4=[22] s5=[23]
 *        s6=[24] s7=[25] s8=[26] s9=[27] s10=[28] s11=[29]
 *        gp=[30] tp=[31]
 *      漏掉这一步的真实故障（子进程 ret 到地址 0/后来的 s0 帧指针
 *      写故障）完整叙述见 proc.h 对应声明处的大注释。
 *   4. memcpy(child->ofile, parent->ofile, sizeof(child->ofile))——
 *      拷贝打开文件表，然后对每个 FD_PIPE 的 fd 调 pipe_dup() 增加引用
 *      计数（memcpy 拷走的是指针，不会自动涨计数）。
 *   5. child->parent_pid = parent->pid; child->state = PROC_RUNNABLE;
 *      return child->pid;
 *
 * 找不到空闲进程表槠位返回 -1，不 panic。 */
int sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
             uintptr_t *caller_frame)
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
    (void)caller_frame;
    panic("sys_fork: TODO 10 未实现");
}

/* TODO 11（Lab9，signature 和 body 都跟 Lab7/8 完全不同——旧版本的
 * sys_exec(void) 只能"重新加载自己"，见本文件顶部模块注释；新版本
 * 才第一次名副其实。真正的加载工作全在 exec_load() 里（exec.c，两个
 * 架构共用）,本函数只负责三件架构相关的事：取参数、调加载器、把"这次
 * ecall 返回后恢复到哪里"改写成新程序的入口）：
 *
 * sys_exec(const char *path, char *const argv[], uintptr_t *user_rip_slot,
 *          uintptr_t *user_rsp_slot)
 *
 *   1. path == NULL 直接返回 -1。
 *   2. 在调 exec_load() *之前*，把 path 的 basename（去掉目录部分）拷进
 *      一个内核局部变量 name_buf——path 是指向调用者*旧*地址空间的用户
 *      指针，exec_load() 成功后会拆掉旧映射，之后 path 就是悬空指针，
 *      不能再碰它。完整踩坑过程见 x86_64 版本同一处的大注释。
 *   3. uintptr_t entry = 0, sp = 0;
 *      if (exec_load(p, path, argv, &entry, &sp) < 0) { return -1; }
 *      ——失败时地址空间完全没动，直接返回 -1，用户程序（sh）借此打
 *      "command not found"。
 *   4. 把 name_buf 拷进 p->name（exec_load() 已经成功提交，p 是内核
 *      自己的结构体，不受地址空间切换影响）。
 *   5. *user_rip_slot = entry; *user_rsp_slot = sp;——顺序不能提前：
 *      必须在 exec_load() 成功之后才写，先写 slot 再加载会让一次失败
 *      的 exec 把返回地址指向一个不存在的入口。
 *   6. return 0;
 */
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

/* TODO 12（Lab9，body 完全重写，跟 x86_64 版本同一份实现、同一个理由,
 * 两处变化：
 *
 *   一、变成阻塞的——shell 的主循环是 fork → exec → wait，如果 wait
 *      找不到 ZOMBIE 子进程就立刻返回 -1，shell 会在上一条命令还没跑完
 *      时就打下一个提示符。改成"没找到就 yield()，被调度回来再找一遍"
 *      的忙等循环。循环之前必须先确认"我到底有没有子进程"——没有子
 *      进程的话 yield 循环永远等不到东西，会永久卡住（死锁）。
 *   二、回收用户页——找到 ZOMBIE 子进程之后，先 uvm_clear(p) 把用户页
 *      还给 kalloc，再 kfree_pages(p->kstack_phys, PROC_KSTACK_PAGES)
 *      回收内核栈（PROC_KSTACK_PAGES 页，不再是 1 页），页表交给槠位
 *      复用不释放。
 *
 * 提示（外层 for(;;) 循环）：
 * for (;;) {
 *     int have_child = 0;
 *     for (int i = 0; i < NPROC; i++) {
 *         struct proc *p = &proc_table[i];
 *         if (p->parent_pid != caller->pid || p->state == PROC_UNUSED) {
 *             continue;
 *         }
 *         have_child = 1;
 *         if (p->state != PROC_ZOMBIE) { continue; }
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
