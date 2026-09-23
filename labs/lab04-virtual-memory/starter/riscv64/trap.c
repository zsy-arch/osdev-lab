/* Lab4 starter (riscv64)：Supervisor 态异常处理,专门处理页故障。
 *
 * riscv64 的异常机制和 x86_64 的 IDT 完全不同：没有一张"按向量号
 * 索引的表",只有一个寄存器 stvec,指向*唯一*一个异常入口点——不管
 * 是什么原因触发的异常,硬件都跳到同一个地方,再由软件自己去读
 * scause 寄存器判断"这次是因为什么"。
 *
 * 这次异常的"原因"和"详情"分别放在两个 CSR（Control and Status
 * Register)里：
 *   scause  异常原因编号（我们只关心 page fault 的三个编号)
 *   stval   异常相关的附加信息（page fault 时,这里放的是触发故障的
 *           虚拟地址——riscv64 没有 x86_64 CR2 那种专门叫法,但作用
 *           完全一样)
 */
#include "types.h"
#include "console.h"
#include "panic.h"

/* scause 的最高位是"这是中断还是异常"的标志位,riscv64 把中断和
 * 异常统一编码进同一个寄存器,靠这一位区分。本 Lab 不处理中断,只
 * 处理异常,所以这一位必须是 0。已经写好,不是 TODO。 */
#define SCAUSE_INTERRUPT_BIT (1ull << 63)

/* 三个 page fault 的 scause 编号（RISC-V 特权架构手册规定)。注意
 * 编号 14 被故意跳过——手册里这个编号保留未用,不是我们漏写了。已经
 * 写好,不是 TODO。 */
#define SCAUSE_INSN_PAGE_FAULT  12ull
#define SCAUSE_LOAD_PAGE_FAULT  13ull
#define SCAUSE_STORE_PAGE_FAULT 15ull

/* 读 stval/scause 这两个 CSR。已经写好,不是 TODO——纯粹是内联汇编
 * 的样板代码,和"理解页故障"这个教学目标没关系。 */
static inline uint64_t read_stval(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, stval" : "=r"(value));
    return value;
}

static inline uint64_t read_scause(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, scause" : "=r"(value));
    return value;
}

/* trap_entry.S 里定义,汇编入口在保存好寄存器之后跳过来这里。 */
extern void supervisor_trap_entry(void);

/* TODO 1：supervisor_trap_handler() —— 分类这次异常,打印诊断信息。
 *
 * 和 x86_64 的 page_fault_handler() 教学目标完全一样,只是信息来源
 * 从"硬件塞进异常帧的错误码+CR2"换成了"两个 CSR"。步骤：
 *
 *   1. 读 scause。如果最高位（SCAUSE_INTERRUPT_BIT)被设了,说明这是
 *      中断不是异常——本 Lab 没有开任何中断源,理论上不该发生,直接
 *      panic 说明情况。
 *   2. 把 scause 和 SCAUSE_INTERRUPT_BIT 相与清掉最高位后的值,与三个
 *      已知的 page fault 编号比较。如果一个都不匹配,说明触发了本
 *      Lab 没预料到的异常类型（比如非法指令、除零……),kprintf 打印
 *      出 scause 原始值再 panic,方便调试。
 *   3. 确认是 page fault 之后,读 stval 拿到触发故障的虚拟地址。
 *   4. 根据具体是哪一种 page fault,用三元表达式链算出一个描述性的
 *      字符串（"exec"/"read"/"write"),和 x86_64 那边"根据错误码
 *      bit 1 判断 read/write"是同一个教学点,只是 riscv64 这边编号
 *      本身就直接区分了三种,不需要再拆位。
 *   5. kprintf 打印一行完整诊断（scause 原始值、fault_addr、reason
 *      字符串),再 panic。本 Lab 到此为止不恢复执行,后续 Lab 才会
 *      实现按需调页 (demand paging)。
 *
 * 提示：
 *   void supervisor_trap_handler(void)
 *   {
 *       uint64_t scause = read_scause();
 *
 *       if (scause & SCAUSE_INTERRUPT_BIT) {
 *           panic("supervisor_trap_handler: unexpected interrupt (scause=0x%lx), interrupts are not enabled in this lab", scause);
 *       }
 *
 *       uint64_t code = scause & ~SCAUSE_INTERRUPT_BIT;
 *       if (code != SCAUSE_INSN_PAGE_FAULT &&
 *           code != SCAUSE_LOAD_PAGE_FAULT &&
 *           code != SCAUSE_STORE_PAGE_FAULT) {
 *           kprintf("supervisor_trap_handler: unrecognized exception, scause=0x%lx\n", scause);
 *           panic("supervisor_trap_handler: unhandled exception code %lu", code);
 *       }
 *
 *       uint64_t fault_addr = read_stval();
 *       const char *reason = (code == SCAUSE_INSN_PAGE_FAULT) ? "exec" :
 *                            (code == SCAUSE_LOAD_PAGE_FAULT) ? "read" : "write";
 *
 *       kprintf("page fault: %s access to unmapped/invalid address 0x%lx (scause=0x%lx)\n",
 *               reason, fault_addr, scause);
 *       panic("supervisor_trap_handler: unhandled page fault");
 *   }
 */


/* TODO 2：trap_init() —— 把 stvec 指向 supervisor_trap_entry。
 *
 * stvec 的低 2 位是模式位：0 表示 Direct 模式（所有异常/中断统一跳
 * 到 stvec 存的这一个地址,由软件自己分类——这正是我们在用的模式);
 * 1 表示 Vectored 模式（不同中断号跳到 stvec 为基址、按中断号计算
 * 出的不同偏移,需要配合一张跳转表,本 Lab 不使用)。
 *
 * 提示：
 *   void trap_init(void)
 *   {
 *       uint64_t entry = (uint64_t)supervisor_trap_entry;
 *       __asm__ volatile("csrw stvec, %0" : : "r"(entry) : "memory");
 *   }
 */
