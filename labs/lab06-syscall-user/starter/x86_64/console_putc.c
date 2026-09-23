/* 与 Lab1/Lab2 完全相同——串口输出不是 Lab3 的教学内容，原样带过来。
 * 见 labs/lab01-boot-hello/solution/x86_64/console_putc.c 的注释。 */
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
