/* 架构无关的控制台/格式化输出接口。
 *
 * console_putc() 由每个架构分别实现（x86_64 走 0x3F8 端口 I/O，riscv64 走
 * 0x10000000 MMIO），本文件声明的其它函数都建立在它之上，不关心底层怎么
 * 把一个字节送出去。这是本课程"共享代码 vs 架构专属代码"分界的典型例子。
 */
#ifndef OSDEV_CONSOLE_H
#define OSDEV_CONSOLE_H

#include "types.h"

/* 架构专属实现：把一个字符写到串口。*/
void console_putc(char c);

/* 架构专属实现：从串口取一个字符；此刻没有输入时返回 -1。Lab9 起使用。
 *
 * 返回 int 而不是 char，是为了让"没有数据"有地方表达——char 的 256 个取值
 * 全都是合法的输入字节，挤不出一个哨兵值。这和标准库 getchar() 返回 int
 * 是同一个理由（它用 EOF == -1）。
 *
 * **不阻塞**。没有输入就立刻返回 -1，由调用者决定是等还是走。这个选择把
 * "怎么等"留在了上层：Lab9 的 sys_read 在 FD_CONSOLE 上是 yield() 忙等
 * （见 trap.c），而中断驱动的实现会换成"挂进等待队列"——底下这个函数
 * 两种情况下都不用改。反过来如果这里直接死等，上层就再没有选择余地，
 * 而且一个进程等键盘会把整个系统卡住（单核 + 不可抢占的系统调用）。
 *
 * Lab0~Lab8 只声明不实现也没关系：声明本身不产生任何符号引用，链接器
 * 只在真的有人调用时才去找定义。 */
int console_getc(void);

/* 建立在 console_putc 之上，架构无关。*/
void console_puts(const char *s);

/* 极简 printf：支持 %d %u %x %p %s %c %% ，以及 l 长度修饰符 %ld %lu %lx——
 * 本课程两个目标架构（x86_64/riscv64）都是 LP64，long 和指针一样是 64 位，
 * 不加 l 的 %d/%u/%x 走 va_arg 取的是 32 位的 int/unsigned，直接拿它们打印
 * uint64_t（比如 Lab3 起要打印的物理地址、内存区域大小）会读错栈上的
 * 参数——不是"打印出来的数不好看"，是实打实的未定义行为。打印 64 位值时
 * 用 %p（地址）或者 %lx/%lu（数值），不要用不带 l 的版本。
 * 除此之外不支持宽度/精度；内核 panic 路径和早期调试用够了，不追求兼容
 * 标准库全部格式。 */
void kprintf(const char *fmt, ...);

/* 打印一行后自动换行，等价于 kprintf("%s\n", s) 但不经过可变参数机制，
 * 在还没验证可变参数调用约定是否工作的极早期 boot 阶段更可靠。 */
void console_puts_line(const char *s);

#endif /* OSDEV_CONSOLE_H */
