/* Lab3 starter (riscv64)：内核 C 入口。到这里为止 BSS 已经清零、栈已经
 * 设好（Lab2 的内容，boot.S 里没有本 Lab 的 TODO）。这个函数要做三件事：
 * 调用内存探测（memmap_discover，见 memmap.c 的 TODO），做一次
 * kalloc_pages/kfree_pages 往返验证，最后触发一次 panic() 作检查点。
 *
 * 参数顺序 (hartid, dtb_paddr) 必须和 a0/a1 的顺序一致，即使 hartid
 * 本身没用到——如果顺序写反，编译器会把 a1（真正的 DTB 地址）当成
 * hartid 读，把 a0（hartid）当成 dtb_paddr 传给 memmap_discover，
 * 表现成"DTB magic 校验失败"或者更隐蔽的解析出一堆垃圾数据。
 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"

void memmap_discover(uint64_t dtb_paddr);

void kernel_main(uint64_t hartid, uint64_t dtb_paddr)
{
    (void)hartid;

    /* TODO 1：打印身份字符串
     * "Hello OS from riscv64 (Lab3: physical memory)"，然后调用
     * memmap_discover(dtb_paddr)。
     */


    /* TODO 2：记录调用前的空闲页数，跑一次 kalloc_pages(4)，校验非 NULL、
     * 页对齐，打印分配到的地址。逻辑和 x86_64 版本完全一样（分配器本身
     * 是架构无关的 src/common/kalloc.c），具体提示见
     * labs/lab03-physical-memory/starter/x86_64/kernel_main.c 的 TODO 2。
     */


    /* TODO 3：kfree_pages(pages, 4)，校验释放后空闲页数和释放前一致，
     * 打印确认信息。格式必须和 tests/expect-riscv64.txt 逐字匹配，
     * 提示同 x86_64 版本的 TODO 3。
     */


    /* TODO 4：用 panic() 结束，消息用
     * "Lab3 checkpoint: kalloc/kfree verified, halting here on purpose"。
     */
}
