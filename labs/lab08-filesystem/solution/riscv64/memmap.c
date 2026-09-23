/* Lab3 riscv64：解析 OpenSBI 通过 a1 交给我们的设备树（Devicetree Blob，
 * DTB/FDT），找到 /memory 节点的 reg 属性，把可用 RAM 喂给
 * kalloc_add_region()。
 *
 * 不链接 libfdt——本课程"完全掌控、不依赖隐藏假设"的一贯取向（types.h
 * 不用宿主 <stdint.h> 是同一个理由），手写最小 FDT 结构块 token walker，
 * 只认本 Lab 需要的两种 token（FDT_BEGIN_NODE 找节点名，FDT_PROP 找
 * reg 属性），其它 token 原样跳过。
 *
 * FDT 二进制格式（Devicetree Specification "5.2 Header" + "5.4
 * Structure Block"）：
 *
 *   文件头 struct fdt_header，全部字段是大端 u32：
 *     magic (必须是 0xd00dfeed)
 *     totalsize
 *     off_dt_struct   -- 结构块相对文件头的字节偏移
 *     off_dt_strings  -- 字符串块相对文件头的字节偏移
 *     off_mem_rsvmap
 *     version
 *     last_comp_version
 *     boot_cpuid_phys
 *     size_dt_strings
 *     size_dt_struct
 *
 *   结构块由 token 序列构成，每个 token 是一个大端 u32：
 *     FDT_BEGIN_NODE (0x1) : 后面紧跟一个 NUL 结尾的节点名字符串，
 *                            整个 "token + 字符串 + NUL" 补齐到 4 字节。
 *     FDT_END_NODE   (0x2) : 没有额外数据。
 *     FDT_PROP       (0x3) : 后面跟 { len (u32), nameoff (u32), 数据 }，
 *                            len 是数据的字节数，nameoff 是属性名在字符串
 *                            块里的偏移；数据本身补齐到 4 字节。
 *     FDT_NOP        (0x4) : 没有额外数据，纯粹跳过。
 *     FDT_END        (0x9) : 结构块结束。
 *
 * 大端序（big-endian）是本文件唯一需要手动处理的"意外"：riscv64 本身
 * 是小端 CPU，所有从 DTB 里读出来的多字节字段都要手动交换字节序，
 * 直接当 uint32_t/uint64_t 解引用会读出错的值——这不是理论提醒，是
 * FDT 规范明确写的"structure block 里所有字段都是大端"，如果这里漏做
 * 字节序转换，读出来的 totalsize/reg 之类字段会是一个巨大的错误数字，
 * 表现成"解析出来的内存范围完全不对/直接越界"。
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

/* fdt_header 里除了 off_dt_struct/size_dt_struct 之外的字段（totalsize/
 * off_dt_strings/off_mem_rsvmap/version/last_comp_version/
 * boot_cpuid_phys/size_dt_strings）本 Lab 一个都用不上——不读字符串块
 * （见下面 node_name_is_memory 那处注释：靠节点名而不是属性名过滤），
 * 不处理内存预留映射表（本 Lab 唯一关心的"哪段内存不能用"是内核自己
 * 占的范围，走链接脚本符号 __stack_top，不走 off_mem_rsvmap）。为了不
 * 声明一个十个字段、八个字段永远不会被写入的 struct，这里就不建
 * struct fdt_header 了，只取需要的两个字段对应的具体数值。 */
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
 * 报的是"这块物理内存存在"，不是"这块物理内存空闲"——OpenSBI 固件
 * 本身（QEMU virt 上占 0x80000000-0x80200000，这也是内核链接地址
 * 选在 0x80200000、紧接在 OpenSBI 后面的原因）和内核自己这两段都在
 * "存在"但不"空闲"的范围内，DTB 不会为它们打 reserved-memory 标记
 * （riscv-software-src/opensbi 项目的 issue 讨论里确认过这一点：这块
 * 内存被固件用掉了，但设备树里完全没有反映）。任何一段 DTB reg range，
 * 如果和 [DTB 报的 base, __stack_top) 有重叠，重叠部分必须被排除，
 * 不能提交给 kalloc——这个函数就是做这件事的，本文件里发现的每一段
 * DTB reg range 都要过一遍这里，不能直接调 kalloc_add_region()。 */
extern char __stack_top[];

static void add_region_excluding_kernel_image(uint64_t region_base, uint64_t region_len)
{
    uint64_t region_end = region_base + region_len;
    uint64_t kernel_end = (uint64_t)(uintptr_t)__stack_top;

    if (kernel_end <= region_base) {
        /* 这段区域整体在内核占用范围之后，没有重叠，原样提交。 */
        kalloc_add_region(region_base, region_len);
        return;
    }
    if (kernel_end >= region_end) {
        /* 内核占用范围整体覆盖/超出这段区域，这段区域完全不可用。
         * QEMU virt 只有一段 /memory range，实测就是这个情况：DTB 报的
         * range 从 0x80000000 开始，内核链接地址 0x80200000 落在这段
         * range 内部，报出来的一整段几乎全部需要走下面"部分重叠"分支，
         * 不是这里——这个分支处理的是"内核占用范围比这段 DTB range
         * 还大"的情况，教学上仍然要覆盖，避免以后 QEMU 内存配置变化时
         * 悄悄越界。 */
        return;
    }
    /* 部分重叠：[region_base, kernel_end) 被内核占用，
     * [kernel_end, region_end) 才是真正空闲的部分。 */
    kalloc_add_region(kernel_end, region_end - kernel_end);
}

/* strcmp 的极简替代：src/common/string.c 里没有导出 strcmp（Lab0 到现在
 * 一直没有用到字符串比较的场景），本 Lab 只需要判断节点名是不是
 * "memory" 或者以 "memory@" 开头（DTB 惯例：有单元地址的节点名是
 * "<名字>@<地址>"，QEMU virt 生成的内存节点通常叫 "memory@80000000" 之
 * 类），不追求通用字符串比较，就地写一个够用的检查，不新增共享接口。 */
static bool node_name_is_memory(const char *name)
{
    static const char prefix[] = "memory";
    int i = 0;
    while (prefix[i] != '\0') {
        if (name[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    /* "memory" 后面必须是字符串结束或者 '@'，避免误配 "memory-controller"
     * 之类前缀相同但语义不同的节点名（QEMU virt 目前不会生成这种节点，
     * 但检查逻辑本身不应该依赖"目前不会"这个偶然事实）。 */
    return name[i] == '\0' || name[i] == '@';
}

void memmap_discover(uint64_t dtb_paddr)
{
    /* 恒等映射前提下物理地址可以直接当指针用，跟 x86_64 那份 memmap.c
     * 开头的注释是同一个理由，这里不重复。 */
    const uint8_t *img = (const uint8_t *)(uintptr_t)dtb_paddr;

    uint32_t magic = be32(img);
    if (magic != FDT_MAGIC) {
        panic("memmap: DTB magic mismatch, a1 does not point at a valid FDT");
    }

    uint32_t off_dt_struct = be32(img + FDT_HDR_OFFSET_OFF_DT_STRUCT);
    uint32_t size_dt_struct = be32(img + FDT_HDR_OFFSET_SIZE_DT_STRUCT);

    const uint8_t *struct_block = img + off_dt_struct;
    uint32_t offset = 0;

    /* depth 记录当前嵌套在第几层节点内部，用来判断"下一个 FDT_PROP
     * 属于哪个节点"。root 节点（"/"）本身在 FDT_BEGIN_NODE 时节点名是
     * 空字符串，是第一个被打开的节点，depth 从 0 加到 1——所以 root
     * 自己占了 depth 1，root 的直接子节点（/memory、/cpus、/soc 之类）
     * 落在 depth 2，而 /memory 正是 root 的直接子节点，不是 root 本身。
     * 这一点最初想错了：以为"depth 1 就是设备树顶层节点"，实测 dump
     * QEMU 生成的真实 DTB（用 `qemu-system-riscv64 -machine
     * dumpdtb=file.dtb` 转出来手动解析）才发现 root 自己先占了一层，
     * /memory 是第二层——如果按 depth==1 判断，in_memory_node 永远不会
     * 被置真，memmap_discover() 会把"没找到任何区域"误判成"这块板子
     * 真的没内存"，直接 panic，而不是"解析逻辑的层数算错了"。
     * in_memory_node 记录"当前深度 2 的节点是不是我们要找的 /memory"，
     * 只在这个节点内部才去看 reg 属性。 */
    int depth = 0;
    bool in_memory_node = false;
    uint64_t regions_found = 0;

    while (offset < size_dt_struct) {
        uint32_t token = be32(struct_block + offset);
        offset += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)(struct_block + offset);
            uint32_t name_len = 0;
            while (name[name_len] != '\0') {
                name_len++;
            }
            offset += align_up4(name_len + 1);

            depth++;
            if (depth == 2) {
                in_memory_node = node_name_is_memory(name);
            }
        } else if (token == FDT_END_NODE) {
            if (depth == 2) {
                in_memory_node = false;
            }
            depth--;
        } else if (token == FDT_PROP) {
            uint32_t len = be32(struct_block + offset);
            uint32_t nameoff = be32(struct_block + offset + 4);
            const uint8_t *data = struct_block + offset + 8;
            (void)nameoff; /* 属性名字符串在 strings 块里，本 Lab 靠节点名
                             * 而不是属性名做过滤，不需要读字符串块——
                             * 只要是 in_memory_node 内部叫 reg 长度对得上
                             * 的属性就处理，这里没有校验 nameoff 指向的
                             * 字符串真的是 "reg"，教学取向的简化：QEMU
                             * virt 生成的 /memory 节点只有一个多字节长度
                             * 的属性会是 reg（其它属性如 device_type 是
                             * 字符串"memory"，长度模式不同），实际跑起来
                             * 分辨得开，不追求处理"理论上属性名恰好撞车"
                             * 的边界情况。 */

            if (in_memory_node && len >= 16 && (len % 16) == 0) {
                /* QEMU virt 的根节点 #address-cells/#size-cells 都是 2
                 * （64 位地址、64 位长度），所以 reg 的每一组是 4 个
                 * 32 位大端 cell = 16 字节：<addr_hi addr_lo size_hi
                 * size_lo>。len 可能包含多组（理论上一个内存节点可以
                 * 报多段不连续的范围），按 16 字节一组循环处理全部。 */
                uint32_t group_count = len / 16;
                for (uint32_t g = 0; g < group_count; g++) {
                    const uint8_t *group = data + g * 16;
                    uint64_t region_base = be64_from_cells(group);
                    uint64_t region_len = be64_from_cells(group + 8);
                    add_region_excluding_kernel_image(region_base, region_len);
                    regions_found++;
                }
            }

            offset += 8 + align_up4(len);
        } else if (token == FDT_NOP) {
            /* 无额外数据，offset 已经跳过 token 本身，不需要再前进。 */
        } else if (token == FDT_END) {
            break;
        } else {
            /* 未知 token：说明偏移算错了或者 DTB 版本超出本课程处理
             * 范围，继续往下走只会读到更多垃圾数据，不如直接 panic
             * 报出具体 token 值，比后面某个内存分配失败更好定位。 */
            panic("memmap: unrecognized FDT structure token, offset arithmetic likely wrong");
        }
    }

    kprintf("memmap: %lu region(s) from DTB /memory, %lu page(s) free "
            "(kernel image excluded)\n",
            regions_found, kalloc_free_pages());

    if (regions_found == 0) {
        panic("memmap: no /memory node with a usable reg property found in DTB");
    }
}
