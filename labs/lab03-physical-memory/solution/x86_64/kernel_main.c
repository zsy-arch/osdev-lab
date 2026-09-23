#include "console.h"
#include "panic.h"
#include "kalloc.h"

/* 架构专属实现，声明在这里而不是共享头：memmap_discover() 两个架构的
 * 参数类型不一样（x86_64 收 Multiboot2 info 的 32 位物理地址，riscv64
 * 收 DTB 的 64 位物理地址），没有一个共同的签名可以放进 kalloc.h 之类
 * 的共享头——跟 panic_halt() 在 panic.c 里就地声明是同一个理由，见那边。 */
void memmap_discover(uint32_t mb2_info_addr);

/* GRUB 按 Multiboot2 规范把 boot info 结构体的物理地址通过 %ebx 交给
 * _start，boot.S 的 _start64 又把它原样转交进 %edi（System V AMD64
 * 调用约定的第一个整数参数寄存器），所以这里第一个参数就是它——具体
 * 怎么从 %ebx 转过来、为什么必须用 32 位寄存器名 %edi 而不是 %rdi，
 * 见 boot.S 里 _start64 那条 mov 上面的注释。 */
void kernel_main(uint32_t mb2_info_addr)
{
    console_puts_line("Hello OS from x86_64 (Lab3: physical memory)");

    memmap_discover(mb2_info_addr);

    /* 分配/释放的最小验证：要 4 页，确认拿到的地址页对齐且互不相同
     * （first-fit 从一段连续空闲区切出来，天然连续，这里只抽样检查
     * 首尾两页地址上的算术关系，不是穷举证明），释放后空闲页数应该
     * 和分配前完全一样——这正是 kalloc.h 里说的"没有泄漏、没有重复
     * 计数"最直接的检验方式。 */
    size_t free_before = kalloc_free_pages();

    void *pages = kalloc_pages(4);
    if (pages == NULL) {
        panic("kalloc_pages(4) returned NULL, expected available memory");
    }
    if ((uintptr_t)pages % PAGE_SIZE != 0) {
        panic("kalloc_pages(4) returned an address that is not page-aligned");
    }

    kprintf("kalloc_pages(4) = %p\n", pages);

    kfree_pages(pages, 4);

    size_t free_after = kalloc_free_pages();
    if (free_after != free_before) {
        panic("kfree_pages() did not restore free page count, leak or double-count");
    }

    kprintf("kalloc/kfree round-trip OK, free pages unchanged at %lu\n", free_after);

    panic("Lab3 checkpoint: kalloc/kfree verified, halting here on purpose");
}
