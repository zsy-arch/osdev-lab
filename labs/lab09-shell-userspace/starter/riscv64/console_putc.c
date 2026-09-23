/* 串口输出部分与 Lab1/Lab2 完全相同，见
 * labs/lab01-boot-hello/solution/riscv64/console_putc.c 的注释。
 *
 * Lab9 新增 console_getc()，说明见 x86_64 那一份——两边是同一颗 16550
 * 芯片的同一组寄存器，差别只在"怎么访问它"：x86_64 用端口 I/O 指令
 * （in/out 是独立的地址空间），riscv64 直接当内存读写（MMIO）。RISC-V
 * 根本没有端口 I/O 这个概念，所有外设都在物理地址空间里。
 *
 * 这是本课程里"同一个硬件、两种访问方式"最干净的一个例子：下面两个函数
 * 跟 x86_64 那两个逐行对应，LSR 的位定义一模一样，只有取值的语法不同。 */
#include "console.h"

#define UART_BASE     0x10000000UL
#define UART_RBR      (*(volatile uint8_t *)(UART_BASE + 0))
#define UART_THR      (*(volatile uint8_t *)(UART_BASE + 0))
#define UART_LSR      (*(volatile uint8_t *)(UART_BASE + 5))
#define LSR_THR_EMPTY 0x20

/* LSR 第 0 位：Data Ready，接收缓冲区里有字节可读。见 x86_64 那一份的注释。 */
#define LSR_DATA_READY 0x01

void console_putc(char c)
{
    while ((UART_LSR & LSR_THR_EMPTY) == 0) {
    }
    UART_THR = (uint8_t)c;
}

/* Lab9：非阻塞读一个字节。没有输入返回 -1。
 *
 * 逻辑与 x86_64 版本完全相同，"为什么不在这里死等"那段理由（单核 + 系统
 * 调用期间关中断，在最底层阻塞会卡死整个系统）同样适用——riscv64 上"关
 * 中断"体现为硬件在陷入 S 态时清掉 sstatus.SIE。
 *
 * UART_RBR 和 UART_THR 是同一个地址（UART_BASE + 0），读是接收缓冲区、写是
 * 发送保持寄存器。16550 用这种"同地址读写不同寄存器"的方式省地址空间,
 * 上面故意写成两个名字，是为了让代码读起来知道当下在跟哪一个说话。 */
int console_getc(void)
{
    if ((UART_LSR & LSR_DATA_READY) == 0) {
        return -1;
    }
    return (int)(uint8_t)UART_RBR;
}
