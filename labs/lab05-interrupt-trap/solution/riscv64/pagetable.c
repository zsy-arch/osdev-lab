/* Lab4 riscv64：Sv39 页表（3 级),4KiB 叶子页。
 *
 * Sv39 虚拟地址只有 39 位（比 x86_64 的 48 位少一级),分 3 级,每级
 * 同样 9 位索引（2^9=512 项/表,每项 8 字节,一张表一页 4KiB):
 *
 *   63        39 38    30 29    21 20    12 11         0
 *   +-----------+--------+--------+--------+-----------+
 *   |sign-extend| L2 idx | L1 idx | L0 idx | page off  |
 *   +-----------+--------+--------+--------+-----------+
 *      25 位        9 位     9 位     9 位      12 位
 *
 * "sign-extend"规则和 x86_64 的 canonical address 是同一件事,只是
 * 分界线不同：Sv39 是 bit 38（对应 x86_64 是 bit 47),bit 38 及以上
 * 必须全 0 或全 1,否则 CPU 直接判定这是一个非法地址,不会进入分页
 * 翻译流程（RISC-V 特权架构手册对 Sv39 的规定,不是本课程假设)。
 * Lab4 选的内核虚拟基址 0xFFFFFFC000000000,bit 38 是 1,对应顶层
 * 页表 index 511（和 x86_64 PML4[511]同一个位置关系,只是这里的
 * "顶层"只有 3 级里的最外面一级,x86_64 是 4 级里最外面一级)。
 *
 * riscv64 的 PTE 格式和 x86_64 完全不同,不是"标志位在低位、地址在
 * 高位"这么简单的对应,而是把物理地址整个右移了 2 位再放进去
 * （因为 riscv64 的 PPN 字段单位是"4KiB 页帧号",不是"字节地址",
 * 44 位 PPN 左移 12 位就能覆盖 56 位物理地址,腾出来的这 2 位空间用
 * 来把 bit 8-9 让给 RSW（reserved for supervisor software,本课程
 * 不用)):
 *
 *   bit 0  V   Valid,1=这一项有效
 *   bit 1  R   可读
 *   bit 2  W   可写
 *   bit 3  X   可执行
 *   bit 4  U   用户态可访问
 *   bit 5  G   Global（本课程不用)
 *   bit 6  A   Accessed
 *   bit 7  D   Dirty
 *   bit 10-53  PPN（物理页帧号,不是字节地址,要 <<12 才是字节地址)
 *
 * 关键区别：riscv64 用 R/W/X 三个独立位区分"这一项是指向下一级页表
 * 还是一个叶子页"——R=W=X=0 表示"这是个指向下一级页表的指针",R/W/X
 * 任意一个非 0 表示"这就是叶子页,翻译到此为止"。x86_64 靠"页表层级
 * +PS 位"区分,riscv64 靠"RWX 是否全零",本 Lab 只用 4KiB 叶子（3 级
 * 走完才算叶子,不用 Sv39 的大页能力),两种设计在这个前提下效果一样,
 * 但概念上不能混着理解——riscv64 的中间层节点必须 R=W=X=0,如果不小心
 * 把中间层的 RWX 也设了,CPU 会把这一项当叶子处理,翻译提前终止,读出
 * 来的物理地址是错的（指向了本该是下一级页表的那个物理页,当成最终
 * 目标页处理)。
 */
#include "types.h"
#include "kalloc.h"
#include "pagetable.h"
#include "panic.h"

#define PTE_V (1ull << 0)
#define PTE_R (1ull << 1)
#define PTE_W (1ull << 2)
#define PTE_X (1ull << 3)
#define PTE_U (1ull << 4)

#define PTE_PPN_SHIFT 10

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
 * 哪里去"这个几乎肯定不对的地方,不是巧合触发的——实测在 QEMU 下
 * pagetable_activate() 之后第一次调 pagetable_lookup()（对应 walk()
 * 内部第一次解引用 root)就卡进 load_page_fault,tval 正好落在
 * kalloc 空闲池的头几页,用 `-d int,cpu_reset` 抓日志确认过。
 *
 * 修复方式：walk() 统一通过 KERNEL_VIRT_BASE+物理地址 这个别名去访问
 * 表节点,而不是物理地址本身。这个别名在切换前后都合法：切换前,
 * boot.S 阶段 A 的临时页表里 boot_l1_high[1]->boot_leaf 这条链路把
 * [KERNEL_LOAD_ADDR, KERNEL_LOAD_ADDR+2MiB) 整段（不只是内核镜像本身
 * 占用的部分)都映射成 RWX,足够覆盖 kalloc 早期分配的页表节点；切换后,
 * 需要 kernel_main.c 里的自映射循环同样覆盖到这段范围（不能只覆盖到
 * kernel_end_phys),两处必须配合,单改一处不够——这个函数只负责"怎么
 * 解引用",不负责"这段地址有没有被映射"，后者是调用方（kernel_boot())
 * 的责任，具体覆盖范围见 kernel_main.c 里对应的注释。 */
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull

static inline uint64_t *table_ptr(uintptr_t phys)
{
    return (uint64_t *)(phys + KERNEL_VIRT_BASE);
}

static inline uint32_t pte_index(uintptr_t vaddr, uint32_t shift)
{
    return (uint32_t)((vaddr >> shift) & 0x1FF);
}

/* 物理地址 <-> PTE 里的 PPN 字段,互为反操作：地址转 PTE 要 >>12 再
 * <<10（等价于 >>2),PTE 转地址要反过来 >>10 再 <<12（等价于 <<2）。
 * 写成两个独立的小函数而不是在调用点直接位移,是因为"12 和 10 这两个
 * 数字为什么是这样"（4KiB 页内偏移占 12 位,PPN 字段从 bit 10 开始)
 * 只需要在这里解释一次,调用点直接看函数名就知道意图,不需要每次重新
 * 推一遍位移量。 */
static inline uint64_t paddr_to_ppn_bits(uintptr_t paddr)
{
    return (paddr >> 12) << PTE_PPN_SHIFT;
}

static inline uintptr_t ppn_bits_to_paddr(uint64_t pte)
{
    return (uintptr_t)((pte >> PTE_PPN_SHIFT) << 12);
}

/* PTE_FLAG_* 翻译成 riscv64 实际的硬件位。和 x86_64 版本
 * （pagetable.c 里的 translate_flags)对比着看：riscv64 这边"不设
 * PTE_FLAG_EXECUTABLE 就不设 X 位"是直觉顺着来的,不像 x86_64 那边
 * NX 是"反过来"的历史包袱——这是本 Lab README 里"riscv64 现代设计,
 * x86 历史包袱"这条主线在页表权限位上的又一个具体例子。 */
static uint64_t translate_flags(uint32_t flags)
{
    uint64_t bits = PTE_V | PTE_R;
    if (flags & PTE_FLAG_WRITABLE) {
        bits |= PTE_W;
    }
    if (flags & PTE_FLAG_EXECUTABLE) {
        bits |= PTE_X;
    }
    if (flags & PTE_FLAG_USER) {
        bits |= PTE_U;
    }
    return bits;
}

/* 和 x86_64 版本的 walk() 同一个设计：alloc_missing 控制缺失中间层
 * 时是否现场分配。Sv39 只有 3 级,循环走 2 级（L2、L1),第 3 级
 * （L0,也就是最终的叶子表)由调用者（map/lookup)自己处理最后一步。
 *
 * 中间层新建节点时的标志位是 PTE_V 且只有 V 位（R=W=X=0)——这正是
 * 上面模块注释里强调的"中间层必须 RWX 全零,否则会被当成叶子"，和
 * x86_64 版本"中间层开 P|RW|US,靠最后一级收紧权限"的做法不是同一种
 * 思路：riscv64 这里中间层不存在"权限"的概念（RWX 全零就只是个纯粹
 * 的转发,谈不上收紧或放宽),真正的权限只在最后一级叶子项上体现。 */
static uint64_t *walk(uintptr_t root, uintptr_t vaddr, bool alloc_missing)
{
    uintptr_t table_phys = root;

    for (uint32_t shift = 30; shift > 12; shift -= 9) {
        uint64_t *table = table_ptr(table_phys);
        uint32_t idx = pte_index(vaddr, shift);
        uint64_t entry = table[idx];

        if (!(entry & PTE_V)) {
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
            entry = paddr_to_ppn_bits(new_table) | PTE_V;
            table[idx] = entry;
        }

        table_phys = ppn_bits_to_paddr(entry);
    }

    return table_ptr(table_phys);
}

void pagetable_map(uintptr_t root, uintptr_t vaddr, uintptr_t paddr,
                    uint32_t flags)
{
    uint64_t *pt = walk(root, vaddr, true);
    uint32_t pt_idx = pte_index(vaddr, 12);

    pt[pt_idx] = paddr_to_ppn_bits(paddr) | translate_flags(flags);
}

uintptr_t pagetable_lookup(uintptr_t root, uintptr_t vaddr)
{
    uint64_t *pt = walk(root, vaddr, false);
    if (pt == NULL) {
        return 0;
    }

    uint32_t pt_idx = pte_index(vaddr, 12);
    uint64_t entry = pt[pt_idx];
    if (!(entry & PTE_V)) {
        return 0;
    }

    return ppn_bits_to_paddr(entry);
}

uintptr_t pagetable_create(void)
{
    uintptr_t root = (uintptr_t)kalloc_page();
    if (root == 0) {
        panic("pagetable_create: kalloc_page() failed");
    }
    for (uint32_t i = 0; i < 512; i++) {
        ((uint64_t *)root)[i] = 0;
    }
    return root;
}

/* 和 x86_64 版本不同,这里是*第一次*真正打开分页硬件：riscv64 从
 * Lab1 起一直是 satp=0（bare mode,访存地址直接当物理地址用,完全不
 * 经过任何翻译),S 模式在 bare mode 下本身就是合法的、可以一直运行
 * 下去的状态,不像 x86_64 长模式那样"必须开分页才能进入"。
 *
 * satp 寄存器的格式（Sv39 模式):
 *   bit 63-60  MODE,8 表示 Sv39（0 表示 bare/关闭分页)
 *   bit 43-0   PPN,根页表的物理页帧号（物理地址 >>12)
 *
 * `sfence.vma` 是 riscv64 里 x86_64 `mov cr3,cr3`隐式刷 TLB 的等价物,
 * 但不是隐式的——写 satp 本身*不会*自动刷 TLB（这是和 x86_64 CR3 的
 * 关键差异,RISC-V 特权架构手册明确要求软件在改变地址翻译相关状态后
 * 自己执行 sfence.vma,不能假设硬件会自动处理),不加这一条,旧的（可能
 * 是 bare mode 下"直接当物理地址用"这种退化翻译结果,也可能是上一份
 * 页表的翻译结果)缓存条目可能还留在 TLB 里,后续访存会读到过期的翻译
 * 结果——这不是理论风险,是 riscv64 移植 x86 背景的开发者最容易漏掉的
 * 一步,因为在 x86_64 上"忘记刷 TLB"往往不会立刻炸（CR3 写入正好顺带
 * 刷了),换到 riscv64 上同样的疏忽会直接读到脏数据。
 *
 * 不带操作数的 `sfence.vma`（不指定 vaddr/asid)会刷新*所有*地址空间
 * 的*所有*映射的 TLB 缓存——本课程还没有 ASID 概念（Lab6 引入用户态
 * 之后才会需要区分地址空间),现在用最简单粗暴的"全刷"是正确且足够的。 */
void pagetable_activate(uintptr_t root)
{
    uint64_t satp_value = (8ull << 60) | (root >> 12);
    __asm__ volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(satp_value)
        : "memory");
}
