/* 串口输出部分与 Lab1/Lab2 完全相同，见
 * labs/lab01-boot-hello/solution/x86_64/console_putc.c 的注释。
 *
 * Lab9 新增 console_getc()：本课程第一次从外界*读*字节。到 Lab8 为止串口
 * 一直是单向的（内核说，人听），而 shell 需要反过来——这是"交互"这件事
 * 在本课程里的起点。 */
#include "console.h"

#define COM1_BASE     0x3F8
#define COM1_LSR      (COM1_BASE + 5)
#define LSR_THR_EMPTY 0x20

/* LSR 的第 0 位：Data Ready。置 1 表示接收缓冲区里有一个字节等着被读走。
 *
 * 跟 LSR_THR_EMPTY（第 5 位，发送保持寄存器空）是同一个寄存器的两个不同
 * 位，一个管收一个管发。16550 的 LSR 就是这么设计的：一次 inb 拿到发送和
 * 接收两边的全部状态。 */
#define LSR_DATA_READY 0x01

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

/* Lab9：非阻塞读一个字节。没有输入返回 -1。
 *
 * 注意这里*没有*循环——console_putc 的那个 while 是等硬件把上一个字节发
 * 完（几微秒，必然结束），而这里要等的是人按键（可能永远不来）。在内核最
 * 底层的这个函数里死等，会把整个系统卡住：本 Lab 单核，而且系统调用执行
 * 期间中断是关掉的，定时器打不进来，调度器没有机会运行。所以"等"这件事
 * 必须留给上层——上层至少能在等的间隙 yield() 出去让别的进程跑。
 *
 * 强制转成 uint8_t 再赋给 int，是为了让返回值一定落在 [0, 255]：如果直接
 * 让 char（在 x86_64 上是有符号的）参与转换，0x80~0xFF 的字节会变成负数,
 * 跟 -1 这个"无输入"哨兵混在一起。本 Lab 的输入都是 ASCII，撞不上，但这
 * 种"输入恰好是高位字节时才出错"的 bug 是最难查的一类，不如从一开始就
 * 写对。 */
int console_getc(void)
{
    if ((inb(COM1_LSR) & LSR_DATA_READY) == 0) {
        return -1;
    }
    return (int)(uint8_t)inb(COM1_BASE);
}
