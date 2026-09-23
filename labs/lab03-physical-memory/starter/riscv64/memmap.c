/* Lab3 starter (riscv64)：解析 OpenSBI 通过 a1 交给我们的设备树（Devicetree
 * Blob，DTB/FDT），找到 /memory 节点的 reg 属性，把可用 RAM 喂给
 * kalloc_add_region()。
 *
 * 不链接 libfdt——本课程"完全掌控、不依赖隐藏假设"的一贯取向，手写最小
 * FDT 结构块 token walker，只认本 Lab 需要的两种 token（FDT_BEGIN_NODE
 * 找节点名，FDT_PROP 找 reg 属性），其它 token 原样跳过。完整的 FDT
 * 格式说明见 README.md「核心概念」一节，这里只给实现需要的关键点。
 *
 * 大端序（big-endian）是本文件唯一需要手动处理的"意外"：riscv64 本身是
 * 小端 CPU，所有从 DTB 里读出来的多字节字段都要用 be32()/be64_from_cells()
 * 手动转换，直接当 uint32_t/uint64_t 解引用会读出错误的值。
 */
#include "types.h"
#include "console.h"
#include "kalloc.h"
#include "panic.h"

#define FDT_MAGIC       0xd00dfeedU
#define FDT_BEGIN_NODE  0x1U
#define FDT_END_NODE    0x2U
#define FDT_PROP        0x3U
#define FDT_NOP         0x4U
#define FDT_END         0x9U

#define FDT_HDR_OFFSET_OFF_DT_STRUCT  8
#define FDT_HDR_OFFSET_SIZE_DT_STRUCT 36

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64_from_cells(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static uint32_t align_up4(uint32_t v)
{
    return (v + 3) & ~3U;
}

/* 链接脚本定义的符号，标记内核自身占用的物理内存的末尾（代码/数据/
 * BSS/栈全部在 __stack_top 之前，见 linker.ld）。DTB 的 /memory reg
 * 报的是"这块物理内存存在"，不是"这块物理内存空闲"——OpenSBI 固件本身
 * （QEMU virt 上占 0x80000000-0x80200000）和内核自己这两段都不会被
 * DTB 标记出来（见 README.md「核心概念」一节的详细说明和引用的 issue
 * 讨论），任何一段 DTB reg range，如果和 [DTB 报的 base, __stack_top)
 * 有重叠，重叠部分必须被排除，不能提交给 kalloc。 */
extern char __stack_top[];

/* TODO 1：实现这个函数，处理三种情况：
 *   1. region_end <= kernel_end 之外的情况已经在下面两个 if 里覆盖，
 *      这里补的是：kernel_end <= region_base（整段在内核占用范围之后，
 *      没有重叠）——原样调 kalloc_add_region(region_base, region_len) 提交。
 *   2. kernel_end >= region_end（内核占用范围整体覆盖/超出这段区域）
 *      ——这段区域完全不可用，直接 return，不提交任何内存。
 *   3. 部分重叠（kernel_end 落在 region_base 和 region_end 之间）——
 *      只有 [kernel_end, region_end) 是空闲的，调
 *      kalloc_add_region(kernel_end, region_end - kernel_end)。
 *
 * 提示：
 *   uint64_t region_end = region_base + region_len;
 *   uint64_t kernel_end = (uint64_t)(uintptr_t)__stack_top;
 *   if (kernel_end <= region_base) { ... }
 *   if (kernel_end >= region_end) { ... }
 *   kalloc_add_region(kernel_end, region_end - kernel_end);
 */
static void add_region_excluding_kernel_image(uint64_t region_base, uint64_t region_len)
{
}

/* strcmp 的极简替代：本 Lab 只需要判断节点名是不是 "memory" 或者以
 * "memory@" 开头（DTB 惯例：有单元地址的节点名是 "<名字>@<地址>"，QEMU
 * virt 生成的内存节点通常叫 "memory@80000000" 之类）。
 *
 * TODO 2：实现这个函数。逐字符比较 name 和 "memory" 的前 6 个字符，
 * 全部相同之后还要检查 name 第 7 个字符是字符串结束符 '\0' 或者 '@'
 * （避免误配 "memory-controller" 之类前缀相同但语义不同的节点名）。
 *
 * 提示：
 *   static const char prefix[] = "memory";
 *   int i = 0;
 *   while (prefix[i] != '\0') {
 *       if (name[i] != prefix[i]) {
 *           return false;
 *       }
 *       i++;
 *   }
 *   return name[i] == '\0' || name[i] == '@';
 */
static bool node_name_is_memory(const char *name)
{
    (void)name;
    return false;
}

void memmap_discover(uint64_t dtb_paddr)
{
    /* TODO 3：把 dtb_paddr 当指针用（本 Lab 阶段身份映射，物理地址可以
     * 直接解引用），读开头 4 字节校验 magic == FDT_MAGIC，不对就 panic
     * （说明 a1 根本没指向一个合法的 FDT，继续往下解析只会读到垃圾数据）。
     *
     * 提示：
     *   const uint8_t *img = (const uint8_t *)(uintptr_t)dtb_paddr;
     *   uint32_t magic = be32(img);
     *   if (magic != FDT_MAGIC) {
     *       panic("memmap: DTB magic mismatch, a1 does not point at a valid FDT");
     *   }
     */


    /* TODO 4：读文件头里的 off_dt_struct（偏移 FDT_HDR_OFFSET_OFF_DT_STRUCT）
     * 和 size_dt_struct（偏移 FDT_HDR_OFFSET_SIZE_DT_STRUCT），算出结构块
     * 指针 struct_block = img + off_dt_struct。声明 uint32_t offset = 0
     * 作为结构块内部的遍历游标，int depth = 0，bool in_memory_node = false，
     * uint64_t regions_found = 0。
     */


    /* TODO 5：写主循环 while (offset < size_dt_struct)，每轮先读 4 字节
     * token（be32(struct_block + offset)），offset += 4，然后按 token
     * 类型分支处理：
     *
     * FDT_BEGIN_NODE：紧跟着一个 NUL 结尾的节点名字符串，读出
     *   name_len（逐字节找 NUL），offset += align_up4(name_len + 1)
     *   （name + NUL 一起补齐到 4 字节）。depth++ 之后，
     *
     *   ★ 关键点，容易想错的地方 ★：DTB 的根节点（名字是空字符串）
     *   本身就是第一个被 FDT_BEGIN_NODE 打开的节点，depth 从 0 加到 1
     *   的时候对应的是根节点自己，不是"设备树顶层节点"。/memory 作为
     *   根节点的直接子节点，实际落在 depth == 2，不是 depth == 1。
     *   如果这里判断条件写成 depth == 1，in_memory_node 永远不会被
     *   置真，最后会报"没找到任何区域"然后 panic——如果遇到这个情况，
     *   用 `qemu-system-riscv64 -machine virt -bios default -machine
     *   dumpdtb=/tmp/qemu-virt.dtb` 导出真实 DTB，写个小脚本手动走一遍
     *   token 流确认实际的嵌套层级，比对着规范猜测更快定位。
     *
     *   depth == 2 时，in_memory_node = node_name_is_memory(name)。
     *
     * FDT_END_NODE：没有额外数据。depth == 2 时先把 in_memory_node 置回
     *   false，再 depth--。
     *
     * FDT_PROP：紧跟着 { len (u32), nameoff (u32), 数据 }，读出 len 和
     *   data 指针（struct_block + offset + 8），offset += 8 + align_up4(len)。
     *   如果 in_memory_node 且 len >= 16 且 len % 16 == 0，说明这是一组
     *   或多组 reg 数据（每组 16 字节：addr_hi/addr_lo/size_hi/size_lo
     *   四个大端 u32），按 16 字节一组循环，每组用 be64_from_cells()
     *   算出 region_base/region_len，调用
     *   add_region_excluding_kernel_image(region_base, region_len)，
     *   regions_found++。
     *
     * FDT_NOP：无额外数据，不需要额外前进 offset。
     *
     * FDT_END：break 跳出循环。
     *
     * 其它未知 token：调用 panic()——说明偏移算错了，继续走只会读到
     *   更多垃圾数据。
     */


    /* TODO 6：循环结束后打印找到的区域数和当前空闲页数，格式必须和
     * tests/expect-riscv64.txt 逐字匹配：
     *
     *   kprintf("memmap: %lu region(s) from DTB /memory, %lu page(s) free "
     *           "(kernel image excluded)\n",
     *           regions_found, kalloc_free_pages());
     *
     * regions_found == 0 时调用 panic("memmap: no /memory node with a
     * usable reg property found in DTB")。
     */
}
