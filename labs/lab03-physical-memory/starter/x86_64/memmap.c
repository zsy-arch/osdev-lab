/* Lab3 starter (x86_64)：解析 GRUB 传进来的 Multiboot2 boot information
 * 结构，找到内存映射（memory map）tag，把每一段"可用 RAM"喂给
 * kalloc_add_region()。
 *
 * Multiboot2 boot information 布局（完整字段定义见 README.md「核心概念」
 * 一节，这里只给实现需要的关键点）：
 *
 *   开头 8 字节头：{ total_size (u32), reserved (u32) }。
 *
 *   紧跟着一串 8 字节对齐的 tag，每个 tag 自己也有头：
 *     { type (u32), size (u32) }
 *   从当前 tag 跳到下一个 tag 的偏移量是 (size + 7) & ~7，不是 size 本身
 *   ——size 描述的是这个 tag 实际占用的字节数，下一个 tag 必须从 8 字节
 *   边界开始，中间的 padding 字节需要手动跳过。
 *
 *   type == 0 是结束标记（遇到就停）。
 *   type == 6 是内存映射 tag，比普通 tag 多两个字段：
 *     { type, size, entry_size (u32), entry_version (u32) }
 *   紧跟着若干条表项，每条表项占 entry_size 字节（用这个字段的值，不要
 *   硬编码 24，即使实测这个值目前总是 24）：
 *     { base_addr (u64), length (u64), type (u32), reserved (u32) }
 *   表项的 type == 1 表示这段是可用 RAM。
 */
#include "types.h"
#include "console.h"
#include "kalloc.h"
#include "panic.h"

#define MB2_TAG_END      0
#define MB2_TAG_MMAP     6
#define MB2_MEMORY_AVAIL 1

struct mb2_tag_header {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

static uint64_t align_up8(uint64_t v)
{
    return (v + 7) & ~(uint64_t)7;
}

void memmap_discover(uint32_t mb2_info_addr)
{
    /* TODO 1：把 mb2_info_addr 当成指向 boot information 结构开头的
     * 物理地址（本 Lab 阶段页表仍是身份映射，物理地址可以直接当指针
     * 解引用）。读开头的 total_size 字段（第一个 u32）。
     *
     * 提示：
     *   const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_addr;
     *   uint32_t total_size = *(const uint32_t *)base;
     */


    /* TODO 2：从偏移 8 开始遍历 tag（前 8 字节是 total_size/reserved 头，
     * 不是第一个 tag）。用一个 uint64_t regions_found 计数找到了多少段
     * 可用内存，初始为 0。
     *
     * 循环骨架：
     *   uint64_t offset = 8;
     *   while (offset < total_size) {
     *       const struct mb2_tag_header *hdr =
     *           (const struct mb2_tag_header *)(base + offset);
     *
     *       if (hdr->type == MB2_TAG_END) {
     *           break;
     *       }
     *
     *       if (hdr->type == MB2_TAG_MMAP) {
     *           // TODO 3 的内容填在这里
     *       }
     *
     *       offset += align_up8(hdr->size);
     *   }
     */


    /* TODO 3（嵌套在 TODO 2 的 if (hdr->type == MB2_TAG_MMAP) 分支里）：
     * 把 hdr 转成 struct mb2_mmap_tag *，用它的 entry_size 字段遍历表项。
     * 表项区域的起始偏移是当前 tag 起始偏移 + sizeof(struct mb2_mmap_tag)
     * （也就是 16，四个 u32 字段），结束偏移是当前 tag 起始偏移 +
     * mmap_tag->size。
     *
     * 每条表项转成 struct mb2_mmap_entry *，type == MB2_MEMORY_AVAIL 的
     * 就调用 kalloc_add_region(entry->base_addr, entry->length)，
     * regions_found 加一。表项之间前进 mmap_tag->entry_size 字节
     * （不是 sizeof(struct mb2_mmap_entry)）。
     *
     * 提示：
     *   const struct mb2_mmap_tag *mmap_tag =
     *       (const struct mb2_mmap_tag *)hdr;
     *   uint64_t entry_offset = offset + sizeof(struct mb2_mmap_tag);
     *   uint64_t entries_end = offset + mmap_tag->size;
     *   while (entry_offset < entries_end) {
     *       const struct mb2_mmap_entry *entry =
     *           (const struct mb2_mmap_entry *)(base + entry_offset);
     *       if (entry->type == MB2_MEMORY_AVAIL) {
     *           kalloc_add_region(entry->base_addr, entry->length);
     *           regions_found++;
     *       }
     *       entry_offset += mmap_tag->entry_size;
     *   }
     */


    /* TODO 4：循环结束后，打印找到了多少段可用内存、当前空闲页总数
     * （kalloc_free_pages()）。如果 regions_found 是 0，说明没有找到
     * 任何可用内存，调用 panic()——这不是一个可以安静忽略的情况，
     * 后面的 kalloc_pages() 调用注定会失败。
     *
     * 格式必须和 tests/expect-x86_64.txt 逐字匹配（%lu 用于 64 位值，
     * 见 console.h 顶部注释）：
     *
     *   kprintf("memmap: %lu available region(s) from Multiboot2 mmap tag, "
     *           "%lu page(s) free\n",
     *           regions_found, kalloc_free_pages());
     *
     *   if (regions_found == 0) {
     *       panic("memmap: no available memory region found in Multiboot2 info");
     *   }
     */
}
