/* 内核 panic：打印文件名、行号、消息，然后让 CPU 停下来（架构专属的
 * "停下来"方式在 panic_halt() 里实现——x86 是 cli; hlt 循环，riscv 是
 * wfi 循环，见对应架构的 panic_arch.c）。
 *
 * 用 panic("msg") 宏而不是直接调 kernel_panic()，是为了自动捕获调用点的
 * __FILE__/__LINE__，这样出错时你不需要在几十个调用点里猜是哪一行触发的。
 */
#ifndef OSDEV_PANIC_H
#define OSDEV_PANIC_H

void kernel_panic(const char *file, int line, const char *msg);

#define panic(msg) kernel_panic(__FILE__, __LINE__, (msg))

#endif /* OSDEV_PANIC_H */
