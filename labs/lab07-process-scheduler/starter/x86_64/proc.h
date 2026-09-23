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
 *
 * 已经写好，不是 TODO：这个文件里的每一处字段顺序（struct context 跟
 * swtch.S 的 push/pop 顺序、struct trapframe 跟 trap_entry.S 的 push
 * 顺序）都是手工对齐的硬性契约，不是"选一种合理方式实现"的开放决策——
 * 挪动任何一个字段的位置，如果不同步改汇编那边算偏移量的代码，会在
 * 运行时读出错位的寄存器值（而不是编译错误），本 Lab 把这些还没写完
 * 的汇编契约留给你在 swtch.S/trap_entry.S 里实现，proc.h 只负责先把
 * 契约的 C 侧完整定下来，你在写那两个文件时应该用这里的字段顺序当作
 * 已知条件核对，而不是自己重新设计布局。 */
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

struct proc {
    enum proc_state state;
    int pid;
    int parent_pid; /* -1 表示没有父进程（本 Lab 唯一的初始进程）。 */
    int64_t exit_code;

    uintptr_t pagetable;      /* 这个进程自己的页表根物理地址。 */
    void *kstack_phys;        /* kalloc_page() 分配的内核栈物理页。 */
    struct trapframe *tf;     /* 指向 kstack 顶部往下的 trapframe 位置。 */
    struct context *context;  /* 指向 kstack 里 swtch() 用的现场。 */

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
void scheduler(void) __attribute__((noreturn));
void yield(void);
struct proc *proc_current(void);

#endif /* OSDEV_PROC_H */
