/* Lab2 starter (riscv64)：内核 C 入口。
 *
 * riscv64 到这里不需要任何模式切换，一直都是完整的 64 位 S 模式执行
 * 环境，栈已经设好（如果 boot.S 的 TODO 都填对了）。这个函数要做两件
 * 事：验证 BSS 真的被清零了，然后触发一次 panic() 来验证诊断路径。
 */
#include "console.h"
#include "panic.h"

void kernel_main(void)
{
    /* TODO 1：打印一行 "Hello OS from riscv64 (Lab2: kernel entry)"，
     * 末尾带换行（console_puts_line 会自动加）。
     * 必须和 tests/expect-riscv64.txt 第一行逐字匹配。
     */


    /* TODO 2：验证 BSS 真的被清零了——在函数开头声明一个没有显式初始化的
     * static int（比如叫 untouched_bss_counter），然后用 kprintf 打印它的值。
     * 原理同 x86_64 版本，见那边的注释。
     *
     * 格式必须和 tests/expect-riscv64.txt 第二行逐字匹配：
     *
     *   static int untouched_bss_counter;
     *   ...
     *   kprintf("untouched_bss_counter = %d (expect 0, proves boot.S zeroed .bss)\n",
     *           untouched_bss_counter);
     */


    /* TODO 3：调用 panic() 宏，消息用
     * "Lab2 checkpoint: intentional panic to verify file/line reporting"。
     *
     * 提示：panic("...");
     */
}
