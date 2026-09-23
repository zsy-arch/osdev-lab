/* Lab7 riscv64：进程控制块（PCB）+ 上下文切换的数据结构定义。跟 x86_64
 * 版本（../x86_64/proc.h）同一个整体设计（同一份 proc.c 骨架、同一套
 * struct context/struct trapframe 二分法），这里只写 riscv64 特有的
 * 寄存器集合和字段布局，设计动机跟 x86_64 版本完全一致的部分不重复。
 *
 * struct context —— swtch() 用的，只保存 RISC-V 调用约定里 callee-saved
 *   的寄存器（ra + s0-s11，riscv-abi 规范"Register Convention"表格里
 *   标 "saved register" 的那一组，t0-t6/a0-a7 都是 caller-saved，跟
 *   x86_64 侧 rbx/rbp/r12-r15 是同一个角色，只是 riscv64 的 callee-saved
 *   集合更大——12 个 s 寄存器 + ra，比 x86_64 的 5 个多了不少，这不是
 *   本课程的选择，是 RISC-V calling convention 本身的设计）。
 *
 * struct trapframe —— 这里跟 x86_64 版本出现了一处本质差异，值得单独
 *   说明：x86_64 的 Lab6 timer_stub/page_fault_stub 从一开始就保存全部
 *   15 个通用寄存器（trap_entry.S 的固定序列），Lab7 只是在末尾追加
 *   CS/RFLAGS/RSP/SS 四个硬件已经自动压栈的字段，凑成完整的
 *   struct trapframe——不需要改变"保存哪些寄存器"这个决定。riscv64
 *   这边不一样：Lab4/5/6 的 supervisor_trap_entry（见 trap_entry.S）
 *   只保存 16 个寄存器（ra/t0-t6/a0-a7，128 字节），因为 Lab4/5/6 的
 *   trap 处理函数（page fault 直接 panic、定时器只是自增计数器）从不
 *   需要 s0-s11——那次的 trap 处理完就 sret 回*同一个*执行流，s0-s11
 *   是 riscv 调用约定里"callee 必须原样保留"的寄存器，C 编译器生成的
 *   supervisor_trap_handler 函数体本身就会在需要用到 s 寄存器时自己
 *   压栈保存/弹栈恢复，trap_entry.S 不用管——这个前提在 Lab7 才被打破：
 *   调度器可能把 CPU 切给*另一个*进程很久，等这个进程的 trap 处理"完毕"
 *   （yield() 把自己换出去，另一个进程执行了一段时间后调度器才把这个
 *   进程重新 swtch() 回来）才继续，中间这段时间里，这个进程被打断那一
 *   刻用户态代码正用着的 s0-s11 早已经不在物理寄存器里了（被中间跑的
 *   那些内核代码/别的进程覆盖），如果不在 trap 现场里保存它们，"恢复
 *   执行"这件事就无法真正做到——用户态代码从被打断的地方往下跑，读到
 *   的 s0-s11 会是错误的值，本课程内联汇编写的 user_prog.S 目前不使用
 *   s 寄存器所以不会立刻暴露这个 bug，但这是一个真实的正确性缺口，不能
 *   靠"这个 Lab 的用户程序凑巧没用到"来掩盖——Lab7 引入调度之后，trap
 *   现场必须能完整描述"resume 这个进程需要哪些信息"，这跟 x86_64 版本
 *   "trapframe 必须是完整现场，不能是部分现场"是同一个要求，只是 riscv64
 *   这边需要真正*扩大*保存的寄存器集合，不是像 x86_64 那样"本来就保存
 *   齐了，只是再追加几个硬件字段"。
 *
 *   同理，sepc/sstatus 在 Lab4/5/6 从未需要跨越"trap 处理函数返回之后"
 *   存活——那时唯一的路径就是"处理完，sret，回到刚才被打断的地方"，
 *   sepc/sstatus 全程只活在 CSR 里，从未离开过。Lab7 需要 sepc/sstatus
 *   能被换出、换入：调度器切到另一个进程时，这个进程的 sepc/sstatus
 *   会被那个进程自己的 trap 处理过程覆写（sepc/sstatus 是 CPU 全局的
 *   CSR，不是 per-process 的存储）——这两个字段确实需要能被保存/恢复，
 *   struct trapframe 因此需要 sepc/sstatus 两个字段，对应 x86_64
 *   trapframe 里 RIP/RFLAGS 两个字段的角色（CS/SS 在 riscv64 没有直接
 *   对应物，riscv64 用 sstatus.SPP 表达"回到哪个特权级"，已经包含在
 *   sstatus 字段里，不需要单独的字段）。
 *
 *   这两个字段实际服务*两种*场景，但用的是同一对存储位置，不是两套
 *   机制：场景一，"这个进程从未运行过，第一次被 swtch() 进去"
 *   （proc_alloc_skeleton() 手工摆好整份 trapframe，swtch.S trap_return
 *   读它们、csrw 进 CSR、sret，只发生一次）。场景二，"这个进程已经在
 *   运行，被一次定时器 tick 打断，trap.c timer_interrupt_handler()
 *   调用 yield()，可能引发一次 swtch() detour"——这种情况下 sepc 同样
 *   需要保护，但保护窗口只覆盖 yield() 那一次调用（不是整个 trap 的
 *   生命周期），所以由 timer_interrupt_handler() 自己在调用 yield()
 *   前把 sepc 存进 g_current->tf->sepc，yield() 返回之后再读回来写回
 *   CSR——两种场景用的是同一个字段（trapframe.sepc），只是写入者、
 *   写入时机不同，本质上都是"把 CPU 全局 CSR 的值找一个 per-process
 *   的容器存起来，避开其它进程的覆写"，完整推导见 trap.c
 *   timer_interrupt_handler() 上方注释。ecall 分支（trap_entry.S
 *   supervisor_trap_entry -> trap.c ecall_handler）从不调用 yield()，
 *   不存在 detour，不需要这层保护——trap.c 里 write_sepc(read_sepc()+4)
 *   跳过 ecall 指令本身这一行已经足够。
 *
 * 一个进程结构体里同时有这两种现场：trapframe 描述"它上一次陷入内核时
 * 用户态的样子"，context 描述"它作为内核执行流被 swtch() 换出时内核态
 * 的样子"——跟 x86_64 版本完全一样的关系，"resume a process" = "swtch
 * 到 trap_return，trap_return 从 trapframe 恢复现场再 sret"，见
 * swtch.S trap_return 那段注释。
 */
#ifndef OSDEV_PROC_H
#define OSDEV_PROC_H

#include "types.h"

/* NPROC/PROC_KSTACK_PAGES：跟 x86_64 版本取一样的值，理由完全相同
 * （见 ../x86_64/proc.h 对应注释），两边没有任何架构相关的理由需要
 * 取不同的值。 */
#define NPROC 4
#define PROC_KSTACK_PAGES 1

enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_ZOMBIE,
};

/* struct context 字段顺序就是 swtch.S 里 sd/ld 的顺序——改这里必须同步
 * 改 swtch.S，跟 x86_64 版本同一处注释、riscv64 trap_entry.S 的
 * frame[N] 下标注释是同一类"没有单一数据源、需要人肉对齐"的手工契约。
 * ra 放在最前面只是习惯（跟 x86_64 版本 struct context 把 rip 放在
 * 最后不同），两种顺序本身没有正确性上的差异，swtch.S 里的 sd/ld 顺序
 * 只要跟这里逐字段对应即可。 */
struct context {
    uint64_t ra;
    uint64_t s0;
    uint64_t s1;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
};

/* struct trapframe 字段顺序对应 trap_entry.S 里 sd 的顺序（从低地址到
 * 高地址，即先 sd 的字段在结构体里排在后面——同一个"push 顺序反过来是
 * 内存布局顺序"的道理，riscv64 的 addi sp,sp,-N 也是先减小 sp 再往上
 * 摆放，跟 x86_64 的 push 是同一个方向）。
 *
 * 32 个字段（不算 sepc/sstatus）覆盖了 x0（zero，硬件永久为 0，从不
 * 需要保存/恢复，不设字段）之外全部的通用寄存器——跟 Lab4/5/6 的 16
 * 寄存器（ra/t0-t6/a0-a7）比多了 sp 本身（需要保存"这次陷入时用户栈
 * 顶在哪"，Lab4/5/6 从不需要，因为那时 trap 只可能来自 S-mode，sp
 * 全程就是内核栈，恢复执行时 sp 本来就没变过）、gp/tp（本课程从未主动
 * 使用它们，但保存全套通用寄存器是更严谨的选择，不依赖"用户程序不会
 * 用到 gp/tp"这个假设——真实的 gp 尤其重要，-mcmodel=medany 下编译器
 * 生成的代码可能依赖 gp 指向 .sdata 附近，虽然本课程的 user_prog.S
 * 是纯手写汇编、不使用 gp 相关寄存器寻址,但把它保存进 trapframe 不需要
 * 额外成本,直接按"完整通用寄存器集合"处理更一致)、s0-s11（Lab4/5/6
 * 从不需要，见本文件顶部模块注释详细展开的理由）。 */
struct trapframe {
    uint64_t ra;
    uint64_t sp;
    uint64_t gp;
    uint64_t tp;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
    uint64_t s0;
    uint64_t s1;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;
    /* 以下两个字段是 sret 执行前必须现场写好的 CSR（trap_return 恢复
     * 完上面 32 个通用寄存器之后，最后 csrw 这两个再 sret）——对应
     * x86_64 trapframe 尾部 RIP/CS/RFLAGS/RSP/SS 五个字段里 RIP/RFLAGS
     * 的角色（CS/SS 在 riscv64 侧已经折进 sstatus.SPP，RSP 就是上面
     * 的 sp 字段本身，不需要重复）。 */
    uint64_t sepc;
    uint64_t sstatus;
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

/* sys_fork/sys_exec：跟 x86_64 版本的 uintptr_t* 双槽位签名不同——但
 * 不同的真实原因跟本文件早前版本这段注释写的不一样，这里纠正一下：
 * riscv64 的 ecall 陷入路径（trap_entry.S supervisor_trap_entry ->
 * trap.c ecall_handler）用的仍然是 Lab4/5/6 那套轻量栈帧（ra/t0-t6/
 * a0-a7,16 个字段,128 字节,原样未变——Lab7 没有给这个栈帧扩容,见
 * trap_entry.S 顶部模块注释),*不是* struct trapframe——ecall_handler
 * 只是从这个轻量帧里取 a7/a0/a1 传给 syscall_dispatch,跟 sys_fork()/
 * sys_exec() 需要的"返回后该恢复到哪个 PC/用哪个用户栈"完全不是同一份
 * 数据、也不经过同一条路径。
 *
 * 真正的原因是：sepc 这个 CSR，以及 trap_saved_user_sp 这个全局变量
 * （trap.c 顶部定义、trap_entry.S 的 from_user 分支写入),*本身*就是
 * 全局可寻址的存储位置——sys_fork()/sys_exec() 可以在 proc.c 里直接
 * read_sepc()/write_sepc()、直接读写 trap_saved_user_sp,不需要调用者
 * （ecall_handler)额外传一个指向它们的指针下去。x86_64 那边必须传
 * user_rip_slot/user_rsp_slot 两个指针，是因为 SYSCALL 现场（rcx=
 * 用户 RIP/syscall_saved_user_rsp 全局变量)在 syscall_entry 这次调用
 * 栈帧里，rcx 是*寄存器*不是内存位置,想要"改写父进程这次系统调用返回后
 * 该恢复到哪里"就必须拿到一个指向"rcx 将被弹出的那个栈帧槽位"的地址——
 * riscv64 这边没有这层问题：sepc 是 CSR，从来不在栈帧里，read_sepc()/
 * write_sepc() 在任何函数里都能直接用；trap_saved_user_sp 是全局变量，
 * 同样到处可寻址。
 *
 * 具体做法：
 *   - sys_fork()：子进程的 trapframe（proc_alloc_skeleton() memset
 *     清零)只需要把 tf->sepc/tf->sp 设成"父进程这次 ecall 返回后原本
 *     该恢复到的 sepc/用户 sp"——分别读 read_sepc()（此刻已经是
 *     ecall_handler 分发之前 sepc+=4 之后的值,见 trap.c)和
 *     trap_saved_user_sp 这个全局变量当前的值。子进程 trapframe 的
 *     a0（fork() 返回值寄存器）保持骨架清零时的 0，不需要额外清——
 *     跟 x86_64 版本"子进程 rax 保持 memset 出来的 0"是同一个手法，
 *     只是这里换成 a0。tf->sstatus 需要手工构造 SPP=0/SPIE=1/SUM=1
 *     这个常量（对应 Lab6 已删除的 enter_user_mode 曾经硬编码的同一个
 *     值,现在这个常量搬进了 proc.c),不能直接读当前 sstatus——当前
 *     sstatus 描述的是"父进程这次 ecall 陷入时的现场"，SPP 此刻已经
 *     被硬件设成了 0（还没 sret,一直是 0),读到的值凑巧是对的，但这是
 *     偶然而不是可以依赖的关系，直接写常量更清楚地表达"子进程恢复到
 *     用户态、中断使能、允许访问用户内存"这三个属性是"回到用户态"这件
 *     事本身固定的，不依赖父进程此刻具体是什么状态。
 *   - sys_exec()：直接把*调用者自己* g_current->tf 的 sepc/sp 改写成
 *     新程序的入口/新栈顶——调用者的 trapframe 本来就是它自己的，不需要
 *     像 x86_64 SYSCALL 路径那样借用参数指针去写"外部某个栈帧位置"。
 * 所以本质原因不是"riscv64 ecall 总有完整 trapframe 可用"（这是错的，
 * ecall 路径至今仍是轻量帧),而是"riscv64 等价的两个值天生就是全局
 * 可寻址的,x86_64 等价的两个值天生只是局部寄存器/栈帧槽位"。 */
int sys_fork(void);
int sys_exec(void);

int sys_wait(int64_t *exit_code_out);
void sys_exit_proc(int64_t code);
void scheduler(void) __attribute__((noreturn));
void yield(void);
struct proc *proc_current(void);

#endif /* OSDEV_PROC_H */
