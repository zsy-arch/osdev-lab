#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "pit.h"

void memmap_discover(uint32_t mb2_info_addr);
void idt_init(void);
void gdt_init(void);

extern char __kernel_end[];
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull
#define KERNEL_LOAD_ADDR 0x100000ull

#define TEST_VA 0xFFFFFFFF90000000ull

/* Lab4 用来演示 #PF 的那个"故意读一个从没映射过的地址"已经完成了它的
 * 教学目的（验证 idt_init() 注册的 handler 真的会被调用到）——Lab5 不
 * 重复这个演示，把同样的"验证中断真的送到了"精力放在定时器中断上
 * （下面的 main loop 本身就是持续验证：如果 idt_init()/pit_init()/sti
 * 任何一步有问题，loop 会卡死不打印，而不是"看起来正常但其实中断从来
 * 没触发过"）。 */

void kernel_boot(uint32_t mb2_info_addr)
{
    console_puts_line("Hello OS from x86_64 (Lab5: interrupt & timer)");

    memmap_discover(mb2_info_addr);

    uintptr_t kernel_end_phys = (uintptr_t)__kernel_end - KERNEL_VIRT_BASE;
    kprintf("kernel image: phys [%p, %p)\n", (uintptr_t)KERNEL_LOAD_ADDR,
            kernel_end_phys);

    uintptr_t root = pagetable_create();

    uintptr_t phys_start = KERNEL_LOAD_ADDR;
    uintptr_t phys_end = kernel_end_phys;
    for (uintptr_t pa = phys_start; pa < phys_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    uintptr_t self_map_end = KERNEL_LOAD_ADDR + 0x200000ull;
    for (uintptr_t pa = phys_end; pa < self_map_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    pagetable_map(root, TEST_VA, phys_start, PTE_FLAG_WRITABLE);

    pagetable_activate(root);
    gdt_init();

    console_puts_line("switched to Lab4 page table, low identity map gone");

    uintptr_t looked_up = pagetable_lookup(root, TEST_VA);
    if (looked_up != phys_start) {
        panic("pagetable_lookup(TEST_VA) did not return the mapped physical address");
    }

    uintptr_t self_map_check = pagetable_lookup(root, phys_start + KERNEL_VIRT_BASE);
    if (self_map_check != phys_start) {
        panic("kernel self-map lookup mismatch");
    }
    console_puts_line("page table from Lab4 verified, moving on to Lab5's interrupt framework");

    /* idt_init() 现在填了两项：#PF（保留 Lab4 的能力，虽然本 Lab 不再
     * 故意触发它）和定时器（vector 32）。pit_init() 负责重映射 PIC、
     * 设置 8254 的 reload value、取消屏蔽 IRQ0——但 PIC/PIT 本身不产生
     * "CPU 会响应"的效果，还差最后一步：sti（Set Interrupt Flag，
     * RFLAGS 的 IF 位）。CPU 从复位/boot.S 开始 IF 就是清零状态（这是
     * x86 架构规定的复位默认值，不是本课程哪一步意外清掉的），IF=0 时
     * CPU 会无条件忽略所有可屏蔽外部中断（#PF 这类异常不受 IF 影响，
     * 这也是为什么 Lab4 全程没开中断、#PF 依然能正常递送）——sti 必须
     * 是这一串初始化里的最后一步，提前执行会让"IDT 还没填定时器那项/
     * PIC 还没重映射"时就冒出中断，落进空 IDT entry 触发 #GP 级联。 */
    idt_init();
    pit_init();
    __asm__ volatile("sti");
    console_puts_line("timer armed at 100Hz, entering main loop");

    /* Lab4 结束在"故意触发一次异常然后 panic"，因为 Lab4 的教学目标
     * 就是"证明缺页异常处理管线是通的"，验证完了内核没有更多事可做。
     * Lab5 的教学目标是"周期性事件"，一次性验证不够，需要一个真正
     * 持续运行的内核主循环——hlt 让 CPU 停下来等下一次中断（省电，
     * 也是"内核目前没有别的事可做，等外部事件"的正确表达方式，不是
     * 用一个空转的 for(;;) 忙等去测 pit_ticks），中断处理完 iretq 回
     * 到 hlt 的下一条指令，检查 pit_ticks 是否跨过了下一个整秒的边界，
     * 跨过了就打印，然后回去再 hlt。 */
    uint64_t next_report_tick = 100; /* 100 tick = 1 秒（TIMER_HZ=100）。 */
    for (;;) {
        __asm__ volatile("hlt");
        if (pit_ticks >= next_report_tick) {
            kprintf("tick: %lu seconds\n", next_report_tick / 100);
            next_report_tick += 100;
        }
    }
}
