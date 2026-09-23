/* Lab5 x86_64：在 Lab4"只填 #PF 这一项"的 IDT 基础上，加上第二个
 * 中断源——PIT 定时器（重映射后的向量 32，即 IRQ0）。GDT 重新加载、
 * IDT 的形状/idt_set_entry 的写法全部照抄 Lab4——这些不是 Lab5 的新
 * 教学内容，Lab5 新增的只有"PIC 需要重映射""定时器中断需要在处理完后
 * 发 EOI""中断和异常的 stub 保存/恢复方式不完全一样（有没有硬件 error
 * code）"这三点，其它部分维持 Lab4 原样不重写、不重新讲一遍。
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "pit.h"

static uint64_t gdt[2] = {
    0,
    0x00AF9A000000FFFFull,
};

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void)
{
    static struct gdt_pointer gdtp;
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uintptr_t)gdt;

    __asm__ volatile("lgdt %0" : : "m"(gdtp));
}

/* 33，不是 32：CPU 保留的异常向量是 0-31（本 Lab 只用到 14 号 #PF），
 * PIC 重映射后 IRQ0-15 落在 32-47（见 pit.c pic_remap() 的映射），本 Lab
 * 只用 IRQ0=32 这一个，但数组大小必须能容纳"最大用到的下标+1"，也就是
 * 33——写成跟 Lab4 一样的 32 会让 idt[32] 越界写到数组之后的内存（这里
 * 越界写命中的正好是紧跟在 idt[] 后面声明的 gdtp/idtp 之类静态变量，
 * 表现为 IDT 指针本身被污染，第一次定时器中断处理完 iretq 返回后 CPU
 * 状态就不对了——实测在 QEMU 的 `-d int` 日志里确认过这个现象：v=0x20
 * 正常进入一次，随后立刻级联 v=0x0d（#GP）->v=0x08（#DF），且始终停在
 * 同一个 RIP，最终定位到是这个越界写而不是 PIC/PIT 硬件配置错误）。 */
#define IDT_ENTRIES    33
#define IDT_TYPE_INT64 0x8E /* Present(1) | DPL(00) | 0 | type(1110) */
#define IDT_VEC_PAGE_FAULT 14

/* PIC 重映射后 IRQ0（PIT 定时器）落在的向量号——见 pit.c pic_remap()
 * 里 "IRQ0-7 -> 0x20-0x27" 的映射，IRQ0 对应 0x20 = 32。 */
#define IDT_VEC_TIMER 32

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];

#define PF_ERR_PRESENT (1u << 0)
#define PF_ERR_WRITE   (1u << 1)
#define PF_ERR_USER    (1u << 2)

static uintptr_t read_cr2(void)
{
    uintptr_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

void page_fault_handler(uint64_t error_code)
{
    uintptr_t fault_addr = read_cr2();

    kprintf("page fault: addr=%p error_code=%lx (%s%s%s)\n",
            (void *)fault_addr, error_code,
            (error_code & PF_ERR_PRESENT) ? "protection-violation" : "not-present",
            (error_code & PF_ERR_WRITE) ? ",write" : ",read",
            (error_code & PF_ERR_USER) ? ",user" : ",kernel");

    panic("page_fault_handler: unrecoverable page fault (Lab4/5 do not implement fault recovery)");
}

/* timer_interrupt_handler() 定义在 pit.c 里（自增 pit_ticks、发 EOI）——
 * 跟 page_fault_handler 不一样，这个处理函数正常返回（没有 panic），
 * 这也是为什么 timer_stub（trap_entry.S）的恢复+iret 路径在 Lab5 里
 * 真正会被执行到，不再是"写完整但永远跑不到"的防御性代码。 */
extern void timer_interrupt_handler(void);

extern void page_fault_stub(void);
extern void timer_stub(void);

static void idt_set_entry(int vector, void (*handler)(void))
{
    uintptr_t addr = (uintptr_t)handler;

    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = 0x08;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = IDT_TYPE_INT64;
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    idt_set_entry(IDT_VEC_PAGE_FAULT, page_fault_stub);
    idt_set_entry(IDT_VEC_TIMER, timer_stub);

    static struct idt_pointer idtp;
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uintptr_t)idt;

    __asm__ volatile("lidt %0" : : "m"(idtp));
}
