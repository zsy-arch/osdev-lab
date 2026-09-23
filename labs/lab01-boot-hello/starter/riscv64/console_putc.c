/* Lab1 starter (riscv64)：实现 console.h 声明的 console_putc()。
 *
 * riscv（以及大多数现代架构）把外设寄存器直接映射到内存地址空间的
 * 某一段，用普通的指针读写就能访问——这和 x86_64 用专门的 in/out
 * 指令访问独立 I/O 地址空间是两种不同的硬件设计哲学（对比
 * solution/x86_64/console_putc.c 或 starter/x86_64/console_putc.c）。
 *
 * QEMU virt 机器把 UART16550 固定映射在物理地址 0x10000000。
 */
#include "console.h"

#define UART_BASE       ((volatile uint8_t *)0x10000000UL)
#define UART_THR        (UART_BASE + 0)  /* Transmitter Holding Register：写这里发送字节 */
#define UART_LSR        (UART_BASE + 5)  /* Line Status Register */
#define LSR_THR_EMPTY   0x20

void console_putc(char c)
{
    /* TODO：轮询等待发送就绪，再把字符写进 UART_THR。
     *
     * 判断逻辑和 x86_64 版本完全一样（都是 LSR 的 bit 5），
     * 差异只在"怎么读写这个寄存器"：这里是对 volatile 指针的
     * 普通内存读写，不需要任何特殊指令。
     *
     * 伪代码：
     *   while ((*UART_LSR & LSR_THR_EMPTY) == 0) { }
     *   *UART_THR = (uint8_t)c;
     */
    (void)c;
}
