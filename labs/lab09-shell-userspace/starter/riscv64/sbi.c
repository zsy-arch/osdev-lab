/* Lab5 riscv64：SBI ecall 的实现——见 sbi.h 顶部注释了解为什么需要
 * 这一层。 */
#include "types.h"
#include "sbi.h"

/* TIME extension 的 EID，ASCII "TIME" 的四个字符按小端拼成一个 32 位
 * 值：'T'=0x54, 'I'=0x49, 'M'=0x4D, 'E'=0x45，从低字节到高字节排列
 * (小端)就是 0x54494D45——RISC-V SBI 规范里所有标准 extension 的 EID
 * 都是这样从一个便于记忆的 4 字符名拼出来的，不是随便挑的数字。 */
#define SBI_EID_TIME  0x54494D45ull
#define SBI_FID_SET_TIMER 0ull

/* 通用的 SBI ecall 包装——本 Lab 只用得到一个 extension/function，
 * 但仍然写成通用形式（而不是把 ecall 硬编码在 sbi_set_timer 里），
 * 因为"a7=EID, a6=FID, a0-a5=参数, ecall"这套约定跟具体调的是哪个
 * extension 无关，是 SBI 二进制约定本身规定的形状，写成通用函数体现
 * 出"这是约定，不是这一个函数的特例"。返回值 a0（错误码）本 Lab 不
 * 检查——sbi_set_timer 在 QEMU default OpenSBI 固件下没有失败的
 * 场景（不会传非法参数），检查了也没有除 panic 外的处理策略，教学
 * 上暂不引入。 */
static inline int64_t sbi_call(uint64_t eid, uint64_t fid, uint64_t arg0)
{
    /* GCC/Clang 内联汇编里，普通命名成 a6/a7 的 C 变量*不会*因为名字
     * 恰好撞上寄存器名就被分配到那个物理寄存器——之前一版这里就是
     * 这么写的（`register uint64_t a6 = fid;`），实测编译出来的代码
     * 根本没有任何 `mv a7,...`/`mv a6,...`，等价于从来没把 eid/fid
     * 传给 ecall，ecall 执行时 a6/a7 里是随便什么陈旧值——必须用
     * `register ... __asm__("a7")` 这种显式寄存器变量声明（GCC
     * "Specifying Registers for Local Variables"），才能真正强制
     * 变量落在指定的物理寄存器里，让 ecall 前这些值确实就位。 */
    register uint64_t a0 __asm__("a0") = arg0;
    register uint64_t a6 __asm__("a6") = fid;
    register uint64_t a7 __asm__("a7") = eid;

    __asm__ volatile(
        "ecall"
        : "+r"(a0)
        : "r"(a6), "r"(a7)
        : "memory");

    return (int64_t)a0;
}

void sbi_set_timer(uint64_t stime_value)
{
    sbi_call(SBI_EID_TIME, SBI_FID_SET_TIMER, stime_value);
}
