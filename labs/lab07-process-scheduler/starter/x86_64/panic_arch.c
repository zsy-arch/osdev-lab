/* 与 Lab1/Lab2 完全相同，见 labs/lab01-boot-hello/solution/x86_64/panic_arch.c 的注释。 */
void panic_halt(void)
{
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
