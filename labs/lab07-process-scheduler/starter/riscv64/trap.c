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

/* TODO 1：实现 read_sepc/write_sepc（跟 Lab6 完全一样，本 Lab 没有
 * 改动这两个函数本身——真正变化的是它们*被调用的地方*：Lab6 只在
 * ecall 分支里 write_sepc(+4) 一次；本 Lab 额外新增了 sys_exec()/
 * timer_interrupt_handler() 两处调用点，都是为了应对"sepc 是
 * per-hart CSR，不是 per-process 存储"这个问题，具体见 write_sepc()
 * 在 proc.c 里的调用处、以及本文件下面 timer_interrupt_handler()
 * 上方的模块级注释。
 *
 * sepc：trap 发生那一刻的 PC（riscv-privileged 规范"sepc"一节)——
 * ecall 场景下必须显式加 4（跳过 ecall 指令本身,4 字节,riscv64 不用
 * 压缩指令集时每条指令固定 4 字节),否则 sret 会回到*刚才那条 ecall*
 * 本身,再执行一次 ecall,陷入无限循环。这跟 x86_64 SYSCALL 的行为
 * 不同：SYSCALL 指令执行时硬件自动把 RCX 设成"SYSCALL 之后那条指令"
 * 的地址（AMD64 手册明确写出来的硬件行为,syscall_entry 注释已经提过
 * 这一点),riscv64 的 ecall/sepc 没有这个自动前进,软件必须自己处理
 * ——这是本 Lab README 会点出的"riscv64 ecall vs x86_64 SYSCALL"
 * 具体差异之一。
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

    /* TODO 2：抢占式轮转调度的触发点——如果当前确实有一个进程在跑
     * （proc_current() != NULL），保护 sepc 之后调用 yield()，再恢复
     * sepc。
     *
     * 为什么必须手动保护 sepc（本 Lab 实测调试出来的真实教训，见上面
     * 模块级注释"yield() 前后必须手动保护 sepc 这个 CPU 全局 CSR"的
     * 完整叙述，这里只概括结论）：sepc 是 per-hart 的 CSR，不是
     * per-process 的存储。yield() 内部会 swtch() 让出执行权给
     * scheduler()，scheduler() 可能再 swtch() 进另一个进程，那个进程
     * 自己的每一次 trap 都会覆写同一个 sepc——等这个进程终于被重新
     * swtch() 回来、一路返回到这个函数、回到 trap_entry.S 执行 sret
     * 时，CSR 里躺着的早已是别的进程最后一次 trap 留下的 sepc，不是
     * 这次定时器 tick 打断它那一刻的真实值。修复：把 sepc 存进
     * p->tf->sepc（struct proc 自己的存储，不会被其它进程的 trap
     * 覆写，能跨越 yield() 内部的 swtch() detour 存活），yield()
     * 返回后再写回 CSR。
     *
     * 提示：
     * struct proc *p = proc_current();
     * if (p != NULL) {
     *     p->tf->sepc = read_sepc();
     *     yield();
     *     write_sepc(p->tf->sepc);
     * }
     */
}

/* TODO 3：实现 sys_write（跟 Lab6 完全一样)。
 *
 * 跟 x86_64 版本（trap.c 的同名函数)语义完全一致,包括"用户合法地址在
 * 当前页表下对内核也是合法地址,本 Lab 用户页表和内核页表是同一份,不做
 * 地址校验"这个简化——见 x86_64 版本 sys_write 上方的完整注释,riscv64
 * 侧共用同一个理由,不重复展开。
 *
 * 注意 Lab6 时期这里还有一个 sys_exit 占位版本（只打印,不真正结束
 * 进程),本 Lab 已经把它整个删除——SYS_EXIT 现在直接路由到 proc.c 的
 * sys_exit_proc()（下面 syscall_dispatch 部分能看到),不需要本文件里
 * 再有一个同名的占位函数。
 *
 * 提示：
 * static int64_t sys_write(const char *user_ptr, uint64_t len)
 * {
 *     for (uint64_t i = 0; i < len; i++) {
 *         console_putc(user_ptr[i]);
 *     }
 *     return (int64_t)len;
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
/* TODO 4：实现 syscall_dispatch 的 switch 分支。
 *
 * SYS_WRITE 跟 Lab6 完全一样。SYS_EXIT 改路由到 proc.c 的
 * sys_exit_proc()（Lab6 那个只打印退出码的占位版 sys_exit 已被移除，
 * 见上面注释）。新增 SYS_FORK/SYS_EXEC 直接转发给 proc.c 的同名函数,
 * 不需要任何参数（riscv64 这边"返回后恢复到哪个 PC/用哪个用户栈"
 * 直接是 sepc CSR 和 trap_saved_user_sp 全局变量本身,不需要像 x86_64
 * 版本那样额外传指针,见本文件上方 syscall_dispatch 声明处的完整对比）。
 * SYS_WAIT 转发给 sys_wait(NULL)——ecall 路径的参数只有 a0/a1,SYS_WAIT
 * 语义上不需要输入参数,本 Lab 选择不提供"回传 exit_code 给用户态"这
 * 条路径,只暴露返回值。
 *
 * 提示：
 * int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1)
 * {
 *     switch (num) {
 *     case SYS_WRITE:
 *         return sys_write((const char *)a0, a1);
 *     case SYS_EXIT:
 *         sys_exit_proc((int64_t)a0);
 *         return 0;
 *     case SYS_FORK:
 *         return sys_fork();
 *     case SYS_EXEC:
 *         return sys_exec();
 *     case SYS_WAIT:
 *         return sys_wait(NULL);
 *     default:
 *         kprintf("syscall_dispatch: unknown syscall number %lu\n", num);
 *         return -1;
 *     }
 * }
 */

/* TODO 5：实现 ecall_handler(uint64_t *frame)（跟 Lab6 完全一样)。
 *
 * 从 supervisor_trap_entry 保存好的栈帧（trap_entry.S 里 sd 顺序
 * ra/t0-t6/a0-a7 这 16 个字段,偏移 0-120,128 字节,Lab7 原样未变)里
 * 取出 a7/a0/a1,对应 x86_64 版本 syscall_entry 汇编里手动搬移
 * rax/rdi/rsi 到 syscall_dispatch 参数寄存器这一步——riscv64 这边的
 * 等价动作发生在 C 代码里而不是汇编里,因为 supervisor_trap_handler()
 * 需要保持"零特权参数、自己读 CSR 决定分发"这个 Lab4/5 就定下的整体
 * 结构（page fault/定时器两条路径完全不需要动这个栈帧),但 ecall
 * 分支确实需要拿到原始寄存器值,不能只靠 CSR——syscall 号/参数是通过
 * 通用寄存器传的,不是 CSR。
 *
 * frame[8]/frame[9]/frame[15] 分别对应 trap_entry.S 里
 * `sd a0, 64(sp)`/`sd a1, 72(sp)`/`sd a7, 120(sp)`（字节偏移÷8=
 * uint64_t 数组下标),偏移量必须跟那边保持同步——这是本 Lab 又一处
 * "没有单一数据源、需要人肉对齐"的手工契约,README 会一并指出,跟
 * USER_PROG_VADDR 手写字面量是同一类。
 *
 * 返回值写回 frame[8]（对应 a0 的槽位)——trap_entry.S 的 epilogue会
 * 把这个槽位 ld 回真正的 a0 寄存器,再 sret 回用户态,用户态代码从
 * a0 读到的就是这个返回值,对应 syscall.h 头部注释、user_prog.S 里
 * "结果回 a0"这条约定。
 *
 * 提示：
 * static void ecall_handler(uint64_t *frame)
 * {
 *     uint64_t num = frame[15]; // a7
 *     uint64_t a0  = frame[8];  // a0
 *     uint64_t a1  = frame[9];  // a1
 *
 *     int64_t ret = syscall_dispatch(num, a0, a1);
 *
 *     frame[8] = (uint64_t)ret;
 * }
 */

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

    /* TODO 6：处理 scause == SCAUSE_ECALL_FROM_U 的分支（跟 Lab6 的
     * TODO 5 完全一样）。
     *
     * 为什么：先 write_sepc(read_sepc() + 4) 再分发——跳过 ecall 指令
     * 本身,不然 sret 会回到刚才那条 ecall 再执行一次,见本文件模块顶部
     * 注释里对 sepc 这一段的详细解释。+4 必须在 ecall_handler 分发之前
     * 做（虽然本 Lab 的系统调用都不依赖 sepc,提前做是更"正确"的顺序
     * ——真实场景里如果某个系统调用处理函数自己需要读 sepc,读到的应该
     * 已经是"+4 之后"的值,不应该依赖分发顺序）。
     *
     * 本 Lab 新增的 sys_exec() 会再一次动 sepc,那是另一个问题,由
     * proc.c 里 write_sepc() 上方那段注释负责,跟这里的 +4 不冲突。
     *
     * 提示：
     * if (scause == SCAUSE_ECALL_FROM_U) {
     *     write_sepc(read_sepc() + 4);
     *     ecall_handler(frame);
     *     return;
     * }
     */

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
