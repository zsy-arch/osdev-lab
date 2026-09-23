/* Lab5 riscv64：在 Lab4"只识别三种 page fault 异常"的基础上，加上
 * 第二类 scause——监督态时钟中断（Supervisor Timer Interrupt）。
 * scause 的形状、page fault 三个 code 的处理完全照抄 Lab4，Lab5 新增
 * 的只是"scause 最高位=1 时怎么分发"这一段，之前这种情况一律 panic
 * （Lab4 的 ROADMAP 范围内没有任何合法中断源）。
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "sbi.h"

/* 已经写好，不是 TODO：这几个常量跟 Lab4 完全一样，Lab5 没有改动
 * page fault 这三个 scause code 的定义。 */
#define SCAUSE_INTERRUPT_BIT   (1ull << 63)
#define SCAUSE_INSN_PAGE_FAULT 12ull
#define SCAUSE_LOAD_PAGE_FAULT 13ull
#define SCAUSE_STORE_PAGE_FAULT 15ull

/* TODO 1：确定 SCAUSE_INT_SUPERVISOR_TIMER 该写多少。
 *
 * 提示：riscv-privileged 规范 "Machine Cause Register (mcause)" 表格
 * 里，S-mode 的中断 code：1=Supervisor Software Interrupt，
 * 5=Supervisor Timer Interrupt，9=Supervisor External Interrupt。
 * 本 Lab 只有定时器这一个中断源被使能（sie 只设了 STIE 那一位，见
 * 下面 timer_enable()），其它两种即使 scause 编码上存在，硬件也不会
 * 实际产生。
 *
 * #define SCAUSE_INT_SUPERVISOR_TIMER 5ull
 */

/* 目标频率跟 x86_64 版本一致（TIMER_HZ=100，pit.c 里的同名常量），
 * 两边在"每秒汇报一次"这个可观察行为上保持对等，方便对照表述——但
 * riscv64 这边没有硬件自动周期性重触发的机制（不像 x86_64 PIT mode 2
 * 那样设一次 reload value 之后自己周期性触发），每次处理完这次中断都
 * 要显式调用 sbi_set_timer() 预约下一次，这正是 Lab5 riscv64 侧的
 * 核心新知识点。 */
#define TIMER_HZ 100ull

/* 已经写好，不是 TODO：读 stval/scause 的实现跟 Lab4 完全一样。 */
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

/* TODO 2：实现 read_time() —— 读 riscv 定义的非特权级只读 CSR
 * "time"，映射自 CLINT 的 mtime。
 *
 * 提示：即使 mtime 本身是 M-mode 的 MMIO 寄存器，S-mode/U-mode 都能
 * 通过 time 这个 CSR 直接读到当前时间，不需要走 SBI（读时间不涉及
 * "编程未来触发一次中断"这种需要 M-mode 代劳的操作，纯读不算特权
 * 操作，riscv-privileged 规范 "Counters" 一节）。
 *
 * static uint64_t read_time(void)
 * {
 *     uint64_t value;
 *     __asm__ volatile("csrr %0, time" : "=r"(value));
 *     return value;
 * }
 */

volatile uint64_t timer_ticks = 0;

/* TODO 3：实现 timer_interrupt_handler() —— 自增 timer_ticks，并预约
 * 下一次定时器中断。
 *
 * 关键差异（riscv64 版 PIT 相比 x86_64 8254 mode 2 的核心区别）：
 * 硬件不会自动周期性重触发，每次处理完这一次中断，必须显式请求
 * "下一次"，否则这就是最后一次时钟中断——时间基准要用 read_time()
 * *现在*的值，而不是"上一次预约的值+间隔"，避免处理这次中断本身
 * 耗费的时间被计入下一个间隔，产生累积漂移（教学场景下这个漂移量
 * 微不足道，但用"现在的时间"作为基准是更正确的写法，不需要额外代码
 * 就能避免这个问题，没有理由不这样做）。
 *
 * 提示：QEMU virt 平台 mtime 频率固定 10MHz（QEMU "riscv,virt" 平台
 * 的 CLINT 实现约定，见 QEMU 源码 hw/intc/riscv_aclint.c），
 * interval = 10000000ull / TIMER_HZ，即每 tick 间隔 100000 个 mtime
 * 计数。
 *
 * static void timer_interrupt_handler(void)
 * {
 *     timer_ticks++;
 *
 *     uint64_t interval = 10000000ull / TIMER_HZ;
 *     sbi_set_timer(read_time() + interval);
 * }
 */

/* TODO 4：在 supervisor_trap_handler() 里加上中断分发这一段——放在
 * Lab4 原有的 page fault 判断*之前*：先看 scause 最高位是不是 1
 * （区分"这是中断"还是"这是异常"），是中断的话再看低位的 code 是不是
 * Supervisor Timer Interrupt。
 *
 * 提示：
 * void supervisor_trap_handler(void)
 * {
 *     uint64_t scause = read_scause();
 *
 *     if (scause & SCAUSE_INTERRUPT_BIT) {
 *         uint64_t code = scause & ~SCAUSE_INTERRUPT_BIT;
 *         if (code == SCAUSE_INT_SUPERVISOR_TIMER) {
 *             timer_interrupt_handler();
 *             return;
 *         }
 *         kprintf("supervisor_trap_handler: unexpected interrupt code=%lu\n", code);
 *         panic("supervisor_trap_handler: unhandled interrupt (Lab5 only handles the timer)");
 *     }
 *
 *     // 下面这一段跟 Lab4 完全一样，原样保留：
 *     if (scause != SCAUSE_INSN_PAGE_FAULT &&
 *         scause != SCAUSE_LOAD_PAGE_FAULT &&
 *         scause != SCAUSE_STORE_PAGE_FAULT) {
 *         kprintf("supervisor_trap_handler: unexpected scause=%lu\n", scause);
 *         panic("supervisor_trap_handler: unrecognized exception (Lab4/5 only handle page faults + timer)");
 *     }
 *
 *     uintptr_t fault_addr = read_stval();
 *     const char *reason = (scause == SCAUSE_INSN_PAGE_FAULT) ? "exec" :
 *                          (scause == SCAUSE_LOAD_PAGE_FAULT) ? "read" : "write";
 *
 *     kprintf("page fault: addr=%p reason=%s (scause=%lu)\n",
 *             (void *)fault_addr, reason, scause);
 *
 *     panic("supervisor_trap_handler: unrecoverable page fault (Lab4/5 do not implement fault recovery)");
 * }
 */

extern void supervisor_trap_entry(void);

/* 已经写好，不是 TODO：trap_init() 跟 Lab4 完全一样——Direct 模式，
 * 见 Lab4 版本这里的完整注释：Vectored 模式在本课程只有"page fault +
 * 一个中断源"的规模下不划算，多一种分发机制的教学收益不足以证明额外
 * 复杂度。ROADMAP 把它列成"挑战任务"，不是本 Lab 主线要求。 */
void trap_init(void)
{
    uintptr_t entry = (uintptr_t)supervisor_trap_entry;
    __asm__ volatile("csrw stvec, %0" : : "r"(entry));
}

/* TODO 5：实现 timer_enable() —— 使能监督态时钟中断这条路径上的两个
 * 开关（riscv-privileged 规范 "Supervisor Interrupt Registers (sip
 * and sie)"、"sstatus" 两节）：
 *   sie（Supervisor Interrupt Enable）的 STIE 位（bit 5）：这一类中断
 *     源本身的开关，类似 x86_64 PIC 的 IMR（Interrupt Mask Register）
 *     ——每一种中断源各自一个位，只开定时器这一位，不动其它位。
 *   sstatus 的 SIE 位（bit 1）：全局总闸，类似 x86_64 RFLAGS 的 IF
 *     位——不管 sie 里哪些具体位开了，SIE=0 时 CPU 完全不响应任何
 *     可屏蔽中断（riscv 里的 exception，即本 Lab 的 page fault，不受
 *     这个位影响，跟 x86_64 IF 不影响异常递送是同一个道理）。
 * 两个都要开，缺一个都不会真正收到中断——这跟 x86_64 需要"IDT entry
 * 填了+PIC 取消屏蔽+sti"三步才能收到 PIT 中断是同一个"多层开关"结构，
 * 只是 riscv64 是两层（sie 位+sstatus.SIE）而不是三层。
 *
 * 提示：先 csrr 读出当前值，用 |= 只置需要的那一位，再 csrw 写回去
 * （不要整个寄存器直接赋值覆盖，会误清掉其它本不该动的位）。
 *
 * void timer_enable(void)
 * {
 *     uint64_t sie;
 *     __asm__ volatile("csrr %0, sie" : "=r"(sie));
 *     sie |= (1ull << 5);
 *     __asm__ volatile("csrw sie, %0" : : "r"(sie));
 *
 *     uint64_t sstatus;
 *     __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
 *     sstatus |= (1ull << 1);
 *     __asm__ volatile("csrw sstatus, %0" : : "r"(sstatus));
 * }
 */
