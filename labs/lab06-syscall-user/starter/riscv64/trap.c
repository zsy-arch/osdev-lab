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

/* TODO 1：实现 read_sepc/write_sepc。
 *
 * sepc：trap 发生那一刻的 PC（riscv-privileged 规范"sepc"一节)——
 * ecall 场景下必须显式加 4（跳过 ecall 指令本身,4 字节,riscv64 不用
 * 压缩指令集时每条指令固定 4 字节),否则 sret 会回到*刚才那条 ecall*
 * 本身,再执行一次 ecall,陷入无限循环。这跟 x86_64 SYSCALL 的行为
 * 不同：SYSCALL 指令执行时硬件自动把 RCX 设成"SYSCALL 之后那条指令"
 * 的地址,riscv64 的 ecall/sepc 没有这个自动前进,软件必须自己处理
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

/* 已经写好，不是 TODO：trap_saved_user_sp——trap_entry.S 在
 * ecall-from-U-mode 分支里,换到 __stack_top 之前,把用户 sp 暂存到
 * 这里——对应 x86_64 版本的 syscall_saved_user_rsp,同一个问题的同一种
 * 解法（单核场景下用一个全局槽位就够,不需要 per-hart 存储)。这个符号
 * 只被 trap_entry.S 用汇编直接读写,trap.c 里的 C 代码从来不直接访问它
 * ——放在这个文件只是因为"全局变量需要一个定义它的编译单元"。 */
uint64_t trap_saved_user_sp;

volatile uint64_t timer_ticks = 0;

static void timer_interrupt_handler(void)
{
    timer_ticks++;

    uint64_t interval = 10000000ull / TIMER_HZ;
    sbi_set_timer(read_time() + interval);
}

/* TODO 2：实现 sys_write/sys_exit。
 *
 * 跟 x86_64 版本（../x86_64/trap.c 的同名函数)语义完全一致,包括
 * "用户合法地址在当前页表下对内核也是合法地址,本 Lab 用户页表和内核
 * 页表是同一份,不做地址校验"这个简化——见 x86_64 版本 sys_write 上方
 * 的完整注释,riscv64 侧共用同一个理由,不重复展开。
 *
 * 提示：
 * static int64_t sys_write(const char *user_ptr, uint64_t len)
 * {
 *     for (uint64_t i = 0; i < len; i++) {
 *         console_putc(user_ptr[i]);
 *     }
 *     return (int64_t)len;
 * }
 *
 * static void sys_exit(int64_t code)
 * {
 *     kprintf("user program exited with code %ld\n", code);
 * }
 */

/* TODO 3：实现 syscall_dispatch。
 *
 * 跟 x86_64 版本同名函数同一个签名/同一个分发逻辑——num/a0/a1 对应
 * riscv64 syscall 传参约定里的 a7（调用号)/a0/a1（前两个参数,
 * user_prog.S 已经按这个约定填好),对应 x86_64 版本的 rax/rdi/rsi。
 * 返回值约定：riscv64 侧沿用 a0 传返回值（跟 SBI 调用约定"a0=返回值"
 * 是同一个寄存器角色,而不是 x86_64 侧借用的 Linux syscall ABI
 * "rax 传返回值"惯例)。
 *
 * 提示：
 * int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1)
 * {
 *     switch (num) {
 *     case SYS_WRITE:
 *         return sys_write((const char *)a0, a1);
 *     case SYS_EXIT:
 *         sys_exit((int64_t)a0);
 *         return 0;
 *     default:
 *         kprintf("syscall_dispatch: unknown syscall number %lu\n", num);
 *         return -1;
 *     }
 * }
 */

/* TODO 4：实现 ecall_handler(uint64_t *frame)。
 *
 * 从 supervisor_trap_entry 保存好的栈帧（trap_entry.S 的 128 字节
 * 布局，sd 顺序 ra/t0-t6/a0-a7)里取出 a7/a0/a1,对应 x86_64 版本
 * syscall_entry 汇编里手动搬移 rax/rdi/rsi 到 syscall_dispatch 参数
 * 寄存器这一步——riscv64 这边的等价动作发生在 C 代码里而不是汇编里,
 * 因为 supervisor_trap_handler() 需要保持"零特权参数、自己读 CSR
 * 决定分发"这个 Lab4/5 就定下的整体结构（page fault/定时器两条路径
 * 完全不需要动这个栈帧),但 ecall 分支确实需要拿到原始寄存器值,
 * 不能只靠 CSR——syscall 号/参数是通过通用寄存器传的,不是 CSR。
 *
 * frame[8]/frame[9]/frame[15] 分别对应 trap_entry.S 里
 * `sd a0, 64(sp)`/`sd a1, 72(sp)`/`sd a7, 120(sp)`（字节偏移÷8=
 * uint64_t 数组下标),偏移量必须跟那边保持同步——这是本 Lab 又一处
 * "没有单一数据源、需要人肉对齐"的手工契约,跟 USER_PROG_VADDR 手写
 * 字面量是同一类。
 *
 * 返回值写回 frame[8]（对应 a0 的槽位)——trap_entry.S 的 epilogue 会
 * 把这个槽位 ld 回真正的 a0 寄存器,再 sret 回用户态,用户态代码从
 * a0 读到的就是这个返回值。
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

    /* TODO 5：处理 scause == SCAUSE_ECALL_FROM_U 的分支。
     *
     * 必须先 write_sepc(read_sepc() + 4)——跳过 ecall 指令本身,不然
     * sret 会回到刚才那条 ecall 再执行一次,见上面模块顶部注释里对
     * sepc 这一段的详细解释。必须在 ecall_handler 分发之前做（虽然
     * 本 Lab 两个系统调用都不依赖 sepc,提前做是更"正确"的顺序）。
     * 然后调用 ecall_handler(frame)，处理完 return。
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
