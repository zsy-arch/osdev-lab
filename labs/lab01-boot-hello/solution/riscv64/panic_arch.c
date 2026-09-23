/* riscv64 的 panic_halt()：死循环执行 wfi。
 *
 * Lab1 阶段还没有配置 sstatus.SIE 之类的中断使能位（默认是关闭的），
 * 所以这里不需要像 x86_64 那样专门加一条"关中断"指令——中断本来就没开。
 * wfi 让 hart 进入低功耗等待，效果上等同于永久停机。
 */

void panic_halt(void)
{
    for (;;) {
        __asm__ volatile ("wfi");
    }
}
