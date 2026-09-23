/* riscv64 的 console_putc()：走 UART16550，MMIO 基址 0x10000000。
 *
 * QEMU virt 机器把一个 16550 兼容 UART 映射在这个固定地址（-machine virt
 * 的硬编码布局，可以在 QEMU 源码 hw/riscv/virt.c 里找到,也会出现在 Lab3
 * 要解析的 DTB 里）。x86_64 那边是"用专门的 in/out 指令走独立的 I/O
 * 地址空间"，这里是"就是普通内存地址,用普通的 load/store 指令读写"——
 * 这是 x86 和绝大多数其它架构（包括 riscv、arm）在 I/O 模型上的根本差异：
 * 前者有独立的端口地址空间，后者只有内存地址空间,设备寄存器混在里面
 * (Memory-Mapped I/O)。
 */
#include "console.h"

#define UART_BASE       ((volatile uint8_t *)0x10000000UL)
#define UART_THR        (UART_BASE + 0)   /* Transmitter Holding Register，写入即发送 */
#define UART_LSR        (UART_BASE + 5)   /* Line Status Register */
#define LSR_THR_EMPTY   0x20

void console_putc(char c)
{
    while ((*UART_LSR & LSR_THR_EMPTY) == 0) {
    }
    *UART_THR = (uint8_t)c;
}
