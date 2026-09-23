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
 * OSDev Wiki "8259 PIC" 页面的标准重映射序列（本课程从零推导，这里只是
 * 确认序列跟社区文档一致，不是照抄不理解）。
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

volatile uint64_t pit_ticks = 0;

static void pic_remap(void)
{
    /* 保存两片 8259A 当前的 IRQ mask（重映射过程会临时打乱它们，结束后
     * 恢复——本 Lab 只想开 IRQ0，其它 IRQ 保持"重映射前是什么状态就还是
     * 什么状态"，不去猜测/改变调用方没要求过的行为）。 */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1：开始初始化序列，0x11 = ICW4 会跟着来（bit0） | edge-triggered、
     * cascade mode（bit1=0，两片级联）| 需要 ICW4（bit0=1）。 */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);

    /* ICW2：设置向量偏移——这是"重映射"真正生效的一步。IRQ0-7 -> 0x20-
     * 0x27，IRQ8-15 -> 0x28-0x2F，避开 0x00-0x1F 的 CPU 异常向量。 */
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);

    /* ICW3：告诉每片 PIC 级联关系——主片（PIC1）的 IRQ2 接从片
     * （PIC2）（bit2=1=0x04），从片告诉自己接在主片的哪根 IRQ 上（2）。 */
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);

    /* ICW4：8086/8088 模式（bit0=1），本课程不用 auto-EOI/buffered
     * mode，其余位保持 0。 */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* 恢复调用前的 mask，而不是恢复出厂默认——理由见函数开头注释。 */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/* 取消屏蔽 IRQ0（PIC1 的第 0 位），保持其它所有 IRQ 屏蔽——本 Lab 只
 * 处理定时器这一个中断源，其它设备（键盘、串口……）留给以后的 Lab。 */
static void pic_unmask_irq0(void)
{
    uint8_t mask = inb(PIC1_DATA);
    mask &= (uint8_t)~0x01u;
    outb(PIC1_DATA, mask);
}

/* PIC 送完一个中断后必须显式发 EOI（End Of Interrupt），否则它会认为
 * "上一个中断还没处理完"，不再递送任何新的中断（不只是同一个 IRQ，是
 * 整片 PIC 都会卡住）——这跟 x86_64 CPU 自己的异常不一样，异常（比如
 * Lab4 的 #PF）iretq 就结束了，不需要额外告诉谁"处理完了"，但外部
 * 中断源（PIC 管理的那些）需要。0x20 是 8259A 的 OCW2 里
 * "non-specific EOI" 命令码。 */
static void pic_send_eoi(void)
{
    outb(PIC1_CMD, 0x20);
}

void timer_interrupt_handler(void)
{
    pit_ticks++;
    pic_send_eoi();
}

void pit_init(void)
{
    pic_remap();

    /* PIT command byte（Intel 8254 datasheet "Mode/Command Register"）：
     *   bits 6-7 = 00：选通道 0（接到 IRQ0，另外两个通道本课程不用）
     *   bits 4-5 = 11：access mode = lobyte/hibyte（reload value 分两次
     *                  写，先写低 8 位再写高 8 位，跟下面的写入顺序对应）
     *   bits 1-3 = 010：mode 2，rate generator——计数到 0 就触发一次
     *                   中断并自动重新装载，产生周期性方波，这正是"每秒
     *                   固定触发 N 次"需要的模式（相对 mode 0"只触发一次"
     *                   和 mode 3"方波但占空比语义不同"，mode 2 是教学上
     *                   最直接对应"周期性 tick"这个需求的选择）
     *   bit 0 = 0    ：BCD=0，用二进制计数，不用 BCD */
    outb(PIT_MODE_CMD, 0x34);

    /* reload value = 输入频率 / 目标频率——PIT 计数器每收到一个输入时钟
     * 脉冲减 1，减到 0 触发一次中断并重新装载这个值，所以"减到 0 需要
     * 多少个输入脉冲"就是"两次中断之间间隔多少个 1/1193182 秒"，
     * 换算成频率就是 1193182/reload。整数除法在这里只有轻微舍入误差
     * （100Hz 目标下误差远小于 1 tick/秒，教学场景够用，不需要更精细的
     * 分数频率合成）。 */
    uint16_t reload = (uint16_t)(PIT_INPUT_HZ / TIMER_HZ);
    outb(PIT_CH0_DATA, (uint8_t)(reload & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((reload >> 8) & 0xFF));

    pic_unmask_irq0();
}
