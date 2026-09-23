/* Lab7 x86_64：进程控制块（PCB）+ 上下文切换的数据结构定义。
 *
 * 本 Lab 引入两种"保存的寄存器现场"，教学上必须分清楚，因为它们对应
 * 两条完全不同的路径，混着理解会在读 swtch.S/trap_entry.S 时卡住：
 *
 *   struct context —— swtch() 用的，只保存 callee-saved 寄存器
 *     （rbx/rbp/r12-r15）+ 返回地址（rip）。这是"内核代码主动调用
 *     swtch() 切换到另一个内核执行流"这条路径专用的现场，caller-saved
 *     寄存器（rax/rcx/rdx/rsi/rdi/r8-r11）完全不保存——按 C 调用约定，
 *     调用 swtch() 之前，编译器生成的代码已经该把 caller-saved 寄存器
 *     的活跃值都存到栈上了（如果还需要用），swtch() 本身也是一个普通
 *     C 函数调用，不需要越权保存调用约定本来就不保证跨调用存活的寄存器。
 *
 *   struct trapframe —— 描述"一个进程被打断在用户态时，CPU/中断硬件
 *     已经/需要保存的完整现场"，包含所有通用寄存器 + iretq 需要的
 *     RIP/CS/RFLAGS/RSP/SS 五个字段。这是"进程从用户态陷入内核"这条
 *     路径专用的现场——本 Lab 复用 Lab6 timer_stub 的 15 个 push 顺序
 *     （必须逐字节对应，trap_entry.S 改成"进程专属内核栈"之后，这份
 *     栈帧会落在每个进程自己的 kstack 顶部，而不是共享的 __stack_top）。
 *
 * 一个进程结构体里同时有这两种现场：trapframe 描述"它上一次陷入内核时
 * 用户态的样子"，context 描述"它作为内核执行流被 swtch() 换出时内核态
 * 的样子"——调度器 swtch() 到一个进程时，恢复的是 context，而 context
 * 里存的返回地址指向 trap_return（见 proc.c），trap_return 再去恢复
 * trapframe、执行 iretq 真正回到用户态。这是 xv6 教学内核的经典手法：
 * "resume a process" = "swtch 到一段内核代码，这段代码的唯一工作就是
 * 从 trapframe 恢复现场再 iretq"，让"调度器换进程"和"trap 处理完毕
 * 返回用户态"共用同一条 iretq 出口，不需要发明第二套返回机制。
 */
#ifndef OSDEV_PROC_H
#define OSDEV_PROC_H

#include "types.h"

/* NPROC：固定大小的进程表，教学取向不引入动态扩容——xv6 系教材的标准
 * 选择（真实 xv6 是 NPROC=64），本课程只需要演示"多个进程轮转调度"，
 * 4 个足够看到轮转效果，也足够在一屏 kprintf 输出里看清楚每个进程的
 * pid，不需要更大的表。 */
#define NPROC 4

/* 每个进程的内核栈大小：一整页（4KiB）。真实内核通常给更大的栈（Linux
 * 默认 8KiB/16KiB），本课程的内核态代码调用深度很浅（trap 处理 + 几个
 * syscall 分发函数），一页足够，且跟"一次 kalloc_page() 分配一个栈"
 * 这个简化直接对应，不需要多页拼接。 */
#define PROC_KSTACK_PAGES 1

enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_ZOMBIE,
};

/* struct context 字段顺序就是 swtch.S 里 push/pop 的顺序——改这里必须
 * 同步改 swtch.S，两者是同一份契约的两份表达（一份给 C 类型检查，一份
 * 给汇编算偏移量），跟 riscv64 trap_entry.S 的 frame[N] 下标注释是同一
 * 类"没有单一数据源、需要人肉对齐"的手工契约。 */
struct context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
};

/* struct trapframe 字段顺序对应 trap_entry.S 里 push 的顺序（从低地址
 * 到高地址，即先 push 的字段在结构体里排在后面——C 结构体从低地址到
 * 高地址排列，栈是从高地址向低地址增长，push 顺序反过来才是内存布局
 * 顺序）：本 Lab 让 trap_entry.S 直接用这个结构体的字段偏移，而不是
 * Lab6 那种裸 push/pop 序列（Lab6 只需要保存/恢复现场，不需要在中间
 * 让 C 代码检查/修改具体某个字段——本 Lab 的 fork() 需要拷贝子进程的
 * trapframe、把 rax 清零表示子进程里 fork 返回 0，必须能从 C 侧按字段
 * 名访问，不能只是一段不透明的栈字节）。 */
struct trapframe {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    /* 以下五个字段是 iretq 弹出的顺序（从低地址到高地址）：
     * RIP/CS/RFLAGS/RSP/SS。 */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* Lab8：每个进程的打开文件数量上限。
 *
 * 8 个足够演示"同一个文件被打开两次、两个 fd 各有独立偏移"这个关键性质，
 * 也足够 Lab9 的 shell 做重定向。真实内核这个上限是可调的（Linux 的
 * RLIMIT_NOFILE，默认 1024），因为 fd 表是动态分配的；本课程用定长数组
 * 直接嵌在 PCB 里，省掉一次分配和随之而来的释放时机问题。 */
#define NOFILE 8

/* Lab8：一个打开的文件。
 *
 * 只有三个字段，但它们恰好就是"文件描述符"这个抽象的全部内容：
 *   - 指向哪个文件（inum）
 *   - 读到哪里了（off）
 *   - 这个槽位有没有被占用（used）
 *
 * 关键点是 off 存在这里、而不是存在 inode 里：同一个文件被 open() 两次
 * 会得到两个槽位、两个独立的 off，两个 fd 可以在同一个文件的不同位置
 * 各读各的。如果把"当前偏移"放进 inode（每个文件只有一个），这件事就
 * 做不到了——这正是 Unix 把"文件"和"打开的文件"分成两个概念的原因。
 *
 * 本 Lab 的简化：这张表直接嵌在 struct proc 里，所以 fork() 之后父子
 * 进程各有一份*独立拷贝*，偏移不共享。真正的 Unix 语义是父子共享同一个
 * struct file（fork 只增加引用计数），父进程读了 10 字节、子进程接着读
 * 会从第 10 字节开始。做到那一步需要一张全局 file 表 + 引用计数 + 在
 * exit() 里递减，是本 Lab 的挑战任务，README 的挑战一节有完整说明。 */
struct file {
    int used;
    uint32_t inum;   /* 文件的 inode 号，0 表示无效 */
    uint32_t off;    /* 下一次 read 从文件的第几个字节开始 */
};

struct proc {
    enum proc_state state;
    int pid;
    int parent_pid; /* -1 表示没有父进程（本 Lab 唯一的初始进程）。 */
    int64_t exit_code;

    uintptr_t pagetable;      /* 这个进程自己的页表根物理地址。 */
    void *kstack_phys;        /* kalloc_page() 分配的内核栈物理页。 */
    struct trapframe *tf;     /* 指向 kstack 顶部往下的 trapframe 位置。 */
    struct context *context;  /* 指向 kstack 里 swtch() 用的现场。 */

    /* Lab8：打开文件表。fd 就是这个数组的下标——"文件描述符是一个小整数"
     * 在这里字面成立，没有任何间接层。fork() 直接结构体拷贝，见 proc.c 里
     * sys_fork() 的说明。 */
    struct file ofile[NOFILE];

    char name[16];
};

extern struct proc proc_table[NPROC];

void proc_init(void);
struct proc *proc_alloc(void);

/* sys_fork/sys_exec 不接手完整的 struct trapframe——SYSCALL（syscall_
 * entry，跟 IDT 的 timer_stub/page_fault_stub 是两条独立的陷入路径,
 * 见 trap.c 顶部模块注释)从来不构造一份 trapframe,只有 rip（这次
 * syscall 返回后恢复执行的地址,来自 rcx)和 rsp（用户栈指针,SYSCALL
 * 硬件本身不改动它,全程原样留在 syscall_saved_user_rsp)这两个字段
 * 是有意义、有真实来源的——用户程序在本课程的教学范围内也从不依赖
 * "某个通用寄存器的值能跨越一次 syscall 存活"（fork()/exec() 的调用
 * 约定跟 sys_write/sys_exit 一样,只用 rax/rdi/rsi,不额外定义"哪些
 * 寄存器需要 callee 保留"),所以只传这两个字段完全足够表达"resume 到
 * 哪里"这件事,不需要在 SYSCALL 路径上额外构造一份从未被真正需要过的
 * 完整现场。
 *
 * 两个参数都传*地址*而不是*值*，即使 sys_fork() 自己不需要写它们——
 * 统一成同一种签名，syscall_entry（trap_entry.S）不需要为 sys_fork/
 * sys_exec 分别准备两种不同的调用方式。sys_exec() 需要写：它要改写
 * "这次 syscall 返回后 rcx/用户栈指针应该恢复成什么"，这两个值物理上
 * 存在 syscall_entry 的栈帧位置（rcx 那份)和 trap.c 的 syscall_saved_
 * user_rsp 全局变量（rsp 那份)里，只有传地址才能真正写回去。 */
int sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot);
int sys_exec(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot);

int sys_wait(int64_t *exit_code_out);
void sys_exit_proc(int64_t code);

/* Lab8：文件相关的系统调用实现（在 trap.c 里，紧挨着 sys_write）。
 * 返回值约定跟 POSIX 一致：成功返回非负数，失败返回负数。 */
int64_t sys_open(const char *user_name);
int64_t sys_read(int fd, void *user_buf, uint64_t len);
int64_t sys_close(int fd);
void scheduler(void) __attribute__((noreturn));
void yield(void);
struct proc *proc_current(void);

#endif /* OSDEV_PROC_H */
