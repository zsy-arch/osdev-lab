/* 解析 GRUB 传进来的 Multiboot2 boot information 结构，找到内存映射
 * （memory map）tag，把每一段"可用 RAM"喂给 kalloc——Lab4 起，喂之前先
 * 排除掉内核镜像自己占用的物理范围（见下面 add_region_excluding_kernel_image()
 * 的注释），Lab3 阶段没有这一步，因为那会还没有页表分配器会去踩这段内存。
 *
 * Multiboot2 boot information 的整体形状（GNU Multiboot2 规范"3.4 Boot
 * information format"一节）：
 *
 *   +-------------------+
 *   | total_size (u32)  |  整个结构体的字节数，包括这个头本身
 *   | reserved   (u32)  |  规范保留字段，恒为 0，读的时候不用管
 *   +-------------------+
 *   | tag 0             |  每个 tag 都以 8 字节对齐开始
 *   | tag 1             |
 *   | ...               |
 *   | end tag           |  type=0, size=8，标志列表结束
 *   +-------------------+
 *
 * 每个 tag 自己的头是 { type (u32), size (u32) }，size 包含这个头的
 * 8 字节。tag 之间用 (size + 7) & ~7 计算下一个 tag 的偏移——tag 的
 * 有效内容不需要凑够 8 字节的整数倍，但下一个 tag 的起始地址必须
 * 8 字节对齐，这个向上取整就是补上 padding 的量。
 *
 * 本 Lab 只关心 type=6（memory map）这一种 tag，其它 tag（比如
 * type=2 的 boot loader name、type=4 的 basic memory info）直接跳过。
 * GRUB 默认就会带 memory map tag，不需要在 Multiboot2 header 里显式用
 * "information request" tag 去要——这是实测确认的：Lab3 的 Multiboot2
 * header（boot.S 里的 multiboot_header）没有加任何 information request
 * tag，跑起来 mmap tag 照样存在，说明它属于 GRUB "默认就给"的一类信息，
 * 不属于"你不要我就不给"的可选信息。
 */
#include "types.h"
#include "console.h"
#include "kalloc.h"
#include "panic.h"

#define MB2_TAG_END      0
#define MB2_TAG_MMAP     6
#define MB2_MEMORY_AVAIL 1

/* Lab4 新增：GRUB 的 Multiboot2 mmap tag 描述的是"物理内存上有哪些段
 * 是 RAM"，完全不知道内核自己已经占用了其中一段（内核镜像本来就加载
 * 在某一段可用 RAM 中间，不是单独一块 GRUB 会排除掉的区域）。Lab3 那会
 * 直接把每个 MB2_MEMORY_AVAIL 段整个丢给 kalloc_add_region()，能跑是
 * 因为 Lab3 自己也没往这段内存里写页表——Lab4 一开分页，
 * pagetable_create()/pagetable_map() 走 kalloc_page() 要新页表节点，
 * 如果 free pool 里混进了内核镜像自己占的物理页，分配出来的"空闲页"
 * 可能其实是内核的 .text/.data/.bss，写下去直接损坏正在跑的内核。
 *
 * __boot_load_addr/__kernel_phys_end 是 linker.ld 定义的链接期符号，
 * 分别对应内核镜像物理起止（[__boot_load_addr, __kernel_phys_end)）——
 * __boot_load_addr 就定义在 `. = KERNEL_LOAD_ADDR` 之后、任何 section
 * 内容之前（linker.ld 里 .text.boot 前面那一行），VMA=LMA=物理低地址,
 * 用法和 __kernel_phys_end 一样直接当指针取地址即是物理地址。
 * memmap_discover() 运行在分页开启之前（低地址、身份映射，见下面
 * 函数头注释），这两个符号当普通指针取地址直接就是物理地址，不需要
 * 额外转换——用法和 riscv64 版本用 __stack_top 一样，只是 x86_64 这边
 * 链接脚本已经准备好了专门给 boot.S 用的 __kernel_phys_end，直接复用。
 *
 * 和 riscv64 的 add_region_excluding_kernel_image() 不同的是，riscv64
 * 侧只需要处理 DTB 报告的单一 /memory 区域，且已知内核镜像的起始地址
 * 落在该区域内部、结束地址也在区域内部，所以只clip尾部就够。这里改成
 * 完整处理"内核占用区间落在某个 mmap 区域中间"的一般情况（起始/结束
 * 都可能需要clip，或者内核区间和某个 mmap 区域完全不相交），因为
 * Multiboot2 mmap 允许报告多段不连续的可用内存，不能像 riscv64 那样
 * 假设只有一段。 */
extern char __boot_load_addr[];
extern char __kernel_phys_end[];

static void add_region_excluding_kernel_image(uint64_t region_base, uint64_t region_len)
{
    uint64_t region_end = region_base + region_len;
    uint64_t kernel_start = (uint64_t)(uintptr_t)__boot_load_addr;
    uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_phys_end;

    if (kernel_end <= region_base || kernel_start >= region_end) {
        /* 内核占用区间和这段 mmap 区域完全不相交，整段照常喂给 kalloc。 */
        kalloc_add_region(region_base, region_len);
        return;
    }

    if (kernel_start > region_base) {
        /* 内核区间的前面还有一段空闲，先喂这一段。 */
        kalloc_add_region(region_base, kernel_start - region_base);
    }

    if (kernel_end < region_end) {
        /* 内核区间的后面还有一段空闲，再喂这一段。 */
        kalloc_add_region(kernel_end, region_end - kernel_end);
    }

    /* 如果 kernel_start <= region_base && kernel_end >= region_end，
     * 说明内核占用区间把这整段 mmap 区域完全盖住了，没有空闲部分可喂，
     * 两个 if 都不会执行，函数直接返回——这是预期行为，不是遗漏。 */
}

struct mb2_tag_header {
    uint32_t type;
    uint32_t size;
};

/* memory map tag 自己的头，紧跟在通用 tag header 后面：entry_size 是
 * 每条记录的字节数（规范允许将来扩展这个大小，读的时候必须用这个字段
 * 算偏移，不能硬编码 sizeof(struct mb2_mmap_entry)——虽然目前所有已知
 * 实现都是 24，教学上仍然按规范要求的方式读，这是"读规范定义的字段，
 * 不是猜一个当前观察到的常量"的具体例子）。entry_version 目前恒为 0。 */
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
    /* mb2_info_addr 是 boot.S 从 %ebx 转交过来的物理地址。Lab3 阶段
     * 内核还在恒等映射/低地址运行（页表把虚拟地址 0 映射到物理地址 0，
     * 见 Lab1/Lab2 的页表填充逻辑），物理地址可以直接当指针解引用，
     * 不需要额外的地址转换——这个前提到引入更复杂的虚拟内存布局的
     * 后续 Lab 会失效，那时候需要显式做物理转虚拟的转换，Lab3 暂时
     * 不处理这个问题。 */
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
        /* GRUB 没有给 mmap tag，或者给了但里面一条可用内存都没有——
         * 两种情况都意味着 kalloc 完全没有内存可用，继续跑下去所有
         * kalloc_pages() 调用都会返回 NULL，不如现在就 panic，报错
         * 位置比后面某个不相关的分配失败点更直接。 */
        panic("memmap: no available memory region found in Multiboot2 info");
    }
}
