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
 * 带着用户程序"这个耦合，改从磁盘加载 /init),不重复展开。 */
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

/* 记下"刚刚给这个进程映射了一页用户内存"。 */
void uvm_track(struct proc *p, uintptr_t vaddr, uint32_t flags)
{
    if (p->nupages >= NUSERPAGE) {
        /* 调用点都在 exec 的提交线之后，没有退路，只能 panic——跟 x86_64
         * 版本同一个理由（exec_load() 已经在提交线之前比过页数预算),
         * 不重复展开。 */
        panic("uvm_track: 用户页数超过 NUSERPAGE，exec_load() 的页数预算"
              "检查漏了什么");
    }
    p->upages[p->nupages].vaddr = vaddr;
    p->upages[p->nupages].flags = flags;
    p->nupages++;
}

/* 拆掉这个进程所有用户页的映射，把物理页还给 kalloc，清空 upages[]——跟
 * x86_64 版本同一份实现、同一个"不刷 TLB、不碰中间层节点"的理由，不
 * 重复展开。 */
void uvm_clear(struct proc *p)
{
    for (int i = 0; i < p->nupages; i++) {
        uintptr_t phys = pagetable_unmap(p->pagetable, p->upages[i].vaddr);
        if (phys == 0) {
            panic("uvm_clear: upages[] 里记录的虚拟地址在页表里没有映射，"
                  "两者不一致");
        }
        kfree_page((void *)phys);
    }
    p->nupages = 0;
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

    /* Lab9：用户页清单清空——跟下面 ofile[] 一样，槠位是复用的，必须
     * 显式清，理由跟 x86_64 版本同一处注释一致，不重复展开。 */
    p->nupages = 0;

    /* Lab8：清空打开文件表——理由跟 x86_64 版本同一处注释一致（槠位
     * 复用会导致新进程凭空继承前任的 fd,是一类典型的信息泄露),不
     * 重复展开。 */
    memset(p->ofile, 0, sizeof(p->ofile));

    /* Lab9：页表按进程表槠位复用，只在这个槠位第一次被使用时创建——跟
     * x86_64 版本同一份设计、同一个理由（sys_wait() 只用 uvm_clear()
     * 拆用户页,不释放页表本身,省掉写一个递归销毁函数的代价是页表总量
     * 被钉在 NPROC × 每棵树的节点数,不再归还),不重复展开。 */
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
     * 不重复展开。 */
    uintptr_t kstack_kva_top = (uintptr_t)p->kstack_phys + KERNEL_VIRT_BASE +
                               PROC_KSTACK_PAGES * PAGE_SIZE;
    p->tf = (struct trapframe *)(kstack_kva_top - sizeof(struct trapframe));
    p->context = (struct context *)((uintptr_t)p->tf - sizeof(struct context));

    memset(p->tf, 0, sizeof(struct trapframe));
    memset(p->context, 0, sizeof(struct context));
    p->context->ra = (uint64_t)trap_return;

    return p;
}

/* 创建初始进程：骨架 + 从磁盘加载 /init——跟 x86_64 版本同一个角色（本
 * Lab 唯一的"从无到有"的进程创建入口，只在启动时被 kernel_main.c 调用
 * 一次;sys_fork() 不用它,理由跟 x86_64 版本一致),不重复展开。 */
struct proc *proc_alloc(void)
{
    struct proc *p = proc_alloc_skeleton();
    if (p == NULL) {
        return NULL;
    }

    /* fd 0/1/2 → 串口——跟 x86_64 版本同一份设计、同一个理由（POSIX 的
     * 约定,ulib.c/init.c 直接假设这三个 fd 已经就位),不重复展开。 */
    p->ofile[0].type = FD_CONSOLE;
    p->ofile[0].writable = 0;
    p->ofile[1].type = FD_CONSOLE;
    p->ofile[1].writable = 1;
    p->ofile[2].type = FD_CONSOLE;
    p->ofile[2].writable = 1;

    uintptr_t entry = 0;
    uintptr_t sp = 0;
    /* argv 给 NULL：初始进程没有命令行参数——跟 x86_64 版本同一个理由，
     * 不重复展开。 */
    if (exec_load(p, INIT_PATH, NULL, &entry, &sp) < 0) {
        panic("proc_alloc: 加载 " INIT_PATH " 失败——根文件系统里没有这个"
              "程序，或者它不是一个本架构的静态链接 ELF");
    }

    p->tf->sepc = entry;
    p->tf->sp = sp;
    p->tf->sstatus = INITIAL_SSTATUS;

    memcpy(p->name, "init", 5);

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

            uintptr_t kstack_kva_top = (uintptr_t)p->kstack_phys +
                                       KERNEL_VIRT_BASE +
                                       PROC_KSTACK_PAGES * PAGE_SIZE;
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

    /* Lab9：进程退出时释放它打开的管道端——跟 x86_64 版本同一份实现、
     * 同一个理由（exit 隐含"关闭所有 fd",否则管道另一端会永久卡在
     * read() 上等一个再也不会归零的写端计数),完整展开见 x86_64 版本
     * 同一处注释，不重复。 */
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd].type == FD_PIPE) {
            pipe_close(p->ofile[fd].pipe, p->ofile[fd].writable);
        }
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

/* 把 src 的每一个用户页复制一份给 dst——跟 x86_64 版本同一份实现、同一个
 * "相同虚拟地址+相同权限"的正确性要求、同一个"失败时自己清理已拷页面"
 * 的错误处理约定，完整展开见 x86_64 版本同一处注释，不重复。 */
int uvm_copy(struct proc *dst, struct proc *src)
{
    for (int i = 0; i < src->nupages; i++) {
        uintptr_t vaddr = src->upages[i].vaddr;
        uint32_t flags = src->upages[i].flags;

        uintptr_t src_phys = pagetable_lookup(src->pagetable, vaddr);
        if (src_phys == 0) {
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

/* sys_fork()：user_rip_slot/user_rsp_slot 是"父进程这次 ecall 返回后该
 * 恢复到哪个 sepc/用哪个用户栈指针"这两个值的*地址*——具体指向的是父
 * 进程这次 ecall 陷入时开在它自己内核栈上的那份轻量帧里的 TF_SEPC/TF_SP
 * 两个槠位（trap.c ecall_handler 传进来的 &frame[FRAME_SEPC]/&frame[
 * FRAME_SP],不是 struct trapframe。完整原因见 proc.h sys_fork/sys_exec
 * 声明处的大注释,这里不重复,只写具体步骤：
 *   1. proc_alloc_skeleton() 拿一份骨架（独立页表/内核栈/待填的
 *      trapframe),然后 uvm_copy() 逐页拷贝父进程*全部*已映射的用户页
 *      到子进程自己的物理页,映射到子进程页表里*同样的虚拟地址*——这
 *      就是"fork 出的子进程拥有和父进程一样的地址空间内容"这个语义在
 *      本 Lab 简化范围内的落地方式（完整拷贝，不是 COW，见本文件顶部
 *      模块注释)。
 *   2. 用 *user_rip_slot 和 *user_rsp_slot 填子进程 trapframe 的
 *      sepc/sp——子进程"从 fork() 系统调用返回"这个动作,本质上就是
 *      "在跟父进程这次系统调用一样的 sepc/sp 现场下,走一遍 trap_
 *      return→sret 的返回路径"。sstatus 用 INITIAL_SSTATUS 常量,不读
 *      当前 sstatus——理由见 proc.h 对应注释。
 *   3. 子进程 trapframe 里的 a0（fork() 返回值寄存器）保持 proc_alloc_
 *      skeleton() memset 出来的 0,不用再显式清一次——这正是 fork() 在
 *      子进程里返回 0、在父进程里返回子进程 pid 这条 Unix 经典语义的
 *      全部实现：父进程这次系统调用本身通过正常的 syscall_dispatch
 *      返回值机制拿到子进程 pid（本函数的返回值),子进程将来被调度器
 *      swtch() 进去、trap_return 恢复这份 trapframe、sret 回到用户态
 *      时,看到的 a0 就是这里从未被写过的 0。
 *   4. caller_frame：这次 ecall 陷入时的轻量帧起始地址（跟 user_rip_
 *      slot/user_rsp_slot 指向的是*同一份*内存,只是这里要的是帧本身,
 *      不是某个槠位的地址)——把 ra/t0-t6/a1-a7 这 15 个字段原样抄进
 *      子进程 trapframe 对应字段。这一步是本 Lab 实测踩过的真实 bug 的
 *      修复,完整现象和排查过程见 proc.h sys_fork 声明处"给 sys_fork
 *      新增第三个参数 caller_frame"那段大注释,这里只写结论：漏了这
 *      一步,子进程 trapframe.ra 保持 memset 出来的 0,子进程被调度、
 *      sret 到 fork() 桩函数（user/usys_riscv64.S)里 ecall 之后那条
 *      `ret`时,执行的是 jalr x0, 0(ra),ra=0 直接跳到地址 0,触发一次
 *      page fault: addr=0x0 reason=exec——GDB 断在 trap.c 里
 *      supervisor_trap_handler 第一条语句实测确认过 g_current 是这个
 *      fork() 子进程（pid 2)、frame[FRAME_SEPC]==0，逐层回溯到这里。
 *      a0 故意不抄（原因见上面第 3 点)。
 *
 *      s0-s11/gp/tp 这十四个字段最初不在轻量帧里,曾经作为"已知缺口"
 *      记录在这里——但那是一个真实缺口,不是可以放着不管的简化：GCC
 *      编译的用户程序（init.c main())会用 s0 当帧指针,子进程 s0=0
 *      时执行到 fork() 返回后第一条依赖 s0 的 store 就会故障
 *      （addr=0xffffffffffffffec = 0 + (-20)),已经实测复现过。现在
 *      trap_entry.S 的轻量帧已经扩到 32 个槠位,把这十四个也存进去了
 *      ——完整推导见 trap_entry.S 顶部模块注释,这里下方代码块自己的
 *      注释里有具体抄法,不重复展开。
 *   5. parent_pid 记录血缘关系,state 设成 RUNNABLE 交给调度器。
 *
 * 找不到空闲进程表槠位（NPROC 个槠位全在用)返回 -1，不 panic——这是
 * 用户程序完全可能触发的正常情况（连续 fork 到表满),必须能通过系统
 * 调用返回值告知调用者,不是内部不变量被破坏。 */
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

    /* Lab9：复制父进程的*全部*用户页,页数和虚拟地址都从父进程的
     * upages[] 来——跟 x86_64 版本同一份实现、同一个"uvm_copy 失败时
     * 必须自己还内核栈"的错误处理理由（uvm_copy 只管它自己拷过的用户
     * 页,proc_alloc_skeleton() 分配的内核栈不是它的东西,漏还会造成
     * 真实的内存泄漏),完整展开见 x86_64 版本同一处注释,不重复。 */
    if (uvm_copy(child, parent) < 0) {
        kfree_pages(child->kstack_phys, PROC_KSTACK_PAGES);
        child->kstack_phys = NULL;
        child->state = PROC_UNUSED;
        return -1;
    }

    child->tf->sepc = *user_rip_slot;
    child->tf->sp = *user_rsp_slot;
    child->tf->sstatus = INITIAL_SSTATUS;

    /* Lab9 修复 fork() 寄存器丢失 bug 的核心一步：见上方函数头注释
     * "caller_frame"那一段、以及 proc.h sys_fork 声明处的完整推导。
     * 下标跟 trap_entry.S TF_RA/TF_T0.../TF_A7/TF_S0.../TF_TP（除以 8
     * 之后)、trap.c FRAME_A0/FRAME_A1/FRAME_A2/FRAME_A7 是同一份轻量帧
     * 布局的另一份手工镜像,几处必须保持一致——跟 TF_SEPC/FRAME_SEPC
     * 那对是同一类"没有单一权威来源"的风险。a0（下标 8）故意跳过,理由
     * 见上方函数头注释第 3 点。
     *
     * s0-s11/gp/tp（下标 18-31)这一批是本函数第二次修复时补的：第一次
     * 只抄了 ra/t0-t6/a1-a7（trap_entry.S 当时的轻量帧只有这些),解决了
     * "子进程 ret 到地址 0"这个故障,但换来一个新故障——GCC 编译的
     * init.c main() 里 fork() 返回后紧跟着一条 `sw a5, -20(s0)`（用 s0
     * 当帧指针存 pid 局部变量),子进程 s0 停留在 memset 出来的 0,这条
     * store 算出地址 `0 + (-20)` = 0xffffffffffffffec,直接触发
     * "page fault: addr=0xffffffffffffffec reason=write"。GDB 断在
     * trap.c 故障诊断行确认 g_current 仍是这个 fork() 子进程,sepc 落在
     * 这条 sw 指令上，跟 objdump 反汇编逐条对得上。完整修法（trap_
     * entry.S 把轻量帧再扩 14 个槠位)见 trap_entry.S 顶部模块注释
     * "sys_fork() 撕开的缺口"那一大段,这里只是消费方,把新增的槠位
     * 抄进 struct trapframe 里同名字段即可——proc.h struct trapframe
     * 本来就有 s0-s11 三个字段（swtch()/struct context 的 callee-saved
     * 集合本来就包含它们),这里不需要新增字段,只是第一次把它们真正
     * 填上非零值。 */
    child->tf->ra = caller_frame[0];
    child->tf->t0 = caller_frame[1];
    child->tf->t1 = caller_frame[2];
    child->tf->t2 = caller_frame[3];
    child->tf->t3 = caller_frame[4];
    child->tf->t4 = caller_frame[5];
    child->tf->t5 = caller_frame[6];
    child->tf->t6 = caller_frame[7];
    child->tf->a1 = caller_frame[9];
    child->tf->a2 = caller_frame[10];
    child->tf->a3 = caller_frame[11];
    child->tf->a4 = caller_frame[12];
    child->tf->a5 = caller_frame[13];
    child->tf->a6 = caller_frame[14];
    child->tf->a7 = caller_frame[15];
    child->tf->s0 = caller_frame[18];
    child->tf->s1 = caller_frame[19];
    child->tf->s2 = caller_frame[20];
    child->tf->s3 = caller_frame[21];
    child->tf->s4 = caller_frame[22];
    child->tf->s5 = caller_frame[23];
    child->tf->s6 = caller_frame[24];
    child->tf->s7 = caller_frame[25];
    child->tf->s8 = caller_frame[26];
    child->tf->s9 = caller_frame[27];
    child->tf->s10 = caller_frame[28];
    child->tf->s11 = caller_frame[29];
    child->tf->gp = caller_frame[30];
    child->tf->tp = caller_frame[31];

    /* Lab8：把父进程的打开文件表整份拷给子进程——跟 x86_64 版本同一份
     * "拷贝语义,不是共享语义"的简化,完整展开见 x86_64 版本同一处注释,
     * 不重复。 */
    memcpy(child->ofile, parent->ofile, sizeof(child->ofile));

    /* Lab9：管道是上面那个"拷贝语义"的例外——memcpy 把 pipe 指针也拷
     * 过去了,父子于是指向*同一个* struct pipe,但引用计数不会自己跟着
     * 涨,必须在这里显式加。只有 FD_PIPE 需要这一步。完整展开（包括
     * 漏掉这一步会有多隐蔽)见 x86_64 版本同一处注释,不重复。 */
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
 * Lab9 才第一次名副其实——跟 x86_64 版本同一个理由（Lab7 的 sys_exec()
 * 没有 path 参数,只能"exec 自己",那时没有文件系统也没有 ELF 加载器),
 * 不重复展开。
 *
 * 真正的加载工作全在 exec_load() 里（exec.c，两个架构共用）。本函数只
 * 负责三件架构相关的事：取参数、调加载器、把"这次 ecall 返回后恢复到
 * 哪里"改写成新程序的入口。
 *
 * ── 为什么靠写 user_rip_slot/user_rsp_slot ────────────────────────
 *
 * 跟 sys_fork() 一样，这两个参数是*地址*而不是值（proc.h 声明处有完整
 * 说明）。这次 ecall 陷入路径上没有 struct trapframe 可改——p->tf 只在
 * "这个进程第一次被 swtch() 进去"时才会被 trap_return 读取,这次 ecall
 * 即将执行的 sret 完全不碰它。要改变"系统调用返回后从哪里继续执行"，
 * 唯一的手段就是改写 trap_entry.S 待会儿真的会去 ld 的那两个栈帧槠位：
 * TF_SEPC 和 TF_SP。ecall_handler 调用 syscall_dispatch 返回之后，
 * supervisor_trap_handler 会继续走"ld 恢复寄存器→sret"这条统一路径，
 * 读到的就是这里刚写进去的新值。
 *
 * ── 失败必须可恢复 ────────────────────────────────────────────────
 *
 * exec_load() 返回 -1 时调用者的地址空间一个字节都没动过，所以这里直接
 * return -1 就行，用户程序会从 exec() 调用点继续往下执行——跟 x86_64
 * 版本同一个理由（sh 靠这个打"command not found",没有它一次拼错命令
 * 就会杀掉 shell),不重复展开。
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

    /* 进程名跟着程序走——跟 x86_64 版本同一份实现,取 path 的最后一段
     * （去掉目录部分),不重复展开。
     *
     * 必须在 exec_load() 之前做,结果存进内核局部变量 name_buf——path 是
     * 指向调用者*旧*地址空间的用户指针,exec_load() 内部越过提交线之后
     * 会拆掉旧映射、换上新程序的页表,这之后 path 就是悬空指针。踩过的
     * 真实 bug、完整现象和排查过程见 x86_64 版本同一处的大注释,这里的
     * 修法跟那边逐字对应,不重复展开。 */
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

/* sys_wait()：等一个子进程退出，回收它，返回它的 pid——跟 x86_64 版本
 * 同一份实现、同一个"忙等 yield 循环模拟阻塞"的简化、同一个"没有子
 * 进程直接返回 -1 避免死锁"的前置检查、同一个"回收用户页交给槠位复用"
 * 的资源释放策略，完整展开（包括为什么 Lab9 必须阻塞、为什么这不是
 * 真正的睡眠/唤醒）见 x86_64 版本同一处注释，不重复。 */
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

            uvm_clear(p);
            kfree_pages(p->kstack_phys, PROC_KSTACK_PAGES);
            p->kstack_phys = NULL;
            p->state = PROC_UNUSED;

            return pid;
        }

        if (!have_child) {
            return -1;
        }

        yield();
    }
}
