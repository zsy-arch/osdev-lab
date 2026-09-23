/* Lab5 riscv64：SBI ecall 的实现——见 sbi.h 顶部注释了解为什么需要
 * 这一层。 */
#include "types.h"
#include "sbi.h"

/* 已经写好，不是 TODO：EID/FID 这两个常量是 RISC-V SBI 规范定死的值，
 * 不是需要你推导的部分。TIME extension 的 EID，ASCII "TIME" 的四个
 * 字符按小端拼成一个 32 位值：'T'=0x54, 'I'=0x49, 'M'=0x4D, 'E'=0x45，
 * 从低字节到高字节排列(小端)就是 0x54494D45——RISC-V SBI 规范里所有
 * 标准 extension 的 EID 都是这样从一个便于记忆的 4 字符名拼出来的，
 * 不是随便挑的数字。 */
#define SBI_EID_TIME  0x54494D45ull
#define SBI_FID_SET_TIMER 0ull

/* TODO 1：实现 sbi_call() —— 把 eid/fid/arg0 分别放进 a7/a6/a0，执行
 * ecall，返回 a0（错误码）。
 *
 * 关键坑：C 变量普通声明成 a6/a7 这样的名字*不会*因为名字恰好撞上
 * 寄存器名就被分配到那个物理寄存器——如果你写：
 *
 *   register uint64_t a6 = fid;
 *   register uint64_t a7 = eid;
 *   __asm__ volatile("ecall" : "+r"(a0) : "r"(a6), "r"(a7) : "memory");
 *
 * 这看起来"应该"能工作（变量名就叫 a6/a7），但 GCC 的 register 关键字
 * 在这里只是一个（现代编译器基本忽略的）优化建议，不是寄存器绑定——
 * 实测编译出来的代码根本没有任何 `mv a7,...`/`mv a6,...`，等价于从来
 * 没把 eid/fid 传给 ecall，ecall 执行时 a6/a7 里是随便什么陈旧值。
 *
 * 正确做法：必须用 GCC "Specifying Registers for Local Variables"
 * 里的显式寄存器变量语法——`register 类型 变量名 __asm__("寄存器名")`，
 * 这才能真正强制变量落在指定的物理寄存器里，让 ecall 前这些值确实
 * 就位。
 *
 * 提示：
 * static inline int64_t sbi_call(uint64_t eid, uint64_t fid, uint64_t arg0)
 * {
 *     register uint64_t a0 __asm__("a0") = arg0;
 *     register uint64_t a6 __asm__("a6") = fid;
 *     register uint64_t a7 __asm__("a7") = eid;
 *
 *     __asm__ volatile(
 *         "ecall"
 *         : "+r"(a0)
 *         : "r"(a6), "r"(a7)
 *         : "memory");
 *
 *     return (int64_t)a0;
 * }
 */

/* TODO 2：实现 sbi_set_timer() —— 调 sbi_call()，EID=SBI_EID_TIME，
 * FID=SBI_FID_SET_TIMER，参数是目标时间值。返回值（错误码）本 Lab
 * 不检查——QEMU default OpenSBI 固件下没有失败的场景（不会传非法
 * 参数），检查了也没有除 panic 外的处理策略，教学上暂不引入。
 *
 * 提示：
 * void sbi_set_timer(uint64_t stime_value)
 * {
 *     sbi_call(SBI_EID_TIME, SBI_FID_SET_TIMER, stime_value);
 * }
 */
