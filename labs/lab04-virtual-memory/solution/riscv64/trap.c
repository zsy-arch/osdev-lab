/* Lab4 riscv64：最窄的中断/异常基础设施——只为了接住三种 page fault
 * 原因（取指/读/写)，不是完整的 trap 分发框架。完整的 scause 分类/
 * 分发、时钟中断是 Lab5 的教学内容,这里提前搭好会让 Lab5 没有新东西
 * 可讲——跟 x86_64 版本 trap.c 开头注释是同一个边界，同一个已确认的
 * 设计决策。
 *
 * RISC-V Privileged Architecture 里 scause 的编码（Volume II,
 * "Machine Cause Register (mcause)"一节，S-mode 的 scause 布局相同）：
 *   bit 63（XLEN-1）: Interrupt——1 表示这是中断，0 表示是异常
 *   bits 0-62       : Exception Code / Interrupt Code
 *
 * 本 Lab 只关心三个"异常"（bit 63 = 0）的 code：
 *   12 : Instruction page fault（取指令时触发）
 *   13 : Load page fault（读数据时触发）
 *   15 : Store/AMO page fault（写数据/原子操作时触发）
 * 14 号（Reserved）在这三者中间跳过，不是笔误——RISC-V 规范就是这样
 * 编号的，这里特意写出来避免以后有人以为 13/15 之间少了一项。
 * 除此之外的 scause 值（其它异常、任何中断）本 Lab 的 stvec 处理函数
 * 一律 panic，不识别、不分发——这正是"最窄单向量"在 riscv64 这边的
 * 体现：x86_64 是"只填 IDT 第 14 项，其它 31 项留空转 #GP"，riscv64
 * 没有 IDT 这种按向量分发的硬件机制（trap 统一从 stvec 一个入口进来，
 * 软件自己读 scause 判断"这是什么"），所以"只处理这三种"必须在软件里
 * 显式用 if/panic 表达出来，不能靠硬件的"没填就报错"机制替我们做。
 */
#include "types.h"
#include "console.h"
#include "panic.h"

#define SCAUSE_INTERRUPT_BIT   (1ull << 63)
#define SCAUSE_INSN_PAGE_FAULT 12ull
#define SCAUSE_LOAD_PAGE_FAULT 13ull
#define SCAUSE_STORE_PAGE_FAULT 15ull

/* stval 在触发 page fault 时保存出错的虚拟地址（riscv-privileged
 * 规范 "Supervisor Trap Value (stval) Register"一节）——跟 x86_64
 * 的 CR2 是对应关系,但这里是普通 CSR,用 csrr 就能读,不需要额外的
 * 特殊指令。 */
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

/* supervisor_trap_entry（trap_entry.S）在汇编层面完成的事：保存所有
 * 会被下面调用打乱的通用寄存器,call 这个函数,返回后恢复寄存器再
 * sret（riscv 的"异常返回"指令,对应 x86_64 的 iretq)。跟 x86_64 版本
 * 的分工完全一样,细节见 trap_entry.S。
 *
 * 这个函数没有参数——不像 x86_64 的 #PF 有一个硬件压栈的 error code
 * 需要显式传参,riscv64 的所有异常信息（scause/stval/sepc)都是普通
 * CSR,C 函数体内随时可以自己读,不需要靠调用约定传进来。 */
void supervisor_trap_handler(void)
{
    uint64_t scause = read_scause();

    if (scause & SCAUSE_INTERRUPT_BIT) {
        panic("supervisor_trap_handler: unexpected interrupt (Lab4 only handles page faults)");
    }

    if (scause != SCAUSE_INSN_PAGE_FAULT &&
        scause != SCAUSE_LOAD_PAGE_FAULT &&
        scause != SCAUSE_STORE_PAGE_FAULT) {
        kprintf("supervisor_trap_handler: unexpected scause=%lu\n", scause);
        panic("supervisor_trap_handler: unrecognized exception (Lab4 only handles page faults)");
    }

    uintptr_t fault_addr = read_stval();
    const char *reason = (scause == SCAUSE_INSN_PAGE_FAULT) ? "exec" :
                          (scause == SCAUSE_LOAD_PAGE_FAULT) ? "read" : "write";

    kprintf("page fault: addr=%p reason=%s (scause=%lu)\n",
            (void *)fault_addr, reason, scause);

    /* 跟 x86_64 版本同一个策略：能识别、能报告,不修复。ROADMAP 只要求
     * "触发并正确处理一次缺页异常",按需分页之类"修复后 sret 回去重跑"
     * 的策略留给后面 Lab。 */
    panic("supervisor_trap_handler: unrecoverable page fault (Lab4 does not implement fault recovery)");
}

extern void supervisor_trap_entry(void);

void trap_init(void)
{
    /* stvec 的低 2 位是 MODE 字段,本 Lab 用 Direct 模式（值 0：所有
     * trap 不管 scause 是什么,一律跳到 stvec 存的这一个地址,分发逻辑
     * 全部在软件里做,即上面 supervisor_trap_handler 的 if 链)。riscv
     * 还定义了 Vectored 模式（值 1：不同 scause 跳到 BASE+4*cause 的
     * 不同地址,更接近 x86_64 IDT 那种硬件分发)),本 Lab 不用——多一种
     * 模式的教学收益不足以证明额外的复杂度,supervisor_trap_entry 本身
     * 只有一个入口,跟 Direct 模式天然匹配。 */
    uintptr_t entry = (uintptr_t)supervisor_trap_entry;
    __asm__ volatile("csrw stvec, %0" : : "r"(entry));
}
