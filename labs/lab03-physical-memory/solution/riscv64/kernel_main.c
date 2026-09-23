#include "console.h"
#include "panic.h"
#include "kalloc.h"

/* 架构专属实现，就地声明的理由和 x86_64 那份 kernel_main.c 完全一样：
 * memmap_discover() 两个架构的参数类型不同，没有共同签名可以放共享头。 */
void memmap_discover(uint64_t dtb_paddr);

/* OpenSBI 交接控制权时 a0=hartid、a1=DTB 物理地址，boot.S 的 _start
 * 完全不碰这两个寄存器就直接 call kernel_main，RISC-V 调用约定的前两个
 * 整数参数寄存器正好是 a0/a1，所以这里的参数顺序必须是
 * (hartid, dtb_paddr)——具体见 boot.S 顶部注释。hartid 本 Lab 用不上
 * （单核场景，多核调度是后面 Lab 的内容），但签名必须占住这个位置，
 * 不能只声明 kernel_main(uint64_t dtb_paddr) 然后指望编译器帮忙把 a1
 * 挪到第一个参数——C 函数签名的参数顺序直接对应调用约定的寄存器顺序，
 * 少写一个参数，函数体里读到的第一个参数就会是 a0（hartid）而不是
 * a1（DTB 地址）。 */
void kernel_main(uint64_t hartid, uint64_t dtb_paddr)
{
    (void)hartid;

    console_puts_line("Hello OS from riscv64 (Lab3: physical memory)");

    memmap_discover(dtb_paddr);

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
