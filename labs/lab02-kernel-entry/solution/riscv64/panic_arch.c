/* 与 Lab1 完全相同，见 labs/lab01-boot-hello/solution/riscv64/panic_arch.c 的注释。 */
void panic_halt(void)
{
    for (;;) {
        __asm__ volatile ("wfi");
    }
}
