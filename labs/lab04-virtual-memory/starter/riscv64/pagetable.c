/* Lab4 starter (riscv64)：Sv39 页表（3 级),4KiB 叶子页。
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
 * 分界线不同：Sv39 是 bit 38,bit 38 及以上必须全 0 或全 1,否则 CPU
 * 直接判定这是一个非法地址,不会进入分页翻译流程。
 *
 * riscv64 的 PTE 格式和 x86_64 完全不同：物理地址整个右移了 2 位再
 * 放进去（riscv64 的 PPN 字段单位是"4KiB 页帧号",不是"字节地址"):
 *
 *   bit 0  V   Valid,1=这一项有效
 *   bit 1  R   可读
 *   bit 2  W   可写
 *   bit 3  X   可执行
 *   bit 4  U   用户态可访问
 *   bit 10-53  PPN（物理页帧号,不是字节地址,要 <<12 才是字节地址)
 *
 * 关键区别：riscv64 用 R/W/X 三个独立位区分"这一项是指向下一级页表
 * 还是一个叶子页"——R=W=X=0 表示"这是个指向下一级页表的指针",R/W/X
 * 任意一个非 0 表示"这就是叶子页,翻译到此为止"。x86_64 靠"页表层级
 * +PS 位"区分,riscv64 靠"RWX 是否全零"——中间层节点必须 R=W=X=0,如果
 * 不小心把中间层的 RWX 也设了,CPU 会把这一项当叶子处理,翻译提前
 * 终止,读出来的物理地址是错的。完整背景见 README.md「核心概念」
 * 一节。
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

/* table_ptr()：root/中间层页表节点的地址一律以*物理地址*的形式存储和
 * 传递,但 walk() 要直接解引用它们——这只有在"物理地址本身就是可以
 * 直接当指针用的地址"这个前提下才成立。pagetable_activate() 切换到
 * 新页表之后,这个前提不再自动满足——这里统一通过
 * KERNEL_VIRT_BASE+物理地址 这个别名去访问表节点,而不是物理地址本身
 * （这个别名在切换前后都合法：切换前靠 boot.S 阶段 A 的临时身份映射,
 * 切换后靠 kernel_main.c 里的自映射循环，两处必须配合）。已经写好，
 * 不是 TODO——这是一个不容易 organically 发现的系统性陷阱（本 Lab
 * 开发过程中已经在 QEMU 下实测触发过：pagetable_activate() 之后第一次
 * 调 pagetable_lookup() 就卡进 load_page_fault），完整背景见
 * README.md「常见坑与排查」一节，这里直接给出修复后的版本，让你专注
 * 在页表本身的教学内容上。 */
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull

static inline uint64_t *table_ptr(uintptr_t phys)
{
    return (uint64_t *)(phys + KERNEL_VIRT_BASE);
}

/* 9 位一组,从 vaddr 里抠出某一级的索引。已经写好,不是 TODO。 */
static inline uint32_t pte_index(uintptr_t vaddr, uint32_t shift)
{
    return (uint32_t)((vaddr >> shift) & 0x1FF);
}

/* TODO 1：物理地址 <-> PTE 里的 PPN 字段的转换,互为反操作。
 *
 * 地址转 PTE 要先右移 12 位（拿到"4KiB 页帧号"),再左移 10 位（PPN
 * 字段从 bit 10 开始);PTE 转地址反过来,先右移 10 位再左移 12 位。
 *
 * 提示：
 *   static inline uint64_t paddr_to_ppn_bits(uintptr_t paddr)
 *   {
 *       return (paddr >> 12) << PTE_PPN_SHIFT;
 *   }
 *
 *   static inline uintptr_t ppn_bits_to_paddr(uint64_t pte)
 *   {
 *       return (uintptr_t)((pte >> PTE_PPN_SHIFT) << 12);
 *   }
 */


/* TODO 2：把架构无关的 PTE_FLAG_* 翻译成 riscv64 实际的硬件位。
 *
 * 和 x86_64 版本对比着看：riscv64 这边"不设 PTE_FLAG_EXECUTABLE 就
 * 不设 X 位"是直觉顺着来的,不像 x86_64 那边 NX 是"反过来"的历史
 * 包袱。R 位这里恒置（本 Lab 不区分"可执行不可读"这种特殊场景）。
 *
 * 提示：
 *   static uint64_t translate_flags(uint32_t flags)
 *   {
 *       uint64_t bits = PTE_V | PTE_R;
 *       if (flags & PTE_FLAG_WRITABLE) {
 *           bits |= PTE_W;
 *       }
 *       if (flags & PTE_FLAG_EXECUTABLE) {
 *           bits |= PTE_X;
 *       }
 *       if (flags & PTE_FLAG_USER) {
 *           bits |= PTE_U;
 *       }
 *       return bits;
 *   }
 */


/* TODO 3：walk() —— 走到 vaddr 对应的 L0（叶子级)里那一项的指针。
 *
 * 和 x86_64 版本的 walk() 同一个设计：alloc_missing 控制缺失中间层
 * 时是否现场分配。Sv39 只有 3 级,循环只走 2 级（L2、L1),第 3 级
 * （L0,也就是最终的叶子表)由调用者（map/lookup)自己处理最后一步。
 *
 * 中间层新建节点时的标志位只有 PTE_V（R=W=X=0)——这是本文件模块注释
 * 里强调的"中间层必须 RWX 全零,否则会被当成叶子"。
 *
 * 提示（循环从 shift=30 开始,每轮减 9,直到 shift=12 之前停下——正好
 * 走完 L2/L1 两级,L0 这一级交给调用者处理）：
 *   static uint64_t *walk(uintptr_t root, uintptr_t vaddr, bool alloc_missing)
 *   {
 *       uintptr_t table_phys = root;
 *
 *       for (uint32_t shift = 30; shift > 12; shift -= 9) {
 *           uint64_t *table = table_ptr(table_phys);
 *           uint32_t idx = pte_index(vaddr, shift);
 *           uint64_t entry = table[idx];
 *
 *           if (!(entry & PTE_V)) {
 *               if (!alloc_missing) {
 *                   return NULL;
 *               }
 *               uintptr_t new_table = (uintptr_t)kalloc_page();
 *               if (new_table == 0) {
 *                   panic("pagetable_map: kalloc_page() failed while allocating an intermediate page table node");
 *               }
 *               uint64_t *new_table_ptr = table_ptr(new_table);
 *               for (uint32_t i = 0; i < 512; i++) {
 *                   new_table_ptr[i] = 0;
 *               }
 *               entry = paddr_to_ppn_bits(new_table) | PTE_V;
 *               table[idx] = entry;
 *           }
 *
 *           table_phys = ppn_bits_to_paddr(entry);
 *       }
 *
 *       return table_ptr(table_phys);
 *   }
 */


/* TODO 4：pagetable_map() —— 调 walk(root, vaddr, true) 拿到 L0 基址,
 * 算出 vaddr 在 L0 里的索引（pte_index(vaddr, 12)),把 paddr（转成
 * PPN 字段)和翻译后的 flags 写进那一项。
 *
 * void pagetable_map(uintptr_t root, uintptr_t vaddr, uintptr_t paddr,
 *                     uint32_t flags)
 * {
 *     ...
 * }
 */


/* TODO 5：pagetable_lookup() —— 调 walk(root, vaddr, false) 拿到 L0
 * 基址（如果返回 NULL,说明这段地址没有映射,直接返回 0)，算出 L0 里
 * 的索引,读出那一项,检查 PTE_V 位（没设就是"没映射",返回 0),否则
 * 把 PPN 字段转回物理地址返回。
 *
 * uintptr_t pagetable_lookup(uintptr_t root, uintptr_t vaddr)
 * {
 *     ...
 * }
 */


/* TODO 6：pagetable_create() —— kalloc_page() 一页当根页表,清零
 * 512 项（全零 = 全部 Valid=0,即"空页表"),返回它的物理地址。
 *
 * uintptr_t pagetable_create(void)
 * {
 *     ...
 * }
 */


/* TODO 7：pagetable_activate() —— 装 satp 寄存器,打开 Sv39 分页,
 * 然后 sfence.vma 刷 TLB。
 *
 * 和 x86_64 版本不同,这里是*第一次*真正打开分页硬件：riscv64 从
 * Lab1 起一直是 satp=0（bare mode)。
 *
 * satp 寄存器的格式（Sv39 模式):
 *   bit 63-60  MODE,8 表示 Sv39（0 表示 bare/关闭分页)
 *   bit 43-0   PPN,根页表的物理页帧号（物理地址 >>12)
 *
 * 关键点：写 satp 本身*不会*自动刷 TLB（这是和 x86_64 CR3 的关键
 * 差异,RISC-V 特权架构手册明确要求软件在改变地址翻译相关状态后自己
 * 执行 sfence.vma,不能假设硬件会自动处理）——漏了这一步不会立刻
 * 出错,但会读到过期的翻译结果,是移植 x86 背景开发者最容易漏掉的
 * 一步。不带操作数的 `sfence.vma zero, zero` 会刷新所有地址空间的
 * 所有映射（本课程还没有 ASID 概念,现在用最简单粗暴的"全刷"是正确
 * 且足够的）。
 *
 * 提示：
 *   void pagetable_activate(uintptr_t root)
 *   {
 *       uint64_t satp_value = (8ull << 60) | (root >> 12);
 *       __asm__ volatile(
 *           "csrw satp, %0\n"
 *           "sfence.vma zero, zero\n"
 *           :
 *           : "r"(satp_value)
 *           : "memory");
 *   }
 */
