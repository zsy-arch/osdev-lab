/* Lab6 riscv64：在 Lab5"page fault 三种 + 定时器一种"的基础上,加入
 * 第三条陷入路径——ecall（Environment Call）。跟 x86_64 版本
 * （../x86_64/trap.c）的开篇提醒是同一件事,但结论相反：x86_64 的
 * SYSCALL 完全绕开 IDT,是独立于原有陷入路径的第三条通道；riscv64 的
 * ecall 和 page fault/定时器*共享同一个* stvec 入口（Direct 模式的
 * 本质决定的——trap_entry.S 的注释已经展开过这一点),这里体现为
 * supervisor_trap_handler() 的 scause if 链里多一个分支,不是新增一个
 * 独立的处理函数入口。
 *
 * riscv-privileged 规范"Machine Cause Register (mcause)"表格：
 * scause=8 是"Environment call from U-mode"（scause=9 是"from
 * S-mode",本课程内核从不对自己 ecall,不会触发,不需要处理)。跟
 * page fault 一样是异常（scause 最高位=0),不是中断,分发位置在
 * if (scause & SCAUSE_INTERRUPT_BIT) 判断之后、page fault 判断之前
 * 都可以,这里选在 page fault 判断之前,理由是"ecall 是本 Lab 教学
 * 主线,放在 if 链靠前的位置,阅读时先看到主线,再看到边界情况
 * （page fault 仍然是'未实现故障恢复'的兜底)"。 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "sbi.h"
#include "syscall.h"
#include "proc.h"
#include "fs.h"   /* Lab8: fs_lookup/fs_read——sys_open/sys_read 的实际工作在那边 */
#include "pipe.h" /* Lab9: pipe_alloc/read/write/close/dup——fd 表的第三种类型 */

#define SCAUSE_INTERRUPT_BIT   (1ull << 63)
#define SCAUSE_ECALL_FROM_U    8ull
#define SCAUSE_INSN_PAGE_FAULT 12ull
#define SCAUSE_LOAD_PAGE_FAULT 13ull
#define SCAUSE_STORE_PAGE_FAULT 15ull

#define SCAUSE_INT_SUPERVISOR_TIMER 5ull

#define TIMER_HZ 100ull

static uintptr_t read_stval(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, stval" : "=r"(value));
    return value;
}

static uint64_t read_scause(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, scause" : "=r"(value));
    return value;
}

static uint64_t read_time(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, time" : "=r"(value));
    return value;
}

/* sepc：trap 发生那一刻的 PC（riscv-privileged 规范"sepc"一节)——
 * ecall 场景下必须显式加 4（跳过 ecall 指令本身,4 字节,riscv64 不用
 * 压缩指令集时每条指令固定 4 字节),否则 sret 会回到*刚才那条 ecall*
 * 本身,再执行一次 ecall,陷入无限循环。这跟 x86_64 SYSCALL 的行为
 * 不同：SYSCALL 指令执行时硬件自动把 RCX 设成"SYSCALL 之后那条指令"
 * 的地址（AMD64 手册明确写出来的硬件行为,syscall_entry 注释已经提过
 * 这一点),riscv64 的 ecall/sepc 没有这个自动前进,软件必须自己处理
 * ——这是本 Lab README 会点出的"riscv64 ecall vs x86_64 SYSCALL"
 * 具体差异之一。 */
/* Lab9 起这个 +4 落在*栈帧槽位*上（frame[FRAME_SEPC],见下面
 * supervisor_trap_handler),不再通过 csrr/csrw 直接改 CSR——Lab4~Lab8
 * 在这里定义的 read_sepc()/write_sepc() 两个 static 辅助函数随之删除。
 * 理由是 trap_entry.S 现在把 sepc 存进了栈帧,如果 C 代码继续写 CSR,
 * 同一个逻辑值就有了两份存储、而出口写回的是槽位那一份,+4 会被冲掉,
 * 形成同一条 ecall 无限重复的死循环——这个故障 Lab6~Lab8 真实踩过,
 * 完整经过见 trap_entry.S 顶部模块注释。改成只写槽位之后,CSR 只在
 * sret 那一刻被硬件读一次,冲突从根上消失。 */

/* 栈帧槽位下标。trap_entry.S 里 TF_* 是*字节偏移*,这里是 uint64_t
 * 数组下标,换算关系是 ÷8——同一份契约的两种写法,必须同步修改,这是
 * 本 Lab 又一处"没有单一数据源、需要人肉对齐"的手工契约（跟
 * USER_PROG_VADDR 那类字面量同属一类,README 会一并指出)。
 *
 * 只列出 C 代码真正会用到的几个,不做成完整的 32 项表格——完整表格
 * 的唯一权威在 trap_entry.S,在这里复制一份反而多一个会过期的副本。 */
#define FRAME_A0    8    /* trap_entry.S TF_A0   = 64  */
#define FRAME_A1    9    /* trap_entry.S TF_A1   = 72  */
#define FRAME_A2    10   /* trap_entry.S TF_A2   = 80  */
#define FRAME_A7    15   /* trap_entry.S TF_A7   = 120 */
#define FRAME_SEPC  16   /* trap_entry.S TF_SEPC = 128,Lab9 新增 */
#define FRAME_SP    17   /* trap_entry.S TF_SP   = 136,Lab9 新增 */

/* trap_saved_user_sp 在 Lab9 被删除（Lab6~Lab8 在这里有一个定义)——
 * 用户 sp 现在存在栈帧的 frame[FRAME_SP] 槽位里。删除的理由不是"清理
 * 代码",是一个真实的正确性问题：这个全局变量只有在"从写入到读出之间
 * 不可能有别的进程陷入内核"的前提下才是安全的,而 Lab9 第一次打破了
 * 这个前提（sys_wait/pipe_read/pipe_write/console_read 全都在 ecall
 * 内部 yield())。完整推导见 trap_entry.S 顶部模块注释。 */

/* trap_kernel_sp_top：Lab7 新增,对应 x86_64 版本的 syscall_kernel_rsp
 * ——trap_entry.S 的 from_user 分支换栈时读的就是这个变量当前存的值,
 * scheduler()（proc.c）在 swtch() 进某个进程之前负责把它设成那个进程
 * 自己的内核栈顶。跟 trap_saved_user_sp 一样,只被 trap_entry.S 用
 * 汇编 `la`/`ld` 直接读,定义放在这个文件只是因为需要一个编译单元
 * 承载它,不代表这个文件的 C 代码会主动使用它（不会——写它的是
 * proc.c,读它的是 trap_entry.S,trap.c 本身两头都不碰)。
 *
 * 初值不设成 0 或别的哨兵值：Lab7 的 kernel_boot() 严格顺序是"trap_
 * init()+timer_enable() -> proc_init()+proc_alloc() -> scheduler()",
 * 第一次真正的 trap（无论 ecall 还是定时器)必然发生在 scheduler() 已经
 * 通过 swtch() 进入某个进程*之后*——swtch() 进程之前 scheduler() 一定
 * 先执行过"把 trap_kernel_sp_top 设成这个进程内核栈顶"这一步,所以不
 * 存在"在这个变量被正确赋值之前就有 trap 发生"的时间窗口,不需要额外
 * 的初值防御。 */
uint64_t trap_kernel_sp_top;

volatile uint64_t timer_ticks = 0;

/* proc_current()/yield()：Lab7 新增的 proc.c 函数,声明在 proc.h（本
 * 文件现在直接 #include "proc.h",不再需要本地 extern 声明——引入这个
 * #include 是为了下面 timer_interrupt_handler() 需要用到 struct proc/
 * struct trapframe 的完整定义,不只是函数原型)。timer_interrupt_
 * handler() 需要知道"现在是不是真的有一个进程在跑",不能无条件调用
 * yield()：如果定时器 tick 恰好落在 scheduler() 自己的 for 循环里
 * （没有任何 RUNNABLE 进程,或者刚 swtch() 回调度器、还没找到下一个
 * 进程的间隙),g_current 是 NULL,这种情况下 yield() 内部会直接 return
 * （proc.c yield() 自己也做了这个检查),调用它本身不会出错——但仍然
 * 显式判断一次更清楚地表达"这里的 yield 是有条件的",跟 x86_64 版本
 * pit.c 里同一处判断是同一个理由。 */

/* yield() 前后必须手动保护 sepc 这个 CPU 全局 CSR，真实调试出来的教训
 * ——跟 proc.c write_sepc() 上方那段"sys_exec() 只写 tf 不够"的教训
 * 是同一类根因，这里是它的第二个真实案例：
 *
 * 最早的实现是直接 `if (proc_current() != NULL) yield();`,不做任何
 * sepc 处理。触发条件：这次定时器 tick 打断的是进程 A,A 被标成
 * RUNNABLE、swtch() 回 scheduler() 之后,scheduler() 的 for 循环继续
 * 往下找,可能会先 swtch() 进*另一个*进程 B（A 自己的 for 循环位置在
 * A 让出之前已经过去了,不会立刻转回来找 A),B 执行期间自己的每一次
 * trap（B 的 ecall、B 的下一次定时器 tick、甚至 trap.c 里内核自己
 * 调 sbi_set_timer 触发的 supervisor ecall)都会在硬件层面覆写同一个
 * sepc CSR——sepc 是 per-hart 的,不是 per-process 的存储。等 A 终于
 * 被 scheduler() 重新 swtch() 回来,一路从 swtch() 返回点 return 回到
 * 这个函数、回到 trap_entry.S、执行 `sret`,CSR 里躺着的早已是 B（或
 * 更晚的某个进程)最后一次 trap 留下的 sepc,不是 A 被这次定时器打断
 * 那一刻的真实 sepc——A 会从一个完全不相关的地址继续"执行",实测复现：
 * `qemu-system-riscv64 -d int` 能看到某个进程的 epc 突然跳到另一个
 * 进程的地址范围。
 *
 * 这跟 sys_exec() 那次教训是同一个根因（sepc 是全局 CSR,只多"写"
 * 是不够的,必须保证"读"的那一刻还没被别人覆写),但保护窗口不同：
 * sys_exec() 需要保护的是"写完到 sret 之间"（同一次 trap 内,没有
 * detour),这里需要保护的是"yield() 调用前到 yield() 返回后之间"
 * （跨越 swtch() detour,可能隔着任意长时间、任意多个其它进程的
 * trap）。用 g_current->tf->sepc（struct trapframe 里本来就有的
 * 字段,proc.h 顶部模块注释已经交待过它的双重身份)当这次的容器：
 * yield() 调用前把当前 sepc 存进去,yield() 内部 swtch() 让出、又被
 * scheduler() swtch() 换回来、返回到这里之后,再把它读出来写回 CSR。
 * 这个字段是*这个进程自己*的存储（struct proc 的成员,不是全局变量),
 * 不会被其它进程的 trap 覆写,能够真正跨越这段 detour 存活——跟
 * ra/t0-t6/a0-a7 靠"冻结在自己的内核栈上"存活是同一个原理，只是
 * sepc 是 CSR 不在通用寄存器里，没有栈帧会自动帮它做这件事，需要
 * 这里手动做。
 *
 * Lab8 当时还在这里写了一句"ecall 分支不需要这层保护：它从不调用
 * yield()"——Lab9 这句话不再成立。sys_wait()、pipe_read()、
 * pipe_write()、console_read() 全都在 ecall 内部 yield(),ecall 分支
 * 现在跟定时器分支一样会经历 detour。如果 Lab9 还沿用"哪条路径需要
 * 保护就在那条路径里手工加代码"这个思路,这里就得在 ecall 分支也补一遍
 * 同样的存取,而且每次新增一个会阻塞的系统调用都要重新确认一次有没有
 * 漏掉——这正是把它挪进 trap_entry.S 栈帧的理由：槽位对所有路径一视
 * 同仁,不需要任何一条路径记得照顾它。 */
static void timer_interrupt_handler(void)
{
    timer_ticks++;

    uint64_t interval = 10000000ull / TIMER_HZ;
    sbi_set_timer(read_time() + interval);

    /* 抢占式轮转调度的触发点：每次定时器 tick,只要当前确实有一个
     * 进程在跑,就让它 yield()——跟 x86_64 版本 pit.c 里
     * `if (proc_current() != NULL) yield();` 是同一个位置、同一个
     * 条件,只是 x86_64 那边在 EOI 之后才能 yield（PIC 边沿触发的
     * 顾虑,那边注释有完整解释),riscv64 这边的"重新装定时器"
     * （sbi_set_timer,上面那一行)不是 PIC 那种"必须先应答才能收到
     * 下一次中断"的边沿触发机制,SBI 定时器下一次触发完全由"新设的
     * 目标时间"决定,跟"是否已经调用过 sbi_set_timer"这件事本身没有
     * 顺序上的强制关系——但仍然保持"先重新装定时器,再 yield()"这个
     * 顺序,不是因为必须如此,而是"下一次还能不能被打断"这个属性
     * 应该尽早确定,不依赖 yield() 内部 swtch() 切换之后隔了多久才
     * 回到这里继续执行完剩下的部分)。 */
    /* Lab9：这里只剩一次 yield() 调用。Lab7/Lab8 版本是三行——
     * `p->tf->sepc = read_sepc(); yield(); write_sepc(p->tf->sepc);`
     * ——现在 sepc 由 trap_entry.S 的栈帧槽位负责,yield() 期间别的进程
     * 怎么覆写 sepc CSR 都无关紧要,这个进程被换回来之后出口从*自己的*
     * 槽位把它 csrw 回去。
     *
     * 顺带消掉了一个隐藏依赖：Lab8 那三行必须拿到 struct proc *p 才能
     * 找到存 sepc 的地方,于是"保护 sepc"这件事被迫依赖"当前有一个进程
     * 在跑"。两者本来没有关系——sepc 需要被保护的原因是 yield() 可能
     * 让出 CPU,跟 g_current 是不是 NULL 无关。现在 `if (p != NULL)`
     * 只表达它字面的意思了：没有进程在跑的时候没什么可以被抢占。 */
    struct proc *p = proc_current();
    if (p != NULL) {
        yield();
    }
}

/* ---------------------------------------------------------------------------
 * Lab9：fd 表——open / read / write / close / pipe / dup
 * ---------------------------------------------------------------------------
 * 这一整节跟 x86_64 版本（trap.c 同一节）**逐字相同**——包括注释、包括
 * 每一条 TODO 的编号。这件事本身值得注意，因为它跟 sys_fork/sys_exec
 * 那两个函数（两边签名和实现都不一样）形成了鲜明对照：
 *
 *   sys_fork/sys_exec 操作的是"陷入现场"——rip/rsp 槽位指针、CSR、
 *   栈帧布局——那是架构定义的东西，两边不可能相同。而这一节的六个函数
 *   （fd_lookup/fd_alloc/fd_clear/console_read/sys_write/sys_open/
 *   sys_read/sys_close/sys_pipe/sys_dup）只碰 struct proc 里的 ofile[]、
 *   pipe.c、fs.c——全都是我们自己设计的数据结构，不碰任何 CSR/段寄存器/
 *   页表位布局。
 *
 * 这条分界线是判断"一段内核代码该不该放进架构目录"的实用判据：碰
 * CSR/段寄存器/页表位布局，就是架构相关的；只碰自己设计的结构，就不是。
 * fs.c/pipe.c 能在两边逐字节相同，是同一条判据的更彻底的应用——这一节
 * 是判据应用到"放在 trap.c 里的架构相关文件、但内容本身架构无关"这种
 * 中间状态的例子。
 *
 * 为什么这十个函数仍然放在各自架构的 trap.c 里、而不是提到共享文件：
 * 它们要访问 struct proc（fd 表长在 PCB 里），而 struct proc 的
 * trapframe/context 字段是架构专属的，proc.h 因此无法共用。这是一个
 * 妥协的结果而不是理想的划分——真实内核（xv6、Linux）会把 fd 表和
 * "进程的架构现场"拆成两个结构体，前者共用、后者按架构分。本课程为了
 * 让 PCB 保持成一个能一眼看完的结构体，接受了这处重复。
 *
 * ── 一切皆文件 ──────────────────────────────────────────────────
 *
 * Lab8 的 fd 表只有一种表项："磁盘上的一个 inode，加一个读取偏移"。
 * Lab9 加入控制台（fd 0/1，早已存在，只是之前没有走 fd 表）和管道
 * （sh 实现 `|` 的唯一手段）之后，同一张表里躺着三种性质完全不同的东西，
 * 却要被同一套 sys_read/sys_write 用同一种方式访问——这正是 struct file
 * 加一个 type 标签（FD_CONSOLE/FD_INODE/FD_PIPE）的理由：调用者不需要
 * 关心 fd 背后到底是什么，只需要知道"这是一个 fd"。
 *
 * 换句话说，本 Lab 真正实现的是 Unix 那句"一切皆文件"——它的含义不是
 * "所有东西都是磁盘文件"，而是"所有东西都通过同一张表、同一组 read/write
 * 来访问，差别藏在表项的类型标签后面"。下面这十个函数就是那句话的代码。
 *
 * ── 用户指针：本 Lab 一律直接解引用，不校验 ──────────────────────
 *
 * 下面每个函数拿到的用户指针（buf/name/fds）都是直接当地址用的，没有
 * 检查"它真的落在调用者自己的用户地址空间里、真的可读/可写"。
 *
 * 真实内核必须校验，否则用户程序可以传一个内核地址，骗内核帮它读或写
 * 任意内存——这是一整类真实的特权提升漏洞。read 方向尤其危险，因为它是
 * *往*用户给的地址*写*：传一个内核数据结构的地址进来，内核就替你把文件
 * 内容覆盖到内核内存里，这是一个直接可用的提权原语。
 *
 * 本 Lab 不做校验是教学简化，README 的简化清单和挑战任务里都有这一条
 * （copy_from_user / access_ok）。需要注意的是这个简化在 Lab9 比在 Lab8
 * 更"假"：Lab6/Lab7 早期用户和内核共用一份页表，用户的合法地址对内核也
 * 合法，不校验在当时真的不改变任何行为；到了 Lab9，每个进程有自己的页表,
 * 一个恶意的用户指针完全可以指向只有内核映射的地址，此时不校验就是实打
 * 实的漏洞，只是本 Lab 的用户程序都不这么干而已。
 *
 * ── 拷过来之前请先读完这段 ────────────────────────────────────────
 *
 * 下面 TODO 1~5（fd_lookup/fd_alloc/fd_clear/console_read/sys_write）
 * 的提示代码跟 ../x86_64/trap.c 同一节逐字相同，可以直接整段拷过来——
 * 两边不共享一个文件，只是不约而同地有了同一份实现。sys_open/sys_read/
 * sys_close/sys_pipe/sys_dup 这五个（x86_64 那边编号 TODO 6~10）riscv64
 * 侧不再重复列出提示——同样是逐字拷过来，重复贴一遍提示文字没有新信息，
 * 直接去 ../x86_64/trap.c 找对应 TODO 的提示代码即可。
 *
 * 建议的做法：先在 x86_64 下写完并跑过测试，理解每一步的理由，再把这
 * 十个函数整段拷过来。拷完之后回头想一下为什么它们能这样拷——本节开头
 * 那段注释给了判据。
 *
 * 唯一需要自己写的 riscv64 相关部分是下面 syscall_dispatch 里对应的
 * case（TODO 11），以及 kernel_main.c 里的 virtio MMIO 映射。
 */

/* 前向声明：sys_write 是本文件内部的 static 函数（不像 sys_open/sys_read/
 * sys_close/sys_pipe/sys_dup 那五个——它们是 proc.h 里已经声明好的非
 * static 函数，声明和定义分离，TODO 6-10 只是暂时留空函数体，syscall_
 * dispatch() 编译时看到的是 proc.h 里那份声明，跟函数体是否已经填好
 * 无关）。sys_write 没有这样一份独立于函数体的声明——它在 solution 里
 * 单纯是定义在 syscall_dispatch() 前面的一个 static 函数，C 语言"函数
 * 用之前必须先见过声明或定义"这条规则靠"定义写在调用点上面"就自动满足，
 * 不需要专门再写一份原型。
 *
 * TODO 5 把 sys_write 的函数体整段注释掉之后，这个"定义在调用点上面"
 * 的隐式保证也随之消失：下面 syscall_dispatch() 里的 SYS_WRITE 分支
 * 是 bucket-0（Lab7/8 就已经写好的真代码，不是本 Lab 的 TODO，见那一
 * 行自己的注释)，会在 TODO 5 填好之前就先被编译器看到——如果没有这行
 * 前向声明，会报 "implicit declaration of function 'sys_write'"，而
 * 且是在动这个文件其他任何 TODO 之前就会先炸的第一个编译错误，跟
 * TODO 编号顺序毫无关系,容易误以为是自己的 TODO 1-4 哪里写错了。 */
static int64_t sys_write(int fd, const char *user_buf, uint64_t len);

/* TODO 1：实现 fd_lookup——把 fd 解析成 struct file*，非法返回 NULL。
 *
 * 为什么单独抽一个函数：下面几个系统调用都要做同一套 fd 合法性检查。收在
 * 一处而不是各写一遍 if，是为了让"什么样的 fd 算合法"只有一个定义——
 * 这类检查散落在多处时，迟早有一处漏掉某个条件。
 *
 * 两个条件：
 *   1. fd < 0 || fd >= NOFILE     -> NULL（越界）
 *   2. p->ofile[fd].type == FD_NONE -> NULL（这个槽位没打开）
 * 都过了就返回 &p->ofile[fd]。
 *
 * 第 1 条的下界不能漏：fd 是 int，用户程序可以传 -1。只检查上界的话，
 * p->ofile[-1] 会读到 struct proc 里 ofile 前面那个字段的内存——一个
 * 由用户程序控制的负数下标是标准的内核漏洞形状。
 *
 * 第 2 条判据是 Lab9 相对 Lab8 的变化：用 `type != FD_NONE` 取代了旧的
 * `used` 字段。这不只是换个名字——FD_NONE 定为 0，于是"清零的 PCB 等于
 * 所有 fd 都关闭"这个性质是免费的，而且一个字段同时承担了"在不在用"和
 * "是什么"两件事，不会出现 used=1 但类型没设这种自相矛盾的中间状态。
 *
 * 提示：
 * static struct file *fd_lookup(struct proc *p, int fd)
 * {
 *     if (fd < 0 || fd >= NOFILE) {
 *         return NULL;
 *     }
 *     if (p->ofile[fd].type == FD_NONE) {
 *         return NULL;
 *     }
 *     return &p->ofile[fd];
 * }
 */

/* TODO 2：实现 fd_alloc——找一个空闲 fd，返回*最小*的那个；没有空位返回 -1。
 *
 * "最小"不是审美偏好，是 POSIX 明确规定的行为（open/dup 都必须返回当前
 * 最小的可用 fd），而且 shell 的重定向直接依赖它。user/sh.c 里那句
 *
 *     close(0); if (dup(fd) != 0) { ...报错... }
 *
 * 的全部正确性就建立在这上面：先把 0 关掉，此时 0 成为最小的空位，于是
 * dup 一定返回 0，新打开的文件就顶替到了 fd 0 的位置上。如果这里改成
 * "从头扫但返回第一个碰到的"以外的任何策略（比如轮转、或者从高往低），
 * dup 会返回别的数字，sh 那个 if 会报错——这是刻意留下的、会大声失败的
 * 检查，而不是静默的错误重定向。
 *
 * 提示：
 * static int fd_alloc(struct proc *p)
 * {
 *     for (int fd = 0; fd < NOFILE; fd++) {
 *         if (p->ofile[fd].type == FD_NONE) {
 *             return fd;
 *         }
 *     }
 *     return -1;
 * }
 */

/* TODO 3：实现 fd_clear——把一个 fd 槽位清空。
 *
 * 全部字段归零、而不只是把 type 设成 FD_NONE：残留的 inum/pipe 指针会让
 * "用了一个已关闭的 fd"这类 bug 看起来像在正常工作（表项里还有个像样的
 * inode 号），而清零之后同样的 bug 会立刻撞上 fd_lookup 的 FD_NONE 检查。
 * 同样是"让错误尽早变成可观察的失败"那条纪律。
 *
 * 注意这个函数*不*管管道的引用计数——调用者负责在清空之前先调 pipe_close。
 * 分成两步而不是在这里一并处理，是因为 sys_exit_proc 那边也要走同一条
 * 清理逻辑，而它的调用时机和 sys_close 不同。
 *
 * 提示：
 * static void fd_clear(struct file *f)
 * {
 *     f->type = FD_NONE;
 *     f->inum = 0;
 *     f->off = 0;
 *     f->pipe = NULL;
 *     f->writable = 0;
 * }
 */

/* TODO 4：实现 console_read——从控制台读。至少读到一个字节才返回，读到
 * 换行就停。
 *
 * ── 为什么这里必须阻塞，而 console_getc() 必须不阻塞 ──────────────
 *
 * console_getc() 是硬件层：查一下 UART 状态，有就拿走，没有就说没有。
 * 它不能等,因为在那一层"等"只有忙转一种写法，而单核 + 系统调用期间
 * 关中断意味着忙转会卡死整个系统——定时器打不进来，调度器永远没机会跑。
 *
 * 这里是策略层，可以等，因为这里能 yield()：把 CPU 让给别的进程，下次
 * 轮到自己再看一眼。代价是忙等（这个进程会被反复调度、反复检查），但
 * 系统整体还在动。这就是"底层提供机制、上层决定策略"这句话的一个具体
 * 例子——同一个 console_getc()，中断驱动的实现会把这里换成"挂进等待
 * 队列"，而下面那一层一行都不用改。
 *
 * ── 为什么读到换行就停，而不是凑满 len ───────────────────────────
 *
 * 行缓冲是终端的标准行为（POSIX 的 canonical mode）。sh.c 其实是一个字节
 * 一个字节读的（len 恒为 1），所以这个判断对它没有影响；但对任何用大
 * 缓冲区读一行的程序来说，没有这个判断就得等到缓冲区满才返回，用户按了
 * 回车却没反应。
 *
 * ── 永不返回 0 ──────────────────────────────────────────────────
 *
 * 控制台没有 EOF 这个概念：串口那头的人可能只是还没开始打字。所以这个
 * 函数在没有输入时永远等下去，绝不返回 0。这件事有一个直接的后果，值得
 * 记住：交互式 sh 在 read(0) 上会永远挂住，于是自动测试跑到最后不是
 * "程序结束"，而是 QEMU 超时退出（exit code 124），test-lab.sh 把 124
 * 当成合法退出码正是为了这个。见 user/init.c 末尾的注释。
 *
 * 步骤：
 *   1. len == 0：直接返回 0。
 *   2. 循环，每次 console_getc()：
 *      - c < 0（没有输入）：已经读到东西（got > 0）就先交付（break）；
 *        一个字节都没读到就 yield() 继续等（continue）。
 *      - c == '\r'：转成 '\n'（回车当换行，见下方提示的完整说明）。
 *      - console_putc((char)c) 回显。
 *      - user_buf[got++] = (char)c。
 *      - c == '\n'：读到换行，break。
 *   3. return (int64_t)got。
 *
 * 提示：
 * static int64_t console_read(char *user_buf, uint64_t len)
 * {
 *     if (len == 0) {
 *         return 0;
 *     }
 *
 *     uint64_t got = 0;
 *     while (got < len) {
 *         int c = console_getc();
 *         if (c < 0) {
 *             if (got > 0) {
 *                 break;
 *             }
 *             yield();
 *             continue;
 *         }
 *
 *         if (c == '\r') {
 *             c = '\n';
 *         }
 *
 *         console_putc((char)c);
 *         user_buf[got++] = (char)c;
 *
 *         if (c == '\n') {
 *             break;
 *         }
 *     }
 *
 *     return (int64_t)got;
 * }
 */

/* TODO 5：实现 sys_write(fd, buf, len) -> 实际写了多少字节。
 *
 * Lab8 的版本没有 fd 参数，看见什么都往串口打。Lab9 加上 fd 并真正分派——
 * 这一个改动就是管道能工作的全部内核侧前提。
 *
 * 步骤：
 *   1. proc_current()，为 NULL 就 panic。
 *   2. fd_lookup()，为 NULL 返回 -1。
 *   3. 按 f->type 分派：
 *      - FD_CONSOLE：逐字节 console_putc，返回 len。
 *      - FD_PIPE：!f->writable（拿错了 fd，往读端写）返回 -1；否则
 *        pipe_write(f->pipe, user_buf, (uint32_t)len)。
 *      - FD_INODE：返回 -1——本 Lab 的文件系统是只读的（fs.c 里没有任何
 *        写路径），"往文件写"是一个诚实的失败，不是没实现完。shell 的
 *        `>` 重定向因此不存在，README 的已知限制里有记录。
 *
 * 提示：
 * static int64_t sys_write(int fd, const char *user_buf, uint64_t len)
 * {
 *     struct proc *p = proc_current();
 *     if (p == NULL) {
 *         panic("sys_write: 没有当前进程——系统调用只能来自用户进程");
 *     }
 *
 *     struct file *f = fd_lookup(p, fd);
 *     if (f == NULL) {
 *         return -1;
 *     }
 *
 *     switch (f->type) {
 *     case FD_CONSOLE:
 *         for (uint64_t i = 0; i < len; i++) {
 *             console_putc(user_buf[i]);
 *         }
 *         return (int64_t)len;
 *
 *     case FD_PIPE:
 *         if (!f->writable) {
 *             return -1;
 *         }
 *         return (int64_t)pipe_write(f->pipe, user_buf, (uint32_t)len);
 *
 *     case FD_INODE:
 *         return -1;
 *
 *     default:
 *         panic("sys_write: fd 表项的类型标签是未知值——PCB 被写坏了");
 *     }
 * }
 */

/* Lab7：sys_exit 占位版本（只打印,不真正结束进程)已被移除,SYS_EXIT
 * 改路由到 proc.c 的 sys_exit_proc()——跟 x86_64 版本同一处改动同一个
 * 理由,见 proc.c sys_exit_proc() 顶部注释。sys_wait() 需要一个
 * int64_t* 出参传出子进程的 exit_code,ecall 路径这边"参数"只有 a0/
 * a1（syscall_dispatch 签名固定是 num/a0/a1,SYS_WAIT 语义上不需要
 * 任何输入参数),没有地方能装一个"指向调用者想要接收结果的位置"的
 * 用户指针——本 Lab 选择最简单的处理：sys_wait() 直接传 NULL（跟
 * proc.c sys_wait() 声明处"exit_code_out 为 NULL 时不写出参,只返回
 * pid"的行为一致),用户程序如果想知道子进程的 exit_code,本 Lab 范围
 * 内暂不提供这条路径（ROADMAP 明确要求的是"实现 fork/exec/wait/exit
 * 机制本身",不是"完整暴露 exit_code 给用户态"这个更完整的功能),跟
 * user_prog.S 测试程序设计保持一致——它只关心 sys_wait() 的返回值
 * （子进程 pid,或者 -1),不读 exit_code。
 *
 * Lab6~Lab8 在这里声明了 `extern int sys_fork(void); extern int
 * sys_exec(void); extern int sys_wait(int64_t *exit_code_out); extern
 * void sys_exit_proc(int64_t code);` 四行——当时 proc.h 还没有为这几个
 * 函数写完整原型（或者原型跟这里的签名不完全一致),这四行 extern
 * 是"先用起来,原型的事以后再说"的权宜之计。Lab9 起 proc.h 已经用完整、
 * 匹配的签名声明了它们（sys_fork 需要 uintptr_t* 槽位指针,sys_exec
 * 需要 path/argv 等),本文件顶部 #include "proc.h" 已经带来了这些
 * 声明,这里的本地 extern 会跟 proc.h 里的原型不一致（参数个数都不同),
 * 继续留着不会报错（C 允许同一个符号有多次前向声明,只要类型兼容——
 * 但这四行跟 proc.h 的原型*不兼容*，编译器会报"conflicting types"这个
 * 硬错误),必须删除，交给 proc.h 做唯一的声明来源。 */

/* syscall_dispatch：num/a0/a1/a2 对应 riscv64 传参约定里的 a7（调用号)/
 * a0/a1/a2（前三个参数),ecall_handler 从栈帧槽位里取出来传进来。返回值
 * 沿用 a0——跟 SBI 调用约定"a0=返回值"是同一个寄存器角色,而不是 x86_64
 * 侧借用的 Linux syscall ABI"rax 传返回值"惯例。两边选的"用哪个寄存器
 * 传返回值"这个具体选择不同,但"用调用号/首个参数寄存器传返回值"这个
 * 思路是一致的,都是各自架构里最自然的选择,不是刻意求同或求异。
 *
 * 签名 Lab9 起跟 x86_64 版本逐字相同（包括 user_rip_slot/user_rsp_slot
 * 这两个参数名——riscv64 这边它们指向的是 sepc/sp 两个槽位,沿用 x86_64
 * 的参数名是为了让两份文件能直接对照,不为一个纯粹的命名差异牺牲可比性)。
 *
 * Lab7/Lab8 这里*没有*这两个参数,而且当时的注释花了一整段论证 riscv64
 * 天生不需要它们：sepc 是 CSR、用户 sp 在全局变量里,两者都全局可寻址,
 * proc.c 里的 sys_fork()/sys_exec() 直接读写就行。那段论证本身没错,但
 * 它依赖的前提在 Lab9 被主动推翻了——这两个值被从"CPU 全局存储"搬进了
 * "per-process 的内核栈帧",因为全局存储装不住每个进程各自的值（完整
 * 推导见 trap_entry.S 顶部模块注释）。搬完之后,栈帧的地址只有*这次
 * trap 的调用链*知道,proc.c 里的函数没有别的办法找到它,于是 riscv64
 * 也必须像 x86_64 那样把槽位地址一路传下去。
 *
 * 这是本课程里少见的"两边从不同起点收敛到同一个设计"的例子。x86_64
 * 从 Lab7 起就必须传指针（用户 RIP 躺在 rcx 寄存器里,除了栈帧槽位没有
 * 第二个地方能改它);riscv64 走了两个 Lab 的弯路才到同一个地方,而走弯路
 * 的那两个 Lab 里代码是正确的——是需求变了,不是当时想错了。
 *
 * Lab8 新增的第三个参数 a2 在 riscv64 这边几乎是免费的,跟 x86_64 形成
 * 一个有意思的对照：trap_entry.S 的轻量栈帧从 Lab4 起就保存了 a0-a7
 * 全部八个参数寄存器（`sd a2, TF_A2(sp)`,一直在那儿),加第三个参数只是
 * 多读一个已经存在的槽位,汇编一行都不用改；x86_64 那边要动 trap_entry.S
 * 的寄存器搬移序列。这不是哪个架构"设计得更好",是两边当初做了不同的
 * 取舍：riscv64 侧一开始就按"完整参数寄存器组"保存（照 ABI 划线,不按
 * 当时的需要裁剪),x86_64 侧的 SYSCALL 路径按"当时实际用到的寄存器"保存。
 * 前者多花了几条 sd 指令,换来后续扩展不动汇编。 */

int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                          uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
                          uint64_t *caller_frame)
{
    switch (num) {
    case SYS_WRITE:
        /* Lab9：多了 fd 参数。Lab8 的 sys_write 只有 (buf, len)，看见什么
         * fd 都往串口打；现在 fd 决定往哪写，这是管道能工作的前提。 */
        return sys_write((int)a0, (const char *)a1, a2);
    case SYS_EXIT:
        sys_exit_proc((int64_t)a0);
        return 0; /* 不可达——sys_exit_proc() 内部 swtch() 回调度器，
                   * 见 proc.c 自己的注释；写 return 只是让这个 switch
                   * 分支在类型上完整，不依赖编译器能推断出它不可达。 */
    case SYS_FORK:
        /* Lab9：caller_frame 是这次 ecall 陷入时开在栈上的轻量帧起始
         * 地址（ecall_handler 直接转发它自己收到的 frame 参数）,不是
         * 某个槎位的地址——sys_fork() 要从里面抄 ra/t0-t6/a1-a7,不只是
         * sepc/sp 两个槎位。完整原因见 proc.h sys_fork 声明处、proc.c
         * sys_fork() 函数头两处大注释,不重复展开。 */
        return sys_fork(user_rip_slot, user_rsp_slot,
                         (uintptr_t *)caller_frame);
    case SYS_EXEC:
        /* Lab9：exec 从"加载内核里那份写死的用户程序"变成"按路径加载
         * 文件系统里的 ELF"，于是多了 path/argv 两个参数。a0/a1 是用户
         * 传来的指针，这里只做类型转换、不做校验——校验是 exec_load() 的
         * 事（它比这里更清楚什么样的 path 算合法）。
         *
         * 真实内核在这一层还要做一件本 Lab 省掉的事：确认 a0/a1 确实
         * 指向调用者*自己*的用户地址空间。少了这个检查，用户程序可以
         * 传一个内核地址进来让内核去读——这是一整类权限漏洞的源头。
         * 本 Lab 统一省掉这类检查（sys_write/sys_read/sys_wait 也一样），
         * 补上它是 README 里的挑战任务（copy_from_user/access_ok）。 */
        return sys_exec((const char *)a0, (char *const *)a1,
                        user_rip_slot, user_rsp_slot);
    case SYS_WAIT:
        return sys_wait((int64_t *)a0);
    /* TODO 11：加上 SYS_OPEN / SYS_READ / SYS_CLOSE / SYS_PIPE / SYS_DUP
     * 五个 case，跟 x86_64 版本同一节完全一样：
     *   SYS_OPEN  -> sys_open((const char *)a0);
     *   SYS_READ  -> sys_read((int)a0, (void *)a1, a2);
     *   SYS_CLOSE -> sys_close((int)a0);
     *   SYS_PIPE  -> sys_pipe((int *)a0);
     *   SYS_DUP   -> sys_dup((int)a0);
     *
     * SYS_READ 用到**三个**参数——riscv64 这边不需要额外注意什么，
     * trap_entry.S 的栈帧从 Lab4 起就一直在保存 a0-a7 全部八个寄存器，
     * a2 早就在 frame[FRAME_A2] 里，ecall_handler 已经替你读出来传进来
     * 了（跟 x86_64 那边需要检查 trap_entry.S 是否搬移了对应寄存器不同，
     * 这里天然不会漏）。
     */
    default:
        kprintf("syscall_dispatch: unknown syscall number %lu\n", num);
        return -1;
    }
}

/* ecall_handler：从 supervisor_trap_entry 保存好的栈帧（trap_entry.S
 * 里 sd 顺序 ra/t0-t6/a0-a7 这 16 个字段,偏移 0-120,128 字节,Lab7 原样
 * 未变)里取出 a7/a0/a1,对应
 * x86_64 版本 syscall_entry 汇编里手动搬移 rax/rdi/rsi 到
 * syscall_dispatch 参数寄存器这一步——riscv64 这边的等价动作发生在
 * C 代码里而不是汇编里,因为 supervisor_trap_handler() 需要保持"零
 * 特权参数、自己读 CSR 决定分发"这个 Lab4/5 就定下的整体结构（page
 * fault/定时器两条路径完全不需要动这个栈帧),但 ecall 分支确实需要
 * 拿到原始寄存器值,不能只靠 CSR——syscall 号/参数是通过通用寄存器
 * 传的,不是 CSR。
 *
 * 折中方案：trap_entry.S 把 call supervisor_trap_handler 改成传一个
 * 参数（a0=sp,即栈帧基址),supervisor_trap_handler 从"零参数"变成
 * "一个参数"——这是相对 Lab4/5 的一处签名变化,但只影响 Lab6 自己
 * 这份独立的 trap.c/trap_entry.S 副本,不影响 Lab4/5 已经验证过的
 * 文件,page fault/定时器分支在新签名下只是多了一个从未被读取的参数,
 * 行为不变。FRAME_A0/FRAME_A1/FRAME_A7（本文件顶部定义)分别对应
 * trap_entry.S 里 `sd a0, TF_A0(sp)`/`sd a1, TF_A1(sp)`/
 * `sd a7, TF_A7(sp)`（字节偏移÷8=uint64_t 数组下标),两边的下标/偏移
 * 必须保持同步——这是本 Lab 又一处"没有单一数据源、需要人肉对齐"的
 * 手工契约,README 会一并指出,跟 USER_PROG_VADDR 手写字面量是同一类。
 *
 * 返回值写回 frame[FRAME_A0]（对应 a0 的槽位)——trap_entry.S 的
 * epilogue 会把这个槽位 ld 回真正的 a0 寄存器,再 sret 回用户态,用户态
 * 代码从 a0 读到的就是这个返回值,对应 syscall.h 头部注释、user_prog.S
 * 里"结果回 a0"这条约定。 */
static void ecall_handler(uint64_t *frame)
{
    uint64_t num = frame[FRAME_A7];
    uint64_t a0  = frame[FRAME_A0];
    uint64_t a1  = frame[FRAME_A1];
    uint64_t a2  = frame[FRAME_A2];

    /* Lab9：多传两个指针,指向这次 trap 栈帧里 sepc/sp 两个槽位。
     * 签名跟 x86_64 版本的 syscall_dispatch 现在逐字相同——这是 Lab9
     * 一个不太显眼但挺重要的收敛：Lab7/Lab8 时期 riscv64 这边不需要
     * 这两个参数,因为 sepc 是 CSR、用户 sp 在全局变量里,sys_fork()/
     * sys_exec() 在 proc.c 里直接读写就行（proc.h 有一整段注释论证
     * "riscv64 天生不需要传指针")。那段论证在 Lab9 失效了：两个值都
     * 搬进了 per-process 的栈帧,而栈帧的地址只有*这次 trap 的调用链*
     * 知道,proc.c 里的函数没有别的办法找到它——于是 riscv64 也必须
     * 像 x86_64 那样把槽位地址一路传下去。
     *
     * 值得留意的是两边"为什么需要传指针"的理由并不相同：x86_64 是因为
     * 用户 RIP 躺在 rcx 这个*寄存器*里、只能通过 syscall_entry 那次
     * 压栈的栈帧槽位去改；riscv64 是因为这两个值被主动从 CPU 全局存储
     * 搬到了 per-process 存储。不同的出发点,同一个终点。
     *
     * 再多传的第三个东西——frame 本身（原样转发,不是某个槽位的地址)
     * ——是 Lab9 修一个真实 bug 时加的：sys_fork() 之前只靠 sepc/sp
     * 两个槽位重建子进程的 trapframe,拼不出子进程该有的 ra/t0-t6/
     * a1-a7,这些字段就停留在 proc_alloc_skeleton() memset 出来的 0。
     * 后果是子进程 sret 到用户态后,执行到 fork() 系统调用桩函数自己
     * 的 ret 指令时,ra=0,直接跳到地址 0 取指,触发
     * "page fault: addr=0x0 reason=exec"——完整推导过程见 proc.c
     * sys_fork() 函数头注释,这里只记接口变化：syscall_dispatch 多一个
     * `uint64_t *caller_frame` 参数,只有 SYS_FORK 分支用得到,其余分支
     * 收到但从不读——跟 x86_64 版本 gpr_snapshot"六个系统调用签名都要
     * 加这个参数,五个从不用它"是同一类"接口一致性优先于每个分支各自
     * 精简"的取舍。 */
    int64_t ret = syscall_dispatch(num, a0, a1, a2,
                                   (uintptr_t *)&frame[FRAME_SEPC],
                                   (uintptr_t *)&frame[FRAME_SP],
                                   frame);

    frame[FRAME_A0] = (uint64_t)ret;
}

void supervisor_trap_handler(uint64_t *frame)
{
    uint64_t scause = read_scause();

    if (scause & SCAUSE_INTERRUPT_BIT) {
        uint64_t code = scause & ~SCAUSE_INTERRUPT_BIT;
        if (code == SCAUSE_INT_SUPERVISOR_TIMER) {
            timer_interrupt_handler();
            return;
        }
        kprintf("supervisor_trap_handler: unexpected interrupt code=%lu\n", code);
        panic("supervisor_trap_handler: unhandled interrupt (Lab5/6 only handle the timer)");
    }

    if (scause == SCAUSE_ECALL_FROM_U) {
        /* sepc+4：跳过 ecall 指令本身,不然 sret 会回到刚才那条 ecall
         * 再执行一次,见上面 FRAME_* 定义上方对 sepc 这一段的详细解释。
         *
         * Lab9 这一行从 `write_sepc(read_sepc() + 4)` 变成对槽位做 +4。
         * 不只是"换个写法"——Lab7/Lab8 那个写法在 trap_entry.S 开始保存
         * sepc 之后会直接失效（出口写回槽位,把 +4 冲掉,同一条 ecall
         * 无限重复),这是真实踩过的故障,见 trap_entry.S 顶部模块注释。
         *
         * 必须在 ecall_handler 分发之前做,而且 Lab9 起这件事比之前更
         * 要紧：sys_fork() 要把这个值抄进子进程的 trapframe,当作"子进程
         * 从 fork() 返回后该继续执行的地址"。如果 +4 发生在分发之后,
         * 子进程拿到的就是"那条 ecall 自己"的地址,一被调度就会再执行
         * 一次 fork——Lab7/Lab8 靠 read_sepc() 读 CSR 时顺序同样重要,
         * 只是那时错了的话故障现象是"父进程重复 ecall",现在是"子进程
         * 重复 fork",后者更难一眼看出来。 */
        frame[FRAME_SEPC] += 4;
        ecall_handler(frame);
        return;
    }

    if (scause != SCAUSE_INSN_PAGE_FAULT &&
        scause != SCAUSE_LOAD_PAGE_FAULT &&
        scause != SCAUSE_STORE_PAGE_FAULT) {
        kprintf("supervisor_trap_handler: unexpected scause=%lu\n", scause);
        panic("supervisor_trap_handler: unrecognized exception (Lab4/5/6 only handle page faults + timer + ecall)");
    }

    uintptr_t fault_addr = read_stval();
    const char *reason = (scause == SCAUSE_INSN_PAGE_FAULT) ? "exec" :
                          (scause == SCAUSE_LOAD_PAGE_FAULT) ? "read" : "write";

    kprintf("page fault: addr=%p reason=%s (scause=%lu)\n",
            (void *)fault_addr, reason, scause);

    panic("supervisor_trap_handler: unrecoverable page fault (Lab4/5/6 do not implement fault recovery)");
}

extern void supervisor_trap_entry(void);

void trap_init(void)
{
    uintptr_t entry = (uintptr_t)supervisor_trap_entry;
    __asm__ volatile("csrw stvec, %0" : : "r"(entry));
}

void timer_enable(void)
{
    uint64_t sie;
    __asm__ volatile("csrr %0, sie" : "=r"(sie));
    sie |= (1ull << 5);
    __asm__ volatile("csrw sie, %0" : : "r"(sie));

    uint64_t sstatus;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= (1ull << 1);
    __asm__ volatile("csrw sstatus, %0" : : "r"(sstatus));
}
