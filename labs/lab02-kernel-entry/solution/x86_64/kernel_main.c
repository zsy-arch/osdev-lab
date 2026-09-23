#include "console.h"
#include "panic.h"

/* 落在 .bss 段的静态变量：C 语言规定"没有显式初始化的静态/全局变量的
 * 初值是 0"，这个保证在裸机环境下必须由内核自己的代码去满足——本 Lab
 * 的 boot.S 已经在跳进这个函数之前把 __bss_start..__bss_end 清成了 0，
 * 这里读出来打印，就是"验证 BSS 清零确实生效"的证据，不是走个形式。
 * 如果 boot.S 的清零逻辑漏了、或者链接脚本的 __bss_start/__bss_end
 * 算错了范围，这个值大概率不会恰好是 0（.bss 段在链接后的原始内容是
 * 未定义的，取决于加载器有没有顺手清过、以及内存里的历史残留）。 */
static int untouched_bss_counter;

void kernel_main(void)
{
    console_puts_line("Hello OS from x86_64 (Lab2: kernel entry)");

    kprintf("untouched_bss_counter = %d (expect 0, proves boot.S zeroed .bss)\n",
            untouched_bss_counter);

    /* 演示 panic()：打印调用点的文件名+行号+消息，然后架构专属的
     * panic_halt() 让 CPU 停下来。这不是一个"意外触发"的错误路径——
     * Lab2 的目标就是证明这条诊断路径本身是可用的，故意在这里主动调用
     * 一次，让自动化测试能验证输出格式，后续 Lab（从 Lab3 开始）真正
     * 遇到不该发生的情况时，会调用同一个 panic() 宏，行为和这里完全
     * 一致。 */
    panic("Lab2 checkpoint: intentional panic to verify file/line reporting");
}
