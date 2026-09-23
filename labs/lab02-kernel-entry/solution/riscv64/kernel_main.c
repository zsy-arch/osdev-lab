#include "console.h"
#include "panic.h"

/* 同 x86_64 版本：验证 boot.S 的 BSS 清零循环确实生效，见那边的注释。 */
static int untouched_bss_counter;

void kernel_main(void)
{
    console_puts_line("Hello OS from riscv64 (Lab2: kernel entry)");

    kprintf("untouched_bss_counter = %d (expect 0, proves boot.S zeroed .bss)\n",
            untouched_bss_counter);

    panic("Lab2 checkpoint: intentional panic to verify file/line reporting");
}
