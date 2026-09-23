/* Lab1 starter (x86_64)：内核 C 入口。
 *
 * 这是 boot.S 里 _start64 call 的那个函数——到这里为止，CPU 已经
 * 处于 64 位长模式，栈已经设好，你可以放心写普通的 C 代码了。
 */
#include "console.h"

void kernel_main(void)
{
    /* TODO：打印一行 "Hello OS from x86_64"，末尾带换行。
     * console.h 提供的 console_puts_line() 正是干这个的，
     * 不需要自己拼换行符。
     *
     * 这一行字符串必须和 tests/expect-x86_64.txt 里的内容逐字匹配，
     * 自动化测试才能通过（多一个空格、少一个字母都会判定失败）。
     */
}
