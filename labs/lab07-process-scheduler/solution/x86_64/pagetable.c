/* Lab4 x86_64：4 级页表（PML4 -> PDPT -> PD -> PT),4KiB 叶子页。
 *
 * x86_64 的虚拟地址翻译（分页硬件打开、且不使用大页时)分成 4 级,每级
 * 9 位索引（2^9=512 项/表,每项 8 字节,正好一张表一页 4KiB):
 *
 *   63          48 47    39 38    30 29    21 20    12 11         0
 *   +-------------+--------+--------+--------+--------+-----------+
 *   | sign-extend | PML4 idx| PDPT idx| PD idx | PT idx | page off |
 *   +-------------+--------+--------+--------+--------+-----------+
 *        16 位        9 位      9 位     9 位      9 位      12 位
 *
 * bit 47 及以上必须全 0 或全 1（"sign-extend"),这是 x86_64 的
 * canonical address 规则,不是本课程的假设——CPU 硬件本身只实现了
 * 48 位虚拟地址,bit 48-63 必须是 bit 47 的复制,否则访问该地址直接
 * #GP,不会走到分页翻译那一步。Lab4 选的内核虚拟基址
 * 0xFFFFFFFF80000000,bit 47 是 1,符合规则,对应 PML4 index 511
 * （全 1 的高 16 位 + PML4 idx=511,算出来正好是这个值——见下面
 * KERNEL_PML4_INDEX 的注释)。
 *
 * 每一级页表项（PML4E/PDPTE/PDE/PTE)的低 12 位是标志位,高位（bit 12
 * 起,直到实现的物理地址宽度)是下一级页表（或叶子页,如果这一级就是
 * 叶子)的物理地址,天然 4KiB 对齐所以低 12 位腾出来放标志位:
 *
 *   bit 0  P   Present,1=这一项有效
 *   bit 1  R/W 1=可写,0=只读
 *   bit 2  U/S 1=用户态可访问,0=只有特权级 0 能访问
 *   bit 5  A   Accessed,CPU 访问过这一项对应的地址会自动置 1
 *   bit 6  D   Dirty（只有叶子页表项有意义),CPU 写过会自动置 1
 *   bit 63 NX  1=不可执行（需要 EFER.NXE 置位才生效,否则这一位
 *              被硬件忽略,当成永远可执行)
 *
 * 本 Lab 不用大页（PS 位,PD/PDPT 项的 bit 7),4KiB 叶子只出现在 PT
 * 这一级——教学取向：先把"4 级都得走一遍"这个机制讲清楚,大页作为
 * 挑战任务留给学生,见 README。
 */
#include "types.h"
#include "kalloc.h"
#include "pagetable.h"
#include "panic.h"

#define PTE_P   (1ull << 0)
#define PTE_RW  (1ull << 1)
#define PTE_US  (1ull << 2)
#define PTE_NX  (1ull << 63)

/* 页表项里物理地址部分的掩码：bit 12-51（本课程假设 4 级页表覆盖的
 * 52 位物理地址宽度足够,不处理 5 级页表/更宽物理地址的情况——真实
 * 硬件用 CPUID 探测支持的物理地址宽度,教学简化为固定掩码)。 */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* 9 位一组,从 vaddr 里抠出某一级的索引。SHIFT 是这一级索引在虚拟
 * 地址里的起始位——PML4 是 39（12+9+9+9),PDPT 是 30,PD 是 21,
 * PT 是 12,跟本文件顶部那张位域表一一对应。 */
static inline uint32_t pte_index(uintptr_t vaddr, uint32_t shift)
{
    return (uint32_t)((vaddr >> shift) & 0x1FF);
}

/* 和 kernel_main.c 里同名常量必须保持一致，但不能 #include 它（那是
 * .c 文件,没有对应的头)——只能各自定义一份。为什么 walk() 需要这个：
 * root/中间层页表节点的地址一律以*物理地址*的形式存储和传递（见
 * pagetable.h 对 root 参数的注释),walk() 却要直接解引用它们
 * （`table[idx]`、`((uint64_t *)new_table)[i] = 0`)——这只有在"物理
 * 地址本身就是可以直接当指针用的地址"这个前提下才成立。
 *
 * pagetable_activate() 切换到新页表之后,这个前提不再自动满足：新页表
 * 只装了内核自身镜像的自映射（VA=PA+KERNEL_VIRT_BASE),没有任何身份
 * 映射条目——物理地址不再等于任何合法虚拟地址。kalloc_page() 分配的
 * 页表节点物理地址落在内核镜像范围之外（紧跟在 kernel_end_phys 后面,
 * 是 kalloc 空闲池的起始处),如果 walk() 还在拿这个物理地址直接当指针
 * 解引用,访问到的其实是"如果分页硬件把这串比特当成虚拟地址,会翻译到
 * 哪里去"这个几乎肯定不对的地方,不是巧合触发的——riscv64 版本
 * （pagetable.c 同一个位置)已经在 QEMU 下实测触发过这个 bug：
 * pagetable_activate() 之后第一次调 pagetable_lookup()（对应 walk()
 * 内部第一次解引用 root)直接卡进 page fault,tval 落在 kalloc 空闲池
 * 的头几页。x86_64 这边是同一个 walk() 设计（本文件顶部模块注释也
 * 明确写了"和 x86_64 版本的 walk() 同一个设计"是反过来抄的关系),
 * 同一个漏洞成立,不是"riscv64 特有、x86_64 侥幸躲过"。
 *
 * 修复方式：walk() 统一通过 KERNEL_VIRT_BASE+物理地址 这个别名去访问
 * 表节点,而不是物理地址本身。这个别名在切换前后都合法：切换前,
 * boot.S 阶段 A/B 建的临时表把低 1GiB 身份映射整段（不只是内核镜像
 * 本身占用的部分)映射成可读写,足够覆盖 kalloc 早期分配的页表节点；
 * 切换后,需要 kernel_main.c 里的自映射循环同样覆盖到 kernel_end_phys
 * 往后一段范围（不能只覆盖到 kernel_end_phys 为止),两处必须配合,单改
 * 一处不够——这个函数只负责"怎么解引用",不负责"这段地址有没有被
 * 映射"，后者是调用方（kernel_boot())的责任，具体覆盖范围见
 * kernel_main.c 里对应的注释。 */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull

static inline uint64_t *table_ptr(uintptr_t phys)
{
    return (uint64_t *)(phys + KERNEL_VIRT_BASE);
}

/* 把架构无关的 PTE_FLAG_* 翻译成 x86_64 实际的硬件位。PTE_FLAG_USER
 * 本 Lab 不用（还没有用户态),但翻译逻辑已经写好,Lab6 引入用户态时
 * 这里不用改。注意可执行/不可执行是反过来的：flags 里"没设
 * PTE_FLAG_EXECUTABLE"要翻译成"设 NX 位"（不可执行是 x86_64 里
 * 需要显式声明的),这是两个架构里少数几个"设置意图相反"的地方,
 * riscv64 那边（pagetable.c 的对应函数)是"没设 EXECUTABLE 就不设
 * X 位",直觉上更顺,x86_64 这个历史包袱（NX 是后来才加的位,默认
 * 语义是"可执行",不可执行必须显式声明)值得在这里点破。 */
static uint64_t translate_flags(uint32_t flags)
{
    uint64_t bits = PTE_P;
    if (flags & PTE_FLAG_WRITABLE) {
        bits |= PTE_RW;
    }
    if (flags & PTE_FLAG_USER) {
        bits |= PTE_US;
    }
    if (!(flags & PTE_FLAG_EXECUTABLE)) {
        bits |= PTE_NX;
    }
    return bits;
}

/* 走到 vaddr 对应的 PT（第 4 级)里那一项的指针。alloc_missing 控制
 * "中间某一级页表项还不存在时怎么办"：true（pagetable_map 传)就现场
 * kalloc_page() 一个新节点、清零、挂上去；false（pagetable_lookup 传)
 * 就直接返回 NULL,表示"这段地址目前没有映射,不要凑合创建一个"——
 * 查询操作不应该有"顺手把页表建起来"这种副作用。
 *
 * 中间层（PML4E/PDPTE/PDE)新建页表节点时用的标志位固定是 P|RW|US
 * （不管最终叶子权限是什么都开着写权限)：x86_64 的权限检查是"一路
 * 各级取 AND"，真正收紧权限的是最后一级 PT 的叶子项,中间层放宽
 * 反而是正确做法——如果中间层也照抄叶子的只读标志,会出现"PDE 只读
 * 导致这个 PDE 覆盖的所有页,即使各自的 PTE 标了可写,合成结果也会
 * 变成只读"的诡异 bug（不是这里的 bug,是提醒：这是常见的写反了的坑,
 * 中间层应该总是开着,收紧交给最后一级)。 */
static uint64_t *walk(uintptr_t root, uintptr_t vaddr, bool alloc_missing)
{
    uintptr_t table_phys = root;

    /* PML4 -> PDPT -> PD 这三级结构完全一样,循环处理；只有最后一级
     * （PD 里的项指向 PT)处理完之后,函数返回的是 PT 本身的基址,不是
     * PT 里某一项——调用者（map/lookup)自己再算 PT 内的索引、直接读写
     * 那一项,不需要 walk() 返回"指向某一项的指针",因为 map 和 lookup
     * 对最后一级项的处理方式不同（map 要写入,lookup 只读),没必要在
     * walk() 里强行统一。 */
    for (uint32_t shift = 39; shift > 12; shift -= 9) {
        uint64_t *table = table_ptr(table_phys);
        uint32_t idx = pte_index(vaddr, shift);
        uint64_t entry = table[idx];

        if (!(entry & PTE_P)) {
            if (!alloc_missing) {
                return NULL;
            }
            uintptr_t new_table = (uintptr_t)kalloc_page();
            if (new_table == 0) {
                panic("pagetable_map: kalloc_page() failed while allocating an intermediate page table node");
            }
            uint64_t *new_table_ptr = table_ptr(new_table);
            for (uint32_t i = 0; i < 512; i++) {
                new_table_ptr[i] = 0;
            }
            entry = (new_table & PTE_ADDR_MASK) | PTE_P | PTE_RW | PTE_US;
            table[idx] = entry;
        }

        table_phys = entry & PTE_ADDR_MASK;
    }

    return table_ptr(table_phys);
}

void pagetable_map(uintptr_t root, uintptr_t vaddr, uintptr_t paddr,
                    uint32_t flags)
{
    uint64_t *pt = walk(root, vaddr, true);
    uint32_t pt_idx = pte_index(vaddr, 12);

    pt[pt_idx] = (paddr & PTE_ADDR_MASK) | translate_flags(flags);
}

uintptr_t pagetable_lookup(uintptr_t root, uintptr_t vaddr)
{
    uint64_t *pt = walk(root, vaddr, false);
    if (pt == NULL) {
        return 0;
    }

    uint32_t pt_idx = pte_index(vaddr, 12);
    uint64_t entry = pt[pt_idx];
    if (!(entry & PTE_P)) {
        return 0;
    }

    return (uintptr_t)(entry & PTE_ADDR_MASK);
}

/* pagetable_unmap()：跟 pagetable_lookup() 几乎一样的 walk（同样传
 * alloc_missing=false,不会现场创建中间层节点),区别只是查到叶子项之后
 * 不是读它,而是把它清零（PTE_P 都不再置位,整项归零——不只是清 P 位,
 * 避免残留的地址位/其它标志位被误当成"看起来还有效"的数据),再把清零
 * *之前*那个 entry 里的物理地址部分作为返回值交给调用者。 */
uintptr_t pagetable_unmap(uintptr_t root, uintptr_t vaddr)
{
    uint64_t *pt = walk(root, vaddr, false);
    if (pt == NULL) {
        return 0;
    }

    uint32_t pt_idx = pte_index(vaddr, 12);
    uint64_t entry = pt[pt_idx];
    if (!(entry & PTE_P)) {
        return 0;
    }

    pt[pt_idx] = 0;
    return (uintptr_t)(entry & PTE_ADDR_MASK);
}

uintptr_t pagetable_create(void)
{
    uintptr_t root = (uintptr_t)kalloc_page();
    if (root == 0) {
        panic("pagetable_create: kalloc_page() failed");
    }
    uint64_t *root_ptr = table_ptr(root);
    for (uint32_t i = 0; i < 512; i++) {
        root_ptr[i] = 0;
    }
    return root;
}

/* x86_64 从 Lab2 起分页就已经是开着的（长模式本身要求分页硬件必须
 * 打开,boot.S 里 CR4.PAE + CR3 + EFER.LME + CR0.PG 那一串就是在干
 * 这件事,建的是一份临时的、只做低 1GiB 身份映射的表)。所以这里不是
 * "第一次开分页",是"换根"：把 CR3 指向 Lab4 新建的这份表——只要新表
 * 里当前 PC 所在的地址（不管是低地址过渡代码,还是已经跳到高地址执行
 * 的内核主体)有有效映射,`mov root, %cr3` 这一条指令执行完,下一条
 * 指令照样能正常取指,不会有任何"中间状态"或者"缺一步"的问题——x86_64
 * 换 CR3 是原子的,不存在"切了一半"这种情况。
 *
 * `mov %cr3, %cr3` 本身就会隐式刷新整个 TLB（除了标了 Global 位的
 * 项,本课程不用 Global 位,所以这里不需要额外的 invlpg 序列)——这是
 * x86_64 架构规定的行为,不是本课程的假设。 */
void pagetable_activate(uintptr_t root)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}

/* Lab7 新增。记录"内核自己那份根页表"的物理地址——kernel_boot() 在
 * pagetable_create()+把内核镜像映射好之后,通过这个 setter 告诉本文件
 * "这就是以后每个进程页表都要拷贝内核范围的来源"。不通过参数从
 * pagetable_copy_kernel_range() 传入,是因为调用点（proc_alloc())
 * 不需要、也不应该关心"内核根页表是哪个物理地址"这个纯 kernel_main.c
 * 初始化阶段的细节——调用者只管"给我一个新进程的页表,内核范围帮我
 * 填好"，来源在哪里是本文件内部状态。 */
static uintptr_t g_kernel_root;

void pagetable_set_kernel_root(uintptr_t root)
{
    g_kernel_root = root;
}

/* PML4 index 511 覆盖 [0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFF]——
 * 本课程内核虚拟基址 KERNEL_VIRT_BASE 落在这个范围内（见本文件顶部
 * table_ptr() 旁边的注释),内核镜像本身+kernel_main.c 里那段"自映射
 * 到 kalloc 空闲池"的范围全部落在同一个 PML4 项里（512GiB 的覆盖范围
 * 远大于本课程内核+空闲池的实际大小),所以只需要复制这一项。
 *
 * 复制的是 PML4 *项*本身（8 字节,指向下一级 PDPT 节点的物理地址+
 * 标志位),不是递归深拷贝整棵树——新进程页表和内核页表在这段范围内
 * 共享同一批 PDPT/PD/PT 节点,这是刻意的（本文件顶部 pagetable.h 声明
 * 处的注释已经解释过动机：内核镜像运行时不会再变,共享省去了每个进程
 * 各自复制一整套内核页表节点的开销和空间)。 */
#define KERNEL_PML4_INDEX 511

void pagetable_copy_kernel_range(uintptr_t dst_root)
{
    uint64_t *src = table_ptr(g_kernel_root);
    uint64_t *dst = table_ptr(dst_root);
    dst[KERNEL_PML4_INDEX] = src[KERNEL_PML4_INDEX];
}
