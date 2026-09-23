/* Lab3 starter (x86_64)：内核 C 入口。到这里为止 BSS 已经清零、栈已经
 * 设好（Lab2 的内容，boot.S 里没有本 Lab 的 TODO）。这个函数要做三件事：
 * 调用内存探测（memmap_discover，见 memmap.c 的 TODO），做一次
 * kalloc_pages/kfree_pages 往返验证，最后触发一次 panic() 作检查点。
 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"

void memmap_discover(uint32_t mb2_info_addr);

void kernel_main(uint32_t mb2_info_addr)
{
    /* TODO 1：打印身份字符串
     * "Hello OS from x86_64 (Lab3: physical memory)"，然后调用
     * memmap_discover(mb2_info_addr)。
     *
     * 提示：console_puts_line("Hello OS from x86_64 (Lab3: physical memory)");
     */


    /* TODO 2：记录调用 kalloc_pages 之前的空闲页数
     * （kalloc_free_pages()），跑一次 kalloc_pages(4)：
     *   - 结果是 NULL 就 panic（说明没有可用内存，一般是 TODO 1 里的
     *     内存探测没跑对）。
     *   - 结果不是页对齐（地址 % PAGE_SIZE != 0）也 panic（分配器的
     *     不变量：kalloc_pages 返回的地址必须是页边界，不页对齐说明
     *     分配器实现或者调用方式有问题——本 Lab starter 版本的
     *     kalloc.c 是现成实现，不需要你改，这里更可能是排查方向）。
     *   - 打印分配到的地址：kprintf("kalloc_pages(4) = %p\n", pages);
     *     （%p 见 console.h 顶部注释，用于 64 位指针）。
     *
     * 提示：
     *   size_t free_before = kalloc_free_pages();
     *   void *pages = kalloc_pages(4);
     *   if (pages == NULL) {
     *       panic("kalloc_pages(4) returned NULL, expected available memory");
     *   }
     *   if ((uintptr_t)pages % PAGE_SIZE != 0) {
     *       panic("kalloc_pages(4) returned an address that is not page-aligned");
     *   }
     *   kprintf("kalloc_pages(4) = %p\n", pages);
     */


    /* TODO 3：调用 kfree_pages(pages, 4) 释放刚分配的页，再读一次
     * kalloc_free_pages()，必须和 TODO 2 里记录的 free_before 完全一致
     * ——不一致说明分配器有泄漏或者重复计数（这是检验"分配/释放不重叠
     * 不泄漏"最直接的方式）。一致的话打印一行确认信息，格式必须和
     * tests/expect-x86_64.txt 逐字匹配：
     *
     *   kfree_pages(pages, 4);
     *   size_t free_after = kalloc_free_pages();
     *   if (free_after != free_before) {
     *       panic("kfree_pages() did not restore free page count, leak or double-count");
     *   }
     *   kprintf("kalloc/kfree round-trip OK, free pages unchanged at %lu\n", free_after);
     */


    /* TODO 4：用 panic() 结束，延续本课程每个 Lab 用一次故意触发的
     * panic 作检查点的习惯。消息用
     * "Lab3 checkpoint: kalloc/kfree verified, halting here on purpose"。
     */
}
