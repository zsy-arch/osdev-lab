/* Lab4 starter (x86_64)：解析 GRUB 传进来的 Multiboot2 boot information
 * 结构，找到内存映射（memory map）tag，把每一段"可用 RAM"喂给
 * kalloc——喂之前先排除掉内核镜像自己占用的物理范围（Lab3 阶段没有这
 * 一步，那会还没有页表分配器会去踩这段内存）。
 *
 * tag 遍历部分（读 total_size、按 8 字节对齐步进 tag、按 entry_size
 * 步进 mmap 表项)和 Lab3 完全一样——你在 Lab3 里已经实现并验证过这段
 * 逻辑，这里直接原样保留，不再重复挖 TODO。本 Lab 唯一新增的是喂给
 * kalloc 之前的这一步过滤，也就是下面 add_region_excluding_kernel_
 * image()，这是本文件唯一的 TODO。
 */
#include "types.h"
#include "console.h"
#include "kalloc.h"
#include "panic.h"

#define MB2_TAG_END      0
#define MB2_TAG_MMAP     6
#define MB2_MEMORY_AVAIL 1

/* GRUB 的 Multiboot2 mmap tag 描述的是"物理内存上有哪些段是 RAM"，
 * 完全不知道内核自己已经占用了其中一段（内核镜像本来就加载在某一段
 * 可用 RAM 中间，不是单独一块 GRUB 会排除掉的区域）。Lab3 直接把每个
 * MB2_MEMORY_AVAIL 段整个丢给 kalloc_add_region()，能跑是因为 Lab3
 * 自己也没往这段内存里写页表——本 Lab 一开分页，
 * pagetable_create()/pagetable_map() 走 kalloc_page() 要新页表节点，
 * 如果 free pool 里混进了内核镜像自己占的物理页，分配出来的"空闲页"
 * 可能其实是内核的 .text/.data/.bss，写下去直接损坏正在跑的内核。
 *
 * __boot_load_addr/__kernel_phys_end 是 linker.ld 定义的链接期符号
 * （已经写好，不是 TODO），分别对应内核镜像物理起止
 * [__boot_load_addr, __kernel_phys_end)——两者都在 `. = KERNEL_LOAD_
 * ADDR` 之后、VMA=LMA 的低地址段范围内定义，此刻分页还没开、身份映射
 * 仍然有效，当普通指针取地址即是物理地址，不需要额外转换。 */
extern char __boot_load_addr[];
extern char __kernel_phys_end[];

/* TODO：处理"内核占用区间落在某个 mmap 区域中间"的一般情况——起始、
 * 结束都可能需要裁掉一部分，也可能这段区域和内核占用区间完全不相交，
 * 或者内核占用区间把这段区域整个盖住。Multiboot2 mmap 允许报告多段
 * 不连续的可用内存，不能假设内核只会落在某一段的开头或结尾。
 *
 * 提示（分三种情况，任意一种都不要重复调用 kalloc_add_region）：
 *   static void add_region_excluding_kernel_image(uint64_t region_base, uint64_t region_len)
 *   {
 *       uint64_t region_end = region_base + region_len;
 *       uint64_t kernel_start = (uint64_t)(uintptr_t)__boot_load_addr;
 *       uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_phys_end;
 *
 *       if (kernel_end <= region_base || kernel_start >= region_end) {
 *           // 完全不相交，整段照常提交
 *           kalloc_add_region(region_base, region_len);
 *           return;
 *       }
 *
 *       if (kernel_start > region_base) {
 *           // 内核区间前面还有一段空闲，先提交这一段
 *           kalloc_add_region(region_base, kernel_start - region_base);
 *       }
 *
 *       if (kernel_end < region_end) {
 *           // 内核区间后面还有一段空闲，再提交这一段
 *           kalloc_add_region(kernel_end, region_end - kernel_end);
 *       }
 *
 *       // 如果内核区间把这段区域整个盖住，两个 if 都不执行，
 *       // 函数直接返回——这是预期行为，不是遗漏。
 *   }
 */

struct mb2_tag_header {
    uint32_t type;
    uint32_t size;
};

/* memory map tag 自己的头，紧跟在通用 tag header 后面：entry_size 是
 * 每条记录的字节数（规范允许将来扩展这个大小，读的时候必须用这个字段
 * 算偏移，不能硬编码 sizeof(struct mb2_mmap_entry)）。entry_version
 * 目前恒为 0。 */
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
    /* mb2_info_addr 是 boot.S 从 %ebx 转交过来的物理地址。此刻内核还在
     * 恒等映射/低地址运行，物理地址可以直接当指针解引用。 */
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_addr;
    uint32_t total_size = *(const uint32_t *)base;

    uint64_t regions_found = 0;

    /* 第一个 tag 紧跟在 total_size(u32) + reserved(u32) 之后，即偏移 8。 */
    uint64_t offset = 8;
    while (offset < total_size) {
        const struct mb2_tag_header *hdr =
            (const struct mb2_tag_header *)(base + offset);

        if (hdr->type == MB2_TAG_END) {
            break;
        }

        if (hdr->type == MB2_TAG_MMAP) {
            const struct mb2_mmap_tag *mmap_tag =
                (const struct mb2_mmap_tag *)hdr;

            uint64_t entry_offset = offset + sizeof(struct mb2_mmap_tag);
            uint64_t entries_end = offset + mmap_tag->size;

            while (entry_offset < entries_end) {
                const struct mb2_mmap_entry *entry =
                    (const struct mb2_mmap_entry *)(base + entry_offset);

                if (entry->type == MB2_MEMORY_AVAIL) {
                    add_region_excluding_kernel_image(entry->base_addr, entry->length);
                    regions_found++;
                }

                entry_offset += mmap_tag->entry_size;
            }
        }

        offset += align_up8(hdr->size);
    }

    kprintf("memmap: %lu available region(s) from Multiboot2 mmap tag, "
            "%lu page(s) free\n",
            regions_found, kalloc_free_pages());

    if (regions_found == 0) {
        panic("memmap: no available memory region found in Multiboot2 info");
    }
}
