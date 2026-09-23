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
static uintptr_t read_sepc(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, sepc" : "=r"(value));
    return value;
}

static void write_sepc(uintptr_t value)
{
    __asm__ volatile("csrw sepc, %0" : : "r"(value));
}

/* trap_saved_user_sp：trap_entry.S 在 ecall-from-U-mode 分支里,换到
 * __stack_top 之前,把用户 sp 暂存到这里——对应 x86_64 版本的
 * syscall_saved_user_rsp,同一个问题的同一种解法（单核场景下用一个
 * 全局槽位就够,不需要 per-hart 存储,本课程从不引入多核,见 x86_64
 * 版本对应位置更详细的解释）。这个符号只被 trap_entry.S 用汇编直接
 * 读写（`la`/`sd`/`ld`),trap.c 里的 C 代码从来不直接访问它——放在
 * 这个文件只是因为"全局变量需要一个定义它的编译单元",跟
 * __stack_top 定义在 linker.ld/boot.S 而被 trap_entry.S extern 引用
 * 是同一种"定义和使用不在同一个文件"的关系。 */
uint64_t trap_saved_user_sp;

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
 * ecall 分支（ecall_handler,上面)不需要这层保护：它从不调用 yield(),
 * 不存在 detour,trap_entry.S 保存的 ra/t0-t6/a0-a7 全程留在这次 trap
 * 自己冻结的内核栈上,sepc 只在这次 trap 内部被 write_sepc(+4)一次、
 * 随后 sret 立刻读取,中间不可能被其它进程的 trap 插入覆写（同一个
 * hart 不能同时处理两次 trap）。 */
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
    struct proc *p = proc_current();
    if (p != NULL) {
        p->tf->sepc = read_sepc();
        yield();
        write_sepc(p->tf->sepc);
    }
}

/* sys_write：跟 x86_64 版本（trap.c 的同名函数)语义完全一致,包括
 * "用户合法地址在当前页表下对内核也是合法地址,本 Lab 用户页表和
 * 内核页表是同一份,不做地址校验"这个简化——见 x86_64 版本 sys_write
 * 上方的完整注释,riscv64 侧共用同一个理由,不重复展开。 */
static int64_t sys_write(const char *user_ptr, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        console_putc(user_ptr[i]);
    }
    return (int64_t)len;
}

/* ---------------------------------------------------------------------------
 * Lab8：open / read / close
 * ---------------------------------------------------------------------------
 * 这三个函数跟 x86_64 版本**逐字相同**（连注释都可以照搬，这里只保留
 * 要点，完整展开见 x86_64/trap.c 同一节）。这件事本身值得注意：
 *
 *   sys_fork/sys_exec 两边签名和实现都不一样（一边要传 rip/rsp 槽位
 *   指针，一边直接读写 sepc CSR 和全局变量），而 sys_open/sys_read/
 *   sys_close 两边一模一样。区别在于前者操作的是"陷入现场"——那是架构
 *   定义的东西；后者操作的是 struct proc 里的 ofile[] 和 fs.c——那是
 *   我们自己定义的东西。
 *
 * 这条分界线是判断"一段内核代码该不该放进架构目录"的实用判据：它碰
 * CSR/段寄存器/页表位布局吗？碰，就是架构相关的；只碰自己设计的结构，
 * 就不是。fs.c 能在两边逐字节相同，是同一条判据的更彻底的应用。
 *
 * 为什么这三个函数仍然放在各自架构的 trap.c 里、而不是提到共享文件：
 * 它们要访问 struct proc（fd 表长在 PCB 里），而 struct proc 的
 * trapframe/context 字段是架构专属的，proc.h 因此无法共用。这是一个
 * 妥协的结果而不是理想的划分——真实内核（xv6、Linux）会把 fd 表和
 * "进程的架构现场"拆成两个结构体，前者共用、后者按架构分。本课程为了
 * 让 PCB 保持成一个能一眼看完的结构体，接受了这处重复。
 *
 * 用户指针不做校验，跟 sys_write 同一个简化（本 Lab 用户页表和内核页表
 * 共用同一份映射）。read() 比 write() 更危险：它是*往*用户给的地址
 * *写*数据，真实内核里不校验就是一个直接可用的提权原语。
 */

/* fd -> struct file* 的解析。三个系统调用共用，把"fd 合法性检查"收在
 * 一处，而不是在每个调用里各写一遍 if。 */
static struct file *fd_lookup(struct proc *p, int fd)
{
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    if (!p->ofile[fd].used) {
        return NULL;
    }
    return &p->ofile[fd];
}

int64_t sys_open(const char *user_name)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_open: 没有当前进程——系统调用只能来自用户进程");
    }

    uint32_t inum = fs_lookup(user_name);
    if (inum == 0) {
        /* 文件不存在。返回 -1 而不是 panic：这是*用户程序*可能犯的错，
         * 不是内核的错误。区分这两类失败是内核设计里一条重要的纪律——
         * 用户程序的任何行为都不应该能把内核搞停；只有内核自己的不变式
         * 被破坏时才 panic。 */
        return -1;
    }

    for (int fd = 0; fd < NOFILE; fd++) {
        if (!p->ofile[fd].used) {
            p->ofile[fd].used = 1;
            p->ofile[fd].inum = inum;
            p->ofile[fd].off = 0;
            return fd;
        }
    }

    return -1; /* fd 表满了。真实 Unix 在这里返回 EMFILE。 */
}

int64_t sys_read(int fd, void *user_buf, uint64_t len)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_read: 没有当前进程——系统调用只能来自用户进程");
    }

    struct file *f = fd_lookup(p, fd);
    if (f == NULL) {
        return -1;
    }

    uint32_t got = fs_read(f->inum, f->off, user_buf, (uint32_t)len);

    /* 推进偏移。这一行就是"文件描述符携带状态"的全部实现。 */
    f->off += got;

    /* got == 0 表示 EOF，跟 POSIX read(2) 一致——返回 0 不是错误。 */
    return (int64_t)got;
}

int64_t sys_close(int fd)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_close: 没有当前进程——系统调用只能来自用户进程");
    }

    struct file *f = fd_lookup(p, fd);
    if (f == NULL) {
        /* 关一个没打开的 fd：返回 -1。user_prog.S 故意做一次重复 close
         * 来验证这条路径——"关两次第二次要失败"说明 used 标志真的被清掉
         * 了，而不是 close 什么都没做。 */
        return -1;
    }

    /* 清空整个槽位，不只是 used=0：残留的 inum 会让错误看起来像正常
     * 工作，归零让"用了一个已关闭的 fd"更容易在调试时暴露。 */
    f->used = 0;
    f->inum = 0;
    f->off = 0;
    return 0;
}

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
 * （子进程 pid,或者 -1),不读 exit_code。 */
extern int sys_fork(void);
extern int sys_exec(void);
extern int sys_wait(int64_t *exit_code_out);
extern void sys_exit_proc(int64_t code);

/* syscall_dispatch：跟 x86_64 版本同名函数同一个签名/同一个分发逻辑
 * ——num/a0/a1 对应 riscv64 syscall 传参约定里的 a7（调用号)/a0/a1
 * （前两个参数,user_prog.S 已经按这个约定填好),对应 x86_64 版本的
 * rax/rdi/rsi。返回值约定：riscv64 侧沿用 a0 传返回值（跟 SBI 调用
 * 约定"a0=返回值"是同一个寄存器角色,而不是 x86_64 侧借用的 Linux
 * syscall ABI"rax 传返回值"惯例——两边选的"用哪个寄存器传返回值"这个
 * 具体选择不同,但"用调用号/首个参数寄存器传返回值"这个思路是一致的,
 * 都是各自架构里最自然的选择,不是刻意求同或求异)。
 *
 * SYS_FORK/SYS_EXEC 不用 a0/a1（proc.h 里 sys_fork()/sys_exec() 都是
 * void 参数)——跟 x86_64 版本 sys_fork()/sys_exec() 需要
 * user_rip_slot/user_rsp_slot 两个指针参数不同：x86_64 的 SYSCALL
 * 现场（rcx=用户RIP/syscall_saved_user_rsp 全局变量)只在
 * syscall_entry 这一次调用栈帧里以*局部*形式存在,没有第二个全局可寻址
 * 的位置能找到"这次系统调用返回后该恢复到哪个 RIP/RSP",必须靠调用者
 * （trap_entry.S)显式传地址下去。riscv64 这边等价的两个值——"返回后
 * 该恢复到哪个 PC"和"用哪个用户栈"——分别就是 sepc 这个 CSR 本身
 * （trap.c 顶部 read_sepc()/write_sepc() 已经在直接操作它)和
 * trap_saved_user_sp 这个全局变量（本文件顶部已经定义),两者*本身*
 * 就是全局可寻址、随时可以被任何函数直接 read_sepc()/写 trap_saved_
 * user_sp 修改的位置,不需要再额外传一层指针——proc.h 里 sys_fork/
 * sys_exec 声明处已经写了这段对比,这里再次提到是因为 syscall_
 * dispatch 正是这两个函数实际被调用的地方,读者在这里最容易联想到
 * "为什么参数列表长得不一样"这个问题。 */
/* Lab8 新增第三个参数 a2：sys_read(fd, buf, len) 需要三个参数，前两个
 * 装不下。
 *
 * riscv64 这边这个改动几乎是免费的，跟 x86_64 那边形成一个有意思的对照：
 * trap_entry.S 的轻量栈帧从 Lab4 起就保存了 a0-a7 全部八个参数寄存器
 * （`sd a2, 80(sp)`，一直在那儿），所以加第三个参数只需要多读一个已经
 * 存在的槽位，汇编一行都不用改。x86_64 那边则要动 trap_entry.S。
 *
 * 这不是哪个架构"设计得更好"，而是两边当初做了不同的取舍：riscv64 侧
 * 一开始就按"完整参数寄存器组"保存（照 ABI 划线，不按当时的需要裁剪），
 * x86_64 侧的 SYSCALL 路径按"当时实际用到的寄存器"保存。前者多花了几条
 * sd 指令，换来后续扩展不动汇编。 */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2)
{
    switch (num) {
    case SYS_WRITE:
        return sys_write((const char *)a0, a1);
    case SYS_EXIT:
        sys_exit_proc((int64_t)a0);
        return 0;
    case SYS_FORK:
        return sys_fork();
    case SYS_EXEC:
        return sys_exec();
    case SYS_WAIT:
        return sys_wait(NULL);
    case SYS_OPEN:
        return sys_open((const char *)a0);
    case SYS_READ:
        return sys_read((int)a0, (void *)a1, a2);
    case SYS_CLOSE:
        return sys_close((int)a0);
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
 * 行为不变。frame[8]/frame[9]/frame[15] 分别对应 trap_entry.S 里
 * `sd a0, 64(sp)`/`sd a1, 72(sp)`/`sd a7, 120(sp)`（字节偏移÷8=
 * uint64_t 数组下标),偏移量必须跟那边保持同步——这是本 Lab 又一处
 * "没有单一数据源、需要人肉对齐"的手工契约,README 会一并指出,跟
 * USER_PROG_VADDR 手写字面量是同一类。
 *
 * 返回值写回 frame[8]（对应 a0 的槽位)——trap_entry.S 的 epilogue会
 * 把这个槽位 ld 回真正的 a0 寄存器,再 sret 回用户态,用户态代码从
 * a0 读到的就是这个返回值,对应 syscall.h 头部注释、user_prog.S 里
 * "结果回 a0"这条约定。 */
static void ecall_handler(uint64_t *frame)
{
    uint64_t num = frame[15]; /* a7 */
    uint64_t a0  = frame[8];  /* a0 */
    uint64_t a1  = frame[9];  /* a1 */
    uint64_t a2  = frame[10]; /* a2，Lab8 新增——对应 trap_entry.S 里
                               * `sd a2, 80(sp)`（80÷8=10）。那一行从
                               * Lab4 起就在，不需要改汇编。 */

    int64_t ret = syscall_dispatch(num, a0, a1, a2);

    frame[8] = (uint64_t)ret;
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
         * 再执行一次,见上面模块顶部注释里对 sepc 这一段的详细解释。
         * 必须在 ecall_handler 分发之前做（虽然本 Lab 两个系统调用
         * 都不依赖 sepc,提前做是更"正确"的顺序——真实场景里如果
         * 某个系统调用处理函数自己需要读 sepc（比如实现某种基于返回
         * 地址的机制),读到的应该已经是"+4 之后"的值,不应该依赖分发
         * 顺序）。 */
        write_sepc(read_sepc() + 4);
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
