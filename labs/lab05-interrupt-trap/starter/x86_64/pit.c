/* Lab5 x86_64：PIT（8253/8254）驱动 + legacy PIC（8259A）重新映射。
 *
 * 为什么需要重新映射 PIC（这是本文件里"看起来奇怪但必须做"的部分）：
 * 8259A 出厂/BIOS 默认把 IRQ0-7 映射到中断向量 0x08-0x0F——这一段跟
 * x86 CPU 自己的异常向量（0x00-0x1F，比如 Lab4 用到的 #PF=14=0x0E）
 * 完全重叠。IBM PC 最初的设计者选了这个映射，那时 Intel 还没有把
 * 0x00-0x1F 保留给异常——真正跑起来的效果：IRQ0（定时器）触发时 CPU
 * 会把它当成 0x08 号向量递送，而 0x08 号是 #DF（Double Fault），本该
 * 触发定时器中断的信号会被误当成致命异常处理。这不是本课程实现的 bug，
 * 是 x86 PC 平台从 IBM PC/XT 时代继承下来的历史遗留问题，所有 x86 内核
 * （Linux included）都要做这一步重映射。
 * 标准做法：通过 8259A 的 command/data 端口把 IRQ0-15 重新映射到
 * 0x20-0x2F（0x00-0x1F 让给 CPU 异常，这也是为什么 Lab4 的 idt_init()
 * 把 #PF 放在向量 14 而不会跟任何 IRQ 冲突——因为直到现在 PIC 还没被
 * 重映射，IRQ 还停在出厂默认的 0x08-0x0F）。
 *
 * 参考：Intel 8259A datasheet "Initialization Command Words (ICW1-4)"；
 * OSDev Wiki "8259 PIC" 页面的标准重映射序列。
 */
#include "types.h"
#include "pit.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIT_CH0_DATA 0x40
#define PIT_MODE_CMD 0x43

/* PIT 内部计数器的输入时钟频率是固定的 1193182 Hz（这是 IBM PC 最初
 * 选定的晶振分频结果，跟 NTSC 彩色副载波频率有历史关系，但对本课程
 * 来说只是一个必须记住的硬件常数，PIT 芯片本身不能改变这个输入频率，
 * 只能改"计数到多少归零一次"，即下面的 reload value）。 */
#define PIT_INPUT_HZ 1193182u
#define TIMER_HZ     100u

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 已经写好，不是 TODO：跟 Lab4 的 idt[]/gdt[] 一样，这是一个纯粹的
 * 状态变量声明，没有需要你做决定的地方——真正的知识点在下面几个
 * 函数体里。 */
volatile uint64_t pit_ticks = 0;

/* TODO 1：实现 pic_remap() —— 把 8259A 的 IRQ0-15 从出厂默认的
 * 0x08-0x0F/0x70-0x77 重新映射到 0x20-0x2F。
 *
 * 提示：
 * 1) 先用 inb() 读出 PIC1_DATA/PIC2_DATA 当前的 mask，保存下来——
 *    重映射过程会临时打乱它们，结束后要恢复回*调用前*的状态（不是
 *    恢复出厂默认），因为本 Lab 只想开 IRQ0，其它 IRQ 保持原样。
 * 2) ICW1：向 PIC1_CMD/PIC2_CMD 各写 0x11（开始初始化序列，
 *    cascade mode，需要 ICW4）。
 * 3) ICW2：向 PIC1_DATA 写 0x20（IRQ0-7 -> 向量 0x20-0x27），
 *    向 PIC2_DATA 写 0x28（IRQ8-15 -> 向量 0x28-0x2F）。
 * 4) ICW3：向 PIC1_DATA 写 0x04（主片 IRQ2 接从片），
 *    向 PIC2_DATA 写 0x02（从片告诉自己接在主片 IRQ2 上）。
 * 5) ICW4：向 PIC1_DATA/PIC2_DATA 各写 0x01（8086/8088 模式，
 *    不用 auto-EOI/buffered mode）。
 * 6) 把第 1 步保存的 mask 写回 PIC1_DATA/PIC2_DATA。
 *
 * static void pic_remap(void)
 * {
 *     uint8_t mask1 = inb(PIC1_DATA);
 *     uint8_t mask2 = inb(PIC2_DATA);
 *
 *     outb(PIC1_CMD, 0x11);
 *     outb(PIC2_CMD, 0x11);
 *
 *     outb(PIC1_DATA, 0x20);
 *     outb(PIC2_DATA, 0x28);
 *
 *     outb(PIC1_DATA, 0x04);
 *     outb(PIC2_DATA, 0x02);
 *
 *     outb(PIC1_DATA, 0x01);
 *     outb(PIC2_DATA, 0x01);
 *
 *     outb(PIC1_DATA, mask1);
 *     outb(PIC2_DATA, mask2);
 * }
 */

/* TODO 2：实现 pic_unmask_irq0() —— 取消屏蔽 IRQ0（PIC1 mask 的第 0
 * 位），保持其它所有 IRQ 屏蔽（本 Lab 只处理定时器这一个中断源）。
 *
 * 提示：
 * static void pic_unmask_irq0(void)
 * {
 *     uint8_t mask = inb(PIC1_DATA);
 *     mask &= (uint8_t)~0x01u;
 *     outb(PIC1_DATA, mask);
 * }
 */

/* TODO 3：实现 pic_send_eoi() —— 每次处理完一个 PIC 递送的中断后，必须
 * 显式发 EOI（End Of Interrupt），否则 PIC 会认为"上一个中断还没处理
 * 完"，不再递送任何新的中断（不只是同一个 IRQ，是整片 PIC 都会卡住）——
 * 这跟 x86_64 CPU 自己的异常不一样，异常（比如 Lab4 的 #PF）iretq 就
 * 结束了，不需要额外告诉谁"处理完了"，但外部中断源（PIC 管理的那些）
 * 需要。
 *
 * 提示：0x20 是 8259A OCW2 里"non-specific EOI"的命令码，写到 PIC1_CMD。
 * static void pic_send_eoi(void)
 * {
 *     outb(PIC1_CMD, 0x20);
 * }
 */

/* TODO 4：实现 timer_interrupt_handler() —— trap_entry.S 里的
 * timer_stub 会 call 到这里（零参数，IRQ0 没有硬件 error code）。
 *
 * 提示：自增 pit_ticks，然后发 EOI。
 * void timer_interrupt_handler(void)
 * {
 *     pit_ticks++;
 *     pic_send_eoi();
 * }
 */

/* TODO 5：实现 pit_init() —— 重映射 PIC，把 PIT 通道 0 设成 100Hz
 * 周期性中断，取消屏蔽 IRQ0。
 *
 * 提示：
 * 1) 先调 pic_remap()。
 * 2) 往 PIT_MODE_CMD 写命令字节 0x34（Intel 8254 datasheet
 *    "Mode/Command Register"）：
 *      bits 6-7 = 00：选通道 0（接到 IRQ0）
 *      bits 4-5 = 11：access mode = lobyte/hibyte（reload value 分
 *                     两次写，先低 8 位再高 8 位）
 *      bits 1-3 = 010：mode 2，rate generator——计数到 0 就触发一次
 *                      中断并自动重新装载，产生周期性方波
 *      bit 0 = 0    ：BCD=0，用二进制计数
 * 3) reload = PIT_INPUT_HZ / TIMER_HZ（PIT 计数器每收到一个输入时钟
 *    脉冲减 1，减到 0 触发一次中断并重新装载这个值），先写低字节
 *    再写高字节到 PIT_CH0_DATA。
 * 4) 最后调 pic_unmask_irq0()。
 *
 * void pit_init(void)
 * {
 *     pic_remap();
 *
 *     outb(PIT_MODE_CMD, 0x34);
 *
 *     uint16_t reload = (uint16_t)(PIT_INPUT_HZ / TIMER_HZ);
 *     outb(PIT_CH0_DATA, (uint8_t)(reload & 0xFF));
 *     outb(PIT_CH0_DATA, (uint8_t)((reload >> 8) & 0xFF));
 *
 *     pic_unmask_irq0();
 * }
 */
