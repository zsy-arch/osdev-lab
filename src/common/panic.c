/* panic 的架构无关部分：打印诊断信息。
 * 真正"让 CPU 停下来"的动作是架构相关的，声明在这里、实现在
 * src/<arch>/panic_arch.c，由链接阶段决定用哪一份。
 */
#include "console.h"
#include "panic.h"

void panic_halt(void); /* 架构专属实现 */

void kernel_panic(const char *file, int line, const char *msg)
{
    kprintf("\n*** KERNEL PANIC ***\n");
    kprintf("  at %s:%d\n", file, line);
    kprintf("  %s\n", msg);
    kprintf("System halted.\n");
    panic_halt();
    /* panic_halt() 不会返回；这里加一个不可达的死循环只是为了让
     * "noreturn" 的意图对编译器和读者都更明显，避免个别编译器在
     * panic_halt 没标 _Noreturn 时对后续代码做出错误的可达性假设。 */
    for (;;) {
    }
}
