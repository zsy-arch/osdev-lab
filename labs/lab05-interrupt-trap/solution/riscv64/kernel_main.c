#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "sbi.h"

void trap_init(void);
void timer_enable(void);

extern char __kernel_end[];
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull
#define KERNEL_LOAD_ADDR 0x80200000ull

#define TEST_VA (KERNEL_VIRT_BASE + 0x10000000ull)

extern volatile uint64_t timer_ticks;

static uint64_t read_time(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, time" : "=r"(value));
    return value;
}

/* Lab4 用来演示 page fault 的那个"故意读一个从没映射过的地址"已经
 * 完成了它的教学目的——理由跟 x86_64 版本 kernel_main.c 里同一段
 * 注释一样，Lab5 不重复这个演示。 */

void kernel_boot(uint64_t hartid, uint64_t dtb_paddr)
{
    (void)hartid;
    (void)dtb_paddr;

    console_puts_line("Hello OS from riscv64 (Lab5: interrupt & timer)");

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

    pagetable_map(root, 0x10000000ull, 0x10000000ull, PTE_FLAG_WRITABLE);
    pagetable_map(root, TEST_VA, phys_start, PTE_FLAG_WRITABLE);

    pagetable_activate(root);

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

    /* trap_init() 只是把 stvec 指过去（跟 Lab4 一样），本身不会让任何
     * 中断真正被处理——还需要 timer_enable()（开 sie.STIE + sstatus.SIE
     * 两层开关，见 trap.c 里的注释）和至少一次 sbi_set_timer()（第一次
     * 预约必须由软件主动做，硬件不会在内核什么都没请求的情况下自己冒出
     * 第一个定时器中断）。三步的顺序要求跟 x86_64 版本
     * idt_init()->pit_init()->sti 是同一个道理：先把"收到中断之后跳
     * 到哪"接好，再把"允许收"打开，最后才去主动触发第一次，避免中间
     * 状态不一致时冒出一个还接不住的中断。 */
    trap_init();
    timer_enable();
    sbi_set_timer(read_time() + 10000000ull / 100ull);
    console_puts_line("timer armed at 100Hz, entering main loop");

    /* wfi（Wait For Interrupt）是 riscv 版本的 hlt：让 hart 停下来等
     * 下一次中断，跟 boot.S 结尾的 wfi/j 1b halt 循环是同一条指令，
     * 这里只是把它从"永远等不到中断的死循环"变成"每次醒来做点事再
     * 继续等"。 */
    uint64_t next_report_tick = 100; /* 100 tick = 1 秒（TIMER_HZ=100）。 */
    for (;;) {
        __asm__ volatile("wfi");
        if (timer_ticks >= next_report_tick) {
            kprintf("tick: %lu seconds\n", next_report_tick / 100);
            next_report_tick += 100;
        }
    }
}
