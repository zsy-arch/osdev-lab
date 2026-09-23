/* 与 Lab1/Lab2 完全相同，见 labs/lab01-boot-hello/solution/riscv64/console_putc.c 的注释。 */
#include "console.h"

#define UART_BASE     0x10000000UL
#define UART_THR      (*(volatile uint8_t *)(UART_BASE + 0))
#define UART_LSR      (*(volatile uint8_t *)(UART_BASE + 5))
#define LSR_THR_EMPTY 0x20

void console_putc(char c)
{
    while ((UART_LSR & LSR_THR_EMPTY) == 0) {
    }
    UART_THR = (uint8_t)c;
}
