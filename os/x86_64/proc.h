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
 * 选择（真实 xv6 是 NPROC=64），本课程只需要演示"多个进程轮转调度"。
 *
 * Lab9 从 4 提到 8。4 在 Lab7/Lab8 够用，在 Lab9 正好不够用：跑一条
 * 管道命令的时候，同时活着的进程是 init、sh、管道左边、管道右边——
 * 恰好 4 个，一点余量都没有。而 sh 每执行一条命令都要 fork，如果某个
 * 已退出的子进程还没被 wait 掉（僵尸进程也占着表项），第 5 个 proc_alloc()
 * 就会失败，表现成 shell 突然报 "fork failed" 然后行为变得莫名其妙。
 *
 * 8 是"够用且还看得清"的取值：一屏 kprintf 里 8 个 pid 仍然数得过来。
 * 上限的代价是 PCB 数组变大（每个 PCB 现在多了 upages[]，见下面），
 * 这个数组是静态分配在内核 .bss 里的，不占用 kalloc 的页预算。 */
#define NPROC 8

/* 每个进程的内核栈大小。真实内核通常给 8KiB/16KiB（Linux）。
 *
 * Lab9 从 1 页提到 2 页。Lab7/Lab8 的内核态调用深度很浅，一页够用；
 * Lab9 新增的 exec 路径明显更深：
 *
 *     syscall_entry → syscall_dispatch → sys_exec → exec_load
 *                   → load_segment → fs_read → read_inode → blk_read
 *
 * 加上 exec_load() 栈上那个 64 字节的 Elf64_Ehdr 和 56 字节的
 * Elf64_Phdr、以及 fs.c 里每层各自的局部缓冲区。一页 4KiB 也许还能
 * 撑住，但内核栈溢出的症状是最难查的一类：栈往下越界写进的是*相邻的
 * 那个物理页*，而那一页可能是另一个进程的内核栈、或者一个页表、或者
 * 某个用户程序的代码段——现场看起来是一个完全无关的地方莫名其妙地坏了，
 * 而且每次坏的地方还不一样（取决于 kalloc 那次把哪一页给了谁）。
 *
 * 本课程没有栈溢出检测（真实内核会在栈下面留一个不映射的 guard page，
 * 越界立刻变成一个指着现场的缺页异常，而不是静默的内存破坏——这是
 * README 挑战任务之一）。在没有检测手段的前提下，宁可多给一页。 */
#define PROC_KSTACK_PAGES 2

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

/* Lab9：一个 fd 可以指向三种不同的东西，所以 struct file 需要一个
 * "这是哪一种"的标签。
 *
 * Lab8 的 struct file 只有 {used, inum, off}——它只能表示"一个打开的
 * 磁盘文件"。那时候够用，因为 fd 0/1/2 是特殊照顾的：sys_write 看到
 * fd==1 就往串口打，根本不查 fd 表。
 *
 * Lab9 这么做不下去了，原因是管道。`cat /motd.txt | grep lab` 要求
 * grep 的 fd 0 是管道读端，而 grep 的代码写的是 read(0, ...)——它不
 * 知道自己在管道里，也不该知道。"fd 0 是什么"必须是运行时的、可以被
 * shell 改掉的信息，也就是必须真的存在 fd 表里。这是 Unix "一切皆文件"
 * 那句话的实际含义：不是所有东西都是磁盘文件，而是所有东西都通过同一
 * 张表、同一组 read/write 调用来访问，差别藏在表项的类型标签后面。
 *
 * 加了标签之后，sys_read/sys_write 变成一个三分支的分派：查表拿到
 * 类型，再决定去读串口、读 inode，还是读管道。fd 0/1/2 不再特殊——
 * 它们只是 proc_alloc() 时预先填好 FD_CONSOLE 的三个普通表项。 */
enum fd_type {
    FD_NONE = 0,   /* 空槽位。放在 0 上，这样清零的 PCB 天然是"全部关闭"。 */
    FD_CONSOLE,    /* 串口。读走 UART RX，写走 kprintf 那条路。 */
    FD_INODE,      /* 磁盘文件。用 inum + off。 */
    FD_PIPE,       /* 管道。用 pipe + writable。 */
};

/* struct pipe 定义在 pipe.h 里。这里只需要一个前向声明——proc.h 不
 * include pipe.h，因为它只需要"有这么个类型，我存一个指向它的指针"，
 * 不需要知道里面有什么。少一条 include 就少一条两个头文件互相包含的
 * 可能（pipe.c 要用 yield()，那是 proc.h 里的）。 */
struct pipe;

/* 一个打开的文件。
 *
 * 不管哪种类型，这三个字段合起来就是"文件描述符"这个抽象的全部内容：
 * 指向什么、读到哪里了、能不能写。
 *
 * off 存在这里、而不是存在 inode 里，是 Lab8 就讲过的关键点：同一个
 * 文件被 open() 两次会得到两个槽位、两个独立的 off。如果把"当前偏移"
 * 放进 inode（每个文件只有一个），这件事就做不到——这正是 Unix 把
 * "文件"和"打开的文件"分成两个概念的原因。
 *
 * 本 Lab 的简化（Lab8 起就有，Lab9 继续）：这张表直接嵌在 struct proc
 * 里，所以 fork() 之后父子进程各有一份*独立拷贝*，偏移不共享。真正的
 * Unix 语义是父子共享同一个 struct file，父进程读了 10 字节、子进程
 * 接着读会从第 10 字节开始。
 *
 * 注意管道在这个简化下*不能*跟着一起简化：pipe 字段是指针，fork 的
 * 结构体拷贝复制的是指针值，父子指向同一个 struct pipe——这是必须的，
 * 管道的全部意义就是两个进程共享一个缓冲区。所以本 Lab 的 fd 表是
 * 半共享的：off 各一份，管道本体共享。这个不一致是简化的代价，README
 * 的挑战任务里有"把 struct file 挪到全局表 + 引用计数"的完整说明。 */
struct file {
    enum fd_type type;
    uint32_t inum;        /* FD_INODE：文件的 inode 号。 */
    uint32_t off;          /* FD_INODE：下一次 read 从第几个字节开始。 */
    struct pipe *pipe;     /* FD_PIPE：指向共享的管道本体。 */
    int writable;          /* FD_PIPE：这一端是写端还是读端。 */
};

/* Lab9：每个进程的用户页数量上限。
 *
 * 为什么突然需要这个上限，而 Lab7/Lab8 不需要：那两个 Lab 里每个进程的
 * 用户地址空间是*写死的*——一页代码（USER_PROG_VADDR）加一页栈
 * （USER_STACK_VADDR），fork 拷这两页，exec 换这两页，代码里就是两行。
 * Lab9 的地址空间由 ELF 文件决定，页数和虚拟地址都是运行时才知道的，
 * "这个进程有哪些用户页"必须被真正记下来。
 *
 * 16 的来源：本 Lab 最大的程序 grep 是 1 页代码 + 1 页数据 + 2 页栈 = 4 页。
 * 16 留了 4 倍余量，学生把某个程序写大一点不会撞上限。
 *
 * 上限不能随意放大，因为有一个真实的天花板。kalloc 的可用池是
 * [0x110000, 0x300000)，496 页——上界是内核自映射窗口的尽头
 * （self_map_end = KERNEL_LOAD_ADDR + 2MiB，见 kernel_main.c）。超出
 * 这个窗口的物理页 kalloc 也会给，但内核没法通过"物理地址 + 偏移"访问
 * 它们，而页表页和 exec 正在填的用户页都必须这样访问。kalloc 是从低地址
 * 开始的 first-fit，所以只要同时活着的页总数不超过 496，每次分配都还在
 * 窗口里；一超过，某次分配就会返回一个内核碰不到的地址，然后在一个跟
 * 分配点毫无关系的地方炸掉。
 *
 * 按 NPROC=8 算最坏情况：8 × (约 5 页页表 + 2 页内核栈 + 16 页用户) =
 * 184 页，离 496 还很远。实际用量 8 × (5 + 2 + 4) = 88 页。 */
#define NUSERPAGE 16

/* 一个用户页的记录：它在用户地址空间的哪里，以及用什么权限映射的。
 *
 * 只记虚拟地址、不记物理地址——物理地址随时可以用 pagetable_lookup()
 * 从页表里查出来，记两份就会有两份不一致的可能（页表改了、这里没改）。
 * 页表是"这个页映射到哪"的唯一权威，upages[] 只回答页表回答不了的那个
 * 问题："哪些虚拟地址是这个进程的用户页"——页表没法反向枚举，它是一棵
 * 稀疏的树，要枚举就得遍历整棵树的每一级。
 *
 * flags 必须记，因为 fork 要用。子进程的每一页要用*跟父进程相同的权限*
 * 映射：代码段是只读可执行，数据段是可写不可执行，权限抄错的后果是
 * 子进程能写自己的代码段（安全问题）或者不能写自己的数据段（一跑就崩）。
 * 从页表项里反解 flags 也能做到，但那要求 pagetable.h 再暴露一个
 * "查权限"的接口，而且反解要处理两个架构不同的 PTE 位布局——记下来
 * 便宜得多。 */
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
 * 是有意义、有真实来源的。
 *
 * 两个参数都传*地址*而不是*值*，即使 sys_fork() 自己不需要写它们——
 * 统一成同一种签名，syscall_entry（trap_entry.S）不需要为 sys_fork/
 * sys_exec 分别准备两种不同的调用方式。sys_exec() 需要写：它要改写
 * "这次 syscall 返回后 rcx/用户栈指针应该恢复成什么"，这两个值物理上
 * 存在 syscall_entry 的栈帧位置（rcx 那份)和 trap.c 的 syscall_saved_
 * user_rsp 全局变量（rsp 那份)里，只有传地址才能真正写回去。
 *
 * Lab9 订正上面这句话的后半：用户 RSP 那一份不再存在全局变量里，而是
 * 存在 syscall_entry 压到*本进程内核栈*上的一个槽位里,rcx 那份本来就
 * 在内核栈上。改动的原因见 trap_entry.S 里 syscall_entry 的函数头注释:
 * Lab9 第一次出现"会 yield 出去、之后还要正常返回"的系统调用,全系统
 * 一份的全局变量会被并发的另一个进程的系统调用覆盖。对这两个函数的
 * 签名没有影响——它们拿到的一直是"一个能写的地址",地址指向哪里是
 * syscall_entry 的事。
 *
 * Lab9 给 sys_exec 多加了两个参数：path 和 argv。Lab7/Lab8 的 exec 加载
 * 的是编译进内核镜像的那一份用户程序,没得选,所以不需要参数;Lab9 的
 * exec 从文件系统里按路径加载 ELF,这两个参数才是它真正的输入。参数
 * 顺序跟 POSIX 的 execv(path, argv) 对齐,两个槽位排在后面。
 *
 * ── Lab9 给 sys_fork 新增第三个参数 gpr_snapshot：一个真实 bug 的修复 ──
 *
 * 上面这段注释在 Lab9 修这个 bug之前，原话是"用户程序在本课程的教学
 * 范围内也从不依赖'某个通用寄存器的值能跨越一次 syscall 存活'"——这句
 * 话对 Lab7/Lab8 手写汇编的 user_prog.S 是对的，但从 Lab9 引入 GCC 编译
 * 的 init.c/sh.c 开始就不再成立：C 调用约定要求 rbp/rbx/r12-r15
 * （callee-saved 寄存器）在任何函数调用前后保持不变，fork() 从调用者的
 * 角度看就是一次普通的函数调用，没有理由是例外。这份课程原来的 sys_fork()
 * 只设置子进程 trapframe 的 rip/rsp/cs/ss/rflags 五个字段，其余寄存器
 * 保持 proc_alloc_skeleton() memset 出来的 0——子进程执行到 fork() 返回
 * 后的第一条指令（典型的是 `mov %eax, -N(%rbp)`，把返回值存进局部变量)
 * 时，rbp=0 跟栈上实际数据的摆放位置完全对不上，直接缺页崩溃。完整的
 * 症状描述、GDB 复现过程、根因推导见 trap_entry.S 顶部"Lab9 修复：
 * fork() 丢失父进程 callee-saved 寄存器的 bug"那段模块注释——这里
 * 只记录接口变化：gpr_snapshot 指向 syscall_entry 在换栈之后立刻存下的
 * 一份 rbx/rbp/r12-r15 快照（struct context * 类型，字段顺序跟 proc.h
 * 已有的 struct context 一致，见 trap_entry.S 里 push 顺序的注释），
 * sys_fork() 把它们逐字段拷进子进程 trapframe 对应的字段。
 *
 * sys_exec() 不需要这个参数：exec 是"替换当前进程的整个地址空间"，
 * 替换之后旧的 rbp/rbx/r12-r15 不再指向任何有意义的东西（栈本身都换了
 * 内容），新程序的 _start/main 会按它自己的方式重新建立栈帧，不依赖
 * "exec 之前 rbp 是什么"——这跟 fork() 需要"这条执行路径原封不动地
 * 在另一个地址空间里继续跑下去"是完全不同的两件事。 */
int sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
             struct context *gpr_snapshot);
int sys_exec(const char *path, char *const argv[],
             uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot);

int sys_wait(int64_t *exit_code_out);
void sys_exit_proc(int64_t code);

/* Lab8/Lab9：文件相关的系统调用实现（在 trap.c 里，紧挨着 sys_write）。
 * 返回值约定跟 POSIX 一致：成功返回非负数，失败返回负数。 */
int64_t sys_open(const char *user_name);
int64_t sys_read(int fd, void *user_buf, uint64_t len);
int64_t sys_close(int fd);

/* Lab9：管道和 dup。sys_pipe 往 user_fds[0]/[1] 写读端和写端。 */
int64_t sys_pipe(int *user_fds);
int64_t sys_dup(int fd);

/* Lab9：用户地址空间的三个操作。实现在本架构的 proc.c 里，因为它们要
 * 用到 pagetable_* 和 kalloc，而调用者（exec.c）是两个架构共用的。
 *
 * 这三个函数替换掉了 Lab7/Lab8 里所有"用户空间就是那两个固定虚拟地址"
 * 的硬编码——那个假设在 Lab9 随着 ELF 加载器一起失效了。
 *
 * uvm_track：记下"刚刚给这个进程映射了一页"。只更新 upages[]，不建映射
 *   （调用者刚 pagetable_map() 过）。超出 NUSERPAGE 会 panic——调用它的
 *   地方都在 exec 的提交线之后，没有退路，而 exec_load() 已经在提交线
 *   之前用页数预算挡掉了这种情况，所以真的触发说明预算算错了。
 *
 * uvm_clear：拆掉所有用户页的映射并把物理页还给 kalloc，清空 upages[]。
 *   内核范围的映射一个都不动——调用者正跑在内核栈上。
 *
 * uvm_copy：把 src 的每一个用户页复制一份给 dst（新分配物理页 + 拷内容 +
 *   用相同 flags 建映射），成功返回 0，内存不够返回 -1。这是 fork 的
 *   核心动作，也是它唯一可能失败的地方。 */
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
