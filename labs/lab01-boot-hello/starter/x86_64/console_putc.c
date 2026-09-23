/* Lab1 starter (x86_64)：实现 console.h 声明的 console_putc()。
 *
 * x86 把外设当成独立的 I/O 地址空间，要用 in/out 指令访问，不能像
 * 普通内存一样直接读写。COM1 串口的基地址是 0x3F8，本课程只用到
 * 其中两个寄存器：数据寄存器（发送字节写这里）和 Line Status
 * Register（LSR，偏移 +5，用来确认"上一个字节发完了没有"）。
 */
#include "console.h"

#define COM1_BASE     0x3F8
#define COM1_LSR      (COM1_BASE + 5)
#define LSR_THR_EMPTY 0x20

/* TODO 1：实现 inb —— 从 port 读一个字节。
 * 用内联汇编包装 `in` 指令：
 *   __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
 * "Nd" 约束允许端口号用立即数或 %dx 寄存器传递，"=a" 表示返回值走 %al。
 */
static inline uint8_t inb(uint16_t port)
{
    (void)port;
    return 0; /* 占位，替换成真正的 in 指令实现 */
}

/* TODO 2：实现 outb —— 往 port 写一个字节 v。
 * 对应的 `out` 指令：
 *   __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
 */
static inline void outb(uint16_t port, uint8_t v)
{
    (void)port;
    (void)v;
}

void console_putc(char c)
{
    /* TODO 3：轮询等待发送就绪，再把字节写出去。
     *
     * LSR 的 bit 5（值 0x20）是 Transmitter Holding Register Empty：
     * 这一位为 1 才说明"上一个字节已经从发送寄存器移走"，可以安全写
     * 下一个字节。写之前不检查这一位，快速连续发送时会丢字节。
     *
     * 伪代码：
     *   while ((inb(COM1_LSR) & LSR_THR_EMPTY) == 0) { }
     *   outb(COM1_BASE, (uint8_t)c);
     */
    (void)c;
}
