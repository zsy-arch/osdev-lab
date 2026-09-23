/* Lab2 starter (x86_64)：内核 C 入口。
 *
 * 到这里为止 CPU 已经在长模式，栈已经设好（如果 boot.S 的 TODO 都填对了）。
 * 这个函数要做两件事：验证 BSS 真的被清零了，然后触发一次 panic() 来验证
 * 诊断路径能正常工作。
 */
#include "console.h"
#include "panic.h"

void kernel_main(void)
{
    /* TODO 1：打印一行 "Hello OS from x86_64 (Lab2: kernel entry)"，
     * 末尾带换行（console_puts_line 会自动加）。
     * 必须和 tests/expect-x86_64.txt 第一行逐字匹配。
     */


    /* TODO 2：验证 BSS 真的被清零了——在函数开头声明一个没有显式初始化的
     * static int（比如叫 untouched_bss_counter），然后用 kprintf 打印它的值。
     *
     * C 语言规定"没有显式初始化的静态/全局变量的初值是 0"，这个保证在
     * 裸机环境下必须由内核自己的代码去满足：如果你 boot.S 里的 BSS 清零
     * TODO 没填对，这里读出来的值大概率不会恰好是 0（.bss 在链接后的
     * 原始内容未定义，取决于加载器有没有顺手清过）。
     *
     * 格式必须和 tests/expect-x86_64.txt 第二行逐字匹配：
     *
     *   static int untouched_bss_counter;
     *   ...
     *   kprintf("untouched_bss_counter = %d (expect 0, proves boot.S zeroed .bss)\n",
     *           untouched_bss_counter);
     */


    /* TODO 3：调用 panic() 宏，消息用
     * "Lab2 checkpoint: intentional panic to verify file/line reporting"。
     *
     * 这不是在制造一个"意外错误"——故意触发一次 panic 是本 Lab 验证
     * panic() 诊断路径（文件名+行号+消息）本身能正常工作的方式，后续
     * Lab 遇到真正不该发生的情况时会调用同一个宏。
     *
     * 提示：panic("...");
     */
}
