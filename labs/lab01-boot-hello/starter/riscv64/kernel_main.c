/* Lab1 starter (riscv64)：内核 C 入口。
 *
 * 这是 boot.S 里 _start call 的那个函数——riscv64 到这里不需要
 * 任何模式切换，一直都是完整的 64 位 S 模式执行环境，栈已经设好，
 * 可以放心写普通的 C 代码。
 */
#include "console.h"

void kernel_main(void)
{
    /* TODO：打印一行 "Hello OS from riscv64"，末尾带换行。
     * console.h 提供的 console_puts_line() 正是干这个的。
     *
     * 这一行字符串必须和 tests/expect-riscv64.txt 里的内容逐字匹配，
     * 自动化测试才能通过。
     */
}
