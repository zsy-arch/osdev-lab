/* Lab5 x86_64：PIT（Intel 8253/8254 Programmable Interval Timer）驱动的
 * 公开接口。跟本课程其它"太架构专属，没法放进 src/common/include/"的
 * 接口一样（比如 Lab4 的 idt_init()/gdt_init()），这里没有单独建一个共享
 * trap.h/timer.h——riscv64 那边完全是另一套硬件（SBI TIME extension，
 * 见 solution/riscv64/sbi.h），两边凑不出一份有意义的公共接口，勉强凑
 * 出来的公共接口只会是"名字一样、参数完全不同"的假抽象。
 *
 * 直接在 kernel_main.c 里 extern 声明这两个符号（跟 idt_init()/gdt_init()
 * 现在的做法一样），本文件只是给 pit.c 自己用的头。 */
#ifndef OSDEV_PIT_H
#define OSDEV_PIT_H

#include "types.h"

/* 初始化 PIT：设成 100Hz 方波中断（模式 2，见 pit.c 里的注释），但不会
 * 自己开中断——调用方需要在这之后自己 idt_set_entry + sti。 */
void pit_init(void);

/* 从 pit_init() 起过了多少个 tick（每 tick = 1/100 秒）。timer_interrupt_
 * handler() 每次处理 IRQ0 都会自增，main loop 靠读这个值判断"又过了一秒"。
 * volatile：会在中断处理函数（不受编译器"看不出会被谁修改"的正常调用流
 * 控制）和主循环之间读写，编译器不能假设它在两次读取之间不变。 */
extern volatile uint64_t pit_ticks;

#endif /* OSDEV_PIT_H */
