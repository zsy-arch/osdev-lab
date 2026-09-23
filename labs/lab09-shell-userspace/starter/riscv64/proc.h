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
 *   运行，被一次 trap（定时器 tick 或者一个会 yield() 的 ecall）打断，
 *   处理过程中可能引发一次 swtch() detour"——这种情况下 sepc/sp 同样
 *   需要保护，保护窗口覆盖"从这次 trap 陷入到 sret 返回"整段时间，
 *   跨越任意长的 detour。
 *
 *   Lab9 起这层保护*不再需要任何一条路径手工去做*：sepc/sp 从"trap
 *   发生那一刻"起就直接落在 trap_entry.S 为这次 trap 开的栈帧里
 *   （TF_SEPC/TF_SP 两个槽位，trap.c FRAME_SEPC/FRAME_SP 是同一份
 *   偏移量的 C 侧镜像，两处必须手工保持一致——本课程一贯的"没有单一
 *   数据源"契约，跟 swtch.S/struct context 的关系相同），yield() 内部
 *   swtch() 让出、被 scheduler() 换回来之后，出口从*这次 trap 自己的*
 *   栈帧槽位读回 sepc/sp，跟其它进程在这段时间里怎么覆写 sepc/sp 这两
 *   个 CSR/寄存器完全无关——槽位天然是 per-trap 的存储，不需要额外找
 *   一个 per-process 的容器。这跟 ra/t0-t6/a0-a7 靠"冻结在自己的内核栈
 *   上"存活是同一个原理，只是 sepc 是 CSR、用户 sp 曾经活在一个全局
 *   变量里，两者都不在通用寄存器堆里，没有硬件自动帮它们做这件事，
 *   Lab9 之前只能靠软件在每一条会 yield() 的路径上手工补一遍。
 *
 *   Lab7/Lab8 版本靠 `g_current->tf->sepc` 手工存/取（timer_interrupt_
 *   handler() 调用 yield() 前把 sepc 存进去，返回后读回来写 CSR）；
 *   ecall 分支当时被认为"从不调用 yield()，不需要这层保护"。Lab9 引入
 *   sys_wait()/pipe_read()/pipe_write()/console_read() 之后这句话不再
 *   成立——这些系统调用全都在 ecall 内部 yield()，如果继续沿用"哪条
 *   路径需要保护就手工加一遍"的思路，每新增一个会阻塞的系统调用都要
 *   重新确认有没有漏补，这正是把它挪进 trap_entry.S 栈帧槽位的理由：
 *   槽位对所有路径一视同仁，不需要任何一条路径记得照顾它。完整的故障
 *   复现过程、新旧两版实现的逐行对比，见 trap.c timer_interrupt_
 *   handler() 上方那段保留下来的历史注释。
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
 * 取不同的值。
 *
 * Lab9 两个都涨了：NPROC 4→8（一条管道命令同时活着 init/sh/左/右 正好
 * 4 个，没有余量），PROC_KSTACK_PAGES 1→2（新增的 exec 路径调用链明显
 * 更深，而本课程没有栈溢出检测）。完整推导见 x86_64 版本。 */
#define NPROC 8
#define PROC_KSTACK_PAGES 2

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

/* Lab8：每个进程的打开文件数量上限，以及"一个打开的文件"这个结构。
 *
 * 这几段定义跟 x86_64 那份逐字相同，因为"文件描述符是什么"完全不涉及
 * 架构——它是一个纯粹的内核数据结构设计决定。x86_64 那份 proc.h 里
 * struct file 上方有完整的展开，这里只重复最关键的一点：
 *
 *   off 存在这里、而不是存在 inode 里。同一个文件被 open() 两次会得到
 *   两个槽位、两个独立的 off，两个 fd 可以在同一个文件的不同位置各读
 *   各的。把"当前偏移"放进 inode（每个文件只有一个）就做不到这件事——
 *   这正是 Unix 把"文件"和"打开的文件"分成两个概念的原因。
 *
 * 本 Lab 的简化：这张表直接嵌在 struct proc 里，所以 fork() 之后父子
 * 进程各有一份*独立拷贝*，偏移不共享。真正的 Unix 语义是父子共享同一个
 * struct file（fork 只增加引用计数）。做到那一步需要一张全局 file 表 +
 * 引用计数 + 在 exit() 里递减，是本 Lab 的挑战任务。 */
#define NOFILE 8

/* Lab9：一个 fd 可以指向三种不同的东西，所以需要一个类型标签。
 *
 * Lab8 的 struct file 只有 {used, inum, off}，只能表示"一个打开的磁盘
 * 文件"；fd 0/1/2 是特殊照顾的，sys_write 看见 fd==1 就往串口打，根本
 * 不查表。Lab9 这么做不下去了，原因是管道：`cat /motd.txt | grep lab`
 * 要求 grep 的 fd 0 是管道读端，而 grep 的代码写的是 read(0, ...)——
 * "fd 0 是什么"必须是运行时的、可以被 shell 改掉的信息，也就是必须真的
 * 存在 fd 表里。完整展开见 ../x86_64/proc.h 对应注释。 */
enum fd_type {
    FD_NONE = 0,   /* 空槽位。放在 0 上，这样清零的 PCB 天然是"全部关闭"。 */
    FD_CONSOLE,    /* 串口。读走 UART RX，写走 kprintf 那条路。 */
    FD_INODE,      /* 磁盘文件。用 inum + off。 */
    FD_PIPE,       /* 管道。用 pipe + writable。 */
};

/* struct pipe 定义在 pipe.h 里，这里只要一个前向声明——proc.h 不 include
 * pipe.h，因为它只需要"有这么个类型，我存一个指向它的指针"。少一条 include
 * 就少一条循环包含的可能（pipe.c 要用 yield()，那是 proc.h 里的）。 */
struct pipe;

struct file {
    enum fd_type type;
    uint32_t inum;         /* FD_INODE：文件的 inode 号。 */
    uint32_t off;          /* FD_INODE：下一次 read 从第几个字节开始。 */
    struct pipe *pipe;     /* FD_PIPE：指向共享的管道本体。 */
    int writable;          /* FD_PIPE：这一端是写端还是读端。 */
};

/* Lab9：每个进程的用户页数量上限，以及一个用户页的记录。
 *
 * 为什么 Lab9 突然需要这个、而 Lab7/Lab8 不需要：那两个 Lab 里用户地址
 * 空间是*写死的*两页（代码 + 栈），fork/exec 就是那两行。Lab9 的地址空间
 * 由 ELF 决定，页数和虚拟地址运行时才知道，"这个进程有哪些用户页"必须被
 * 真正记下来。
 *
 * 只记虚拟地址不记物理地址（页表是唯一权威，记两份就会不一致），但 flags
 * 必须记（fork 要用相同权限映射子进程的页）。16 的来源和 kalloc 页预算的
 * 天花板推导见 ../x86_64/proc.h 对应注释——两边取值和理由完全相同。 */
#define NUSERPAGE 16

struct uvm_page {
    uintptr_t vaddr;
    uint32_t flags;
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

    /* Lab9：这个进程的用户页清单。exec 建立时填，fork 照着拷，exit 照着
     * 释放。三个操作都只看这张表，不再有任何"用户空间长什么样"的硬编码。 */
    struct uvm_page upages[NUSERPAGE];
    int nupages;

    /* Lab8：打开文件表。fd 就是这个数组的下标——"文件描述符是一个小整数"
     * 在这里字面成立，没有任何间接层。 */
    struct file ofile[NOFILE];

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
 * 只是从这个轻量帧里取 a7/a0/a1/a2 传给 syscall_dispatch,跟 sys_fork()/
 * sys_exec() 需要的"返回后该恢复到哪个 PC/用哪个用户栈"不是同一份数据。
 *
 * Lab7/Lab8 这里的答案是"riscv64 天生不需要传指针"：sepc 是 CSR、用户 sp
 * 存在 trap_saved_user_sp 这个全局变量里,两者*本身*就是全局可寻址的存储
 * 位置,proc.c 里直接 read_sepc()/write_sepc()、直接读写那个全局变量就行,
 * 不需要调用者把地址传下来。x86_64 那边必须传,是因为用户 RIP 躺在 rcx
 * 这个*寄存器*里,除了"rcx 将被弹出的那个栈帧槽位"没有第二个地方能改它。
 *
 * Lab9 推翻了这个答案,但不是因为当时想错了——是因为"全局可寻址"这个
 * 优点的背面是"全系统只有一份",而 Lab9 第一次出现了会 yield 出去、之后
 * 还要正常返回的系统调用（sys_wait/pipe_read/pipe_write/console_read）。
 * 一个进程让出 CPU 期间,别的进程每次陷入内核都会覆写同一个 sepc CSR、
 * 同一个 trap_saved_user_sp 全局变量,它被换回来时 sret 恢复的就是别人的
 * PC 和别人的用户栈。两个值因此被搬进 trap_entry.S 的栈帧（TF_SEPC/TF_SP
 * 两个槽位,随进程冻结在它自己的内核栈上),完整推导见 trap_entry.S 顶部
 * 模块注释。搬完之后它们不再全局可寻址——栈帧地址只有"这次 trap 的调用链"
 * 知道——于是 riscv64 也必须像 x86_64 那样把槽位地址一路传下来。
 *
 * 两边最终签名逐字相同,但"为什么需要传指针"的理由不同：x86_64 是硬件
 * 决定的（用户 RIP 在寄存器里,无处可改),riscv64 是我们自己选的（为了
 * 正确性,主动把值从 CPU 全局存储搬到 per-process 存储)。参数名沿用
 * x86_64 的 user_rip_slot/user_rsp_slot——riscv64 这边它们指向 sepc/sp
 * 两个槽位,不为一个纯命名差异牺牲两份文件的可对照性。
 *
 * 具体做法：
 *   - sys_fork()：子进程的 trapframe（proc_alloc_skeleton() memset
 *     清零)把 tf->sepc/tf->sp 设成"父进程这次 ecall 返回后原本该恢复
 *     到的 sepc/用户 sp"——也就是 *user_rip_slot / *user_rsp_slot 两个
 *     槽位当前的值（sepc 那个已经是 +4 之后的值,见 trap.c
 *     supervisor_trap_handler 里 `frame[FRAME_SEPC] += 4` 处的注释)。
 *     子进程 trapframe 的 a0（fork() 返回值寄存器）保持骨架清零时的 0，
 *     不需要额外清——跟 x86_64 版本"子进程 rax 保持 memset 出来的 0"
 *     是同一个手法，只是这里换成 a0。tf->sstatus 需要手工构造
 *     SPP=0/SPIE=1/SUM=1 这个常量（对应 Lab6 已删除的 enter_user_mode
 *     曾经硬编码的同一个值,现在这个常量搬进了 proc.c),不能直接读当前
 *     sstatus——当前 sstatus 描述的是"父进程这次 ecall 陷入时的现场"，
 *     SPP 此刻已经被硬件设成了 0（还没 sret,一直是 0),读到的值凑巧是
 *     对的，但这是偶然而不是可以依赖的关系，直接写常量更清楚地表达
 *     "子进程恢复到用户态、中断使能、允许访问用户内存"这三个属性是
 *     "回到用户态"这件事本身固定的，不依赖父进程此刻具体是什么状态。
 *   - sys_exec()：把两个槽位改写成新程序的入口/新栈顶。Lab7/Lab8 这里
 *     写的是 g_current->tf 加一次 write_sepc()（"写 tf 是为了文档对称,
 *     写 CSR 才真正生效"),Lab9 只写槽位一处——tf 不再需要在 exec 路径
 *     上被碰,因为 ecall 返回走的是槽位,不是 tf。这也顺带消掉了 Lab8
 *     proc.c 里那段"同一个值写两个地方"的注释所描述的别扭。
 *
 * ── 给 sys_fork 新增第三个参数 caller_frame：一个真实 bug 的修复 ──
 *
 * 上面这段"具体做法"漏了一件事：子进程 trapframe 除了 sepc/sp/sstatus/
 * a0 四个字段被显式设置之外，剩下的 ra/t0-t6/a1-a7 全部保持
 * proc_alloc_skeleton() memset 出来的 0。这在 x86_64 版本的原始设计里
 * 也存在过（同一类"fork 丢寄存器"bug，见 x86_64 proc.h 同一处"给 sys_fork
 * 新增第三个参数 gpr_snapshot"那段的完整论证），riscv64 这边论证过程不
 * 重复，只记这里的具体后果：
 *
 * 子进程要"resume 到 fork() 这次系统调用返回之后"，靠的不只是 sepc/sp
 * 两个值对——它落地的那条指令（fork() 桩函数里 ecall 之后那条 ret,见
 * user/usys_riscv64.S)执行的是 riscv64 的 jalr x0, 0(ra)，读的是 ra
 * 寄存器,而 ra 装的是"main() 里 call fork 那一刻,函数返回后该跳到哪"—
 * 这是*父进程*当时活着的 ra,只存在于父进程这次 ecall 的调用链里,不会
 * 出现在 sepc/sp 任何一个槽位。子进程 trapframe 里的 ra 若保持 0,
 * ret 就会跳到地址 0，触发一次 addr=0x0/reason=exec 的取指故障——这是
 * 本 Lab 实测踩过的真实 bug，不是假设。同理，main() 编译产物如果把
 * fork() 调用点前后活跃的某个值放进 t0-t6/a1-a7 之一（caller-saved,
 * 但"caller-saved"只表示"调用约定不保证 callee 会保留",不表示"这次
 * 具体调用一定会破坏"——子进程要接着父进程当时的执行流往下跑，这些
 * 值如果被后续代码读取,读到 0 就是错的),同样需要能被子进程看到。
 *
 * 修法：ecall_handler()（trap.c）手上已经有这次陷入的轻量栈帧指针
 * （trap_entry.S supervisor_trap_entry 保存的字段：ra/t0-t6/a0-a7/
 * s0-s11/gp/tp,32 个,256 字节——见下一段"二次修复"),把它原样转发给
 * syscall_dispatch 再转发给 sys_fork()，sys_fork() 从这个栈帧里把
 * ra/t0-t6/a1-a7/s0-s11/gp/tp 逐字段拷进子进程 trapframe 对应字段——
 * a0 故意不拷（子进程 trapframe.a0 要保持 memset 出来的 0，作为 fork()
 * 在子进程里的返回值，这跟 x86_64 版本"子进程 rax 保持 memset 出来的
 * 0"是同一个手法）。
 *
 * 这个栈帧*不是* struct trapframe（proc.h 早前那段注释已经说明两者是
 * 不同布局：轻量帧字段更少、没有 sepc/sp/sstatus——不过 Lab9 起 sepc/sp
 * 也进了轻量帧,"更少"只是相对 struct trapframe 仍然缺 sstatus)，所以
 * 这里用 `uintptr_t *caller_frame` 而不是 `struct trapframe *`，按偏移
 * 量取值——偏移量跟 trap.c FRAME_RA/FRAME_T0.../FRAME_A7 那组常量必须
 * 一致，这是本文件和 trap.c 之间又一处"没有单一权威来源、必须手工保持
 * 一致"的地方，跟 TF_SEPC/FRAME_SEPC 那对是同一类风险。
 *
 * 二次修复,推翻上一版这里写的判断：s0-s11/gp/tp 四类寄存器最初不在这个
 * 轻量帧里（trap_entry.S supervisor_trap_entry 从 Lab4 起就没保存过
 * 它们，理由见 trap_entry.S 顶部模块注释"从不需要跨越 trap 处理函数
 * 返回之后存活"那一段——那段论证对"resume 同一条执行流"这个场景本身
 * 没错，但没有覆盖到"fork() 需要子进程完整复现父进程现场"这个新场景）。
 * 上一版这里曾经写"本 Lab 的 init.c/sh.c 在 fork() 调用点前后的代码
 * 足够简单，编译产物没有触发这个缺口（这是实测确认过的，不是靠论证
 * 假设）"——这个判断是错的：紧接着的下一次实测（GDB 断在 trap.c 故障
 * 诊断行、objdump 反汇编 user/init)就发现 GCC 把 init.c main() 编译成
 * 用 s0 当帧指针访问栈上的 pid 局部变量（`sw a5, -20(s0)`,fork() 返回值
 * 先经 a0->a5 搬运),子进程 s0 停留在 0 时这条指令算出的地址是
 * `0 + (-20)` = 0xffffffffffffffec（-20 的无符号 64 位表示），直接
 * 触发"page fault: addr=0xffffffffffffffec reason=write"——用手写汇编
 * 用户程序（Lab4~Lab8 的 user_prog.S)测不出来，不代表用编译器产物测
 * 不出来，是"当前测试用例没有暴露"和"缺口不存在"这两件事被错误地
 * 划了等号，这个教训本身值得记录，不只是记录修复本身。
 *
 * 现在的修法：trap_entry.S 的轻量帧扩容到完整的 s0-s11+gp+tp（等价于
 * x86_64 版本"gpr_snapshot 只覆盖通过 SYSCALL 快速路径*不会*被硬件/
 * 编译约定自动保留的那一部分"的 riscv64 对应版本，只是 riscv64 这边
 * 选择了"扩到完整 ABI callee-saved 集合"而不是"只扩需要的那几个"，
 * 理由和代价见 trap_entry.S 顶部模块注释里"代价"那一段——每次 trap 都
 * 多付 14 组 sd/ld，不只是 fork 路径）。完整推导过程见 trap_entry.S
 * 顶部模块注释"sys_fork() 撕开的缺口"那一大段。 */
int sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
             uintptr_t *caller_frame);
int sys_exec(const char *path, char *const argv[],
             uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot);

int sys_wait(int64_t *exit_code_out);
void sys_exit_proc(int64_t code);

/* Lab8/Lab9：文件相关的系统调用实现（在 trap.c 里，紧挨着 sys_write）。
 * 返回值约定跟 POSIX 一致：成功返回非负数，失败返回负数。这几个原型跟
 * x86_64 那份逐字相同——它们不碰任何架构专属的现场（sepc/trapframe），
 * 只读写 struct proc 里的 ofile[] 和调用 fs.c/pipe.c，所以两边的实现也是
 * 逐字相同的（trap.c 里那六个函数可以整段互换,Lab9 就是这么做的)。
 *
 * Lab8 时这一段注释接着说"对照 sys_fork/sys_exec 两边签名都不一样,
 * 这个差别本身就说明了哪些内核功能真的跟架构有关"。Lab9 之后这句话
 * 需要改写：fork/exec 两边签名现在一样了,但它们的*实现*仍然处处是
 * 架构专属的（写 sepc 槽位 vs 写 rcx 槽位、构造 sstatus vs 构造 RFLAGS)。
 * 判据因此更准确的说法是"这段代码碰不碰架构定义的东西",而不是"它的
 * 签名长什么样"——签名只是这件事一个不完全可靠的外在表现。 */
int64_t sys_open(const char *user_name);
int64_t sys_read(int fd, void *user_buf, uint64_t len);
int64_t sys_close(int fd);

/* Lab9：管道和 dup。sys_pipe 往 user_fds[0]/[1] 写读端和写端。 */
int64_t sys_pipe(int *user_fds);
int64_t sys_dup(int fd);

/* Lab9：用户地址空间的三个操作。实现在本架构的 proc.c 里，因为它们要
 * 用到 pagetable_* 和 kalloc，而调用者（exec.c）是两个架构共用的。
 * 三个函数的语义、以及"为什么它们替换掉了 Lab7/Lab8 里所有'用户空间
 * 就是那两个固定虚拟地址'的硬编码"，完整说明见 ../x86_64/proc.h 同一处
 * ——两边逐字相同的声明,没有任何 riscv64 特有的补充。 */
void uvm_track(struct proc *p, uintptr_t vaddr, uint32_t flags);
void uvm_clear(struct proc *p);
int uvm_copy(struct proc *dst, struct proc *src);

/* Lab9：ELF 加载器。实现在 exec.c 里，两个架构逐字节相同（唯一的差异
 * 是文件里那一个 #if：内核虚拟基址和期望的 e_machine）。
 *
 * 成功返回 0 并交出新进程的入口地址和初始栈指针；失败返回 -1，此时 p
 * 的地址空间没有被改动过，调用者可以继续正常运行。sys_exec() 依赖这个
 * 性质，用户程序（sh）也依赖——见 exec.c 顶部关于"提交线"的说明。 */
int exec_load(struct proc *p, const char *path, char *const argv[],
              uintptr_t *entry_out, uintptr_t *sp_out);

void scheduler(void) __attribute__((noreturn));
void yield(void);
struct proc *proc_current(void);

#endif /* OSDEV_PROC_H */
