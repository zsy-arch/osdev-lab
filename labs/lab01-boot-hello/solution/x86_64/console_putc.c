/* x86_64 的 console_putc()：走 COM1 串口（I/O 端口 0x3F8）。
 *
 * QEMU 默认把 -serial stdio/-serial file:... 接到 COM1，这也是为什么这个
 * 地址不需要任何配置就能work——它是 x86 PC 的历史标准端口，不是 QEMU 发明的。
 * 0x3F8+5 是 Line Status Register，bit 5（Transmitter Holding Register
 * Empty）为 1 时才能写下一个字节，否则会覆盖还没发出去的数据。
 */
#include "console.h"

#define COM1_BASE     0x3F8
#define COM1_LSR      (COM1_BASE + 5)
#define LSR_THR_EMPTY 0x20

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

void console_putc(char c)
{
    while ((inb(COM1_LSR) & LSR_THR_EMPTY) == 0) {
    }
    outb(COM1_BASE, (uint8_t)c);
}
