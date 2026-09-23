/* Lab4 starter (x86_64)：4 级页表（PML4 -> PDPT -> PD -> PT),4KiB 叶子页。
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
 * canonical address 规则——CPU 硬件本身只实现了 48 位虚拟地址,
 * bit 48-63 必须是 bit 47 的复制,否则访问该地址直接 #GP,不会走到
 * 分页翻译那一步。
 *
 * 每一级页表项（PML4E/PDPTE/PDE/PTE)的低 12 位是标志位,高位是下一级
 * 页表（或叶子页,如果这一级就是叶子)的物理地址,天然 4KiB 对齐所以
 * 低 12 位腾出来放标志位:
 *
 *   bit 0  P   Present,1=这一项有效
 *   bit 1  R/W 1=可写,0=只读
 *   bit 2  U/S 1=用户态可访问,0=只有特权级 0 能访问
 *   bit 63 NX  1=不可执行（需要 EFER.NXE 置位才生效)
 *
 * 本 Lab 不用大页,4KiB 叶子只出现在 PT 这一级。完整背景见
 * README.md「核心概念」一节。
 */
#include "types.h"
#include "kalloc.h"
#include "pagetable.h"
#include "panic.h"

#define PTE_P   (1ull << 0)
#define PTE_RW  (1ull << 1)
#define PTE_US  (1ull << 2)
#define PTE_NX  (1ull << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* 9 位一组,从 vaddr 里抠出某一级的索引。已经写好,不是 TODO。 */
static inline uint32_t pte_index(uintptr_t vaddr, uint32_t shift)
{
    return (uint32_t)((vaddr >> shift) & 0x1FF);
}

/* table_ptr()：root/中间层页表节点的地址一律以*物理地址*的形式存储和
 * 传递,但 walk() 要直接解引用它们——这只有在"物理地址本身就是可以
 * 直接当指针用的地址"这个前提下才成立。pagetable_activate() 切换到
 * 新页表之后,这个前提不再自动满足：新页表只装了内核自身镜像的自映射
 * （VA=PA+KERNEL_VIRT_BASE),没有任何身份映射条目——物理地址不再等于
 * 任何合法虚拟地址。这里统一通过 KERNEL_VIRT_BASE+物理地址 这个别名去
 * 访问表节点,而不是物理地址本身——这个别名在切换前后都合法（切换前靠
 * boot.S 阶段 A/B 的临时身份映射,切换后靠 kernel_main.c 里的自映射
 * 循环，两处必须配合，具体覆盖范围见 kernel_main.c 对应注释）。
 * 已经写好，不是 TODO——这是一个不容易organically 发现的系统性
 * 陷阱（riscv64 版本在开发过程中已经在 QEMU 下实测触发过一次），
 * 完整背景见 README.md「常见坑与排查」一节，这里直接给出修复后的
 * 版本，让你专注在页表本身的教学内容上。 */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull

static inline uint64_t *table_ptr(uintptr_t phys)
{
    return (uint64_t *)(phys + KERNEL_VIRT_BASE);
}

/* TODO 1：把架构无关的 PTE_FLAG_* 翻译成 x86_64 实际的硬件位。
 *
 * 注意可执行/不可执行是反过来的：flags 里"没设 PTE_FLAG_EXECUTABLE"
 * 要翻译成"设 NX 位"（不可执行是 x86_64 里需要显式声明的,这是历史
 * 包袱——NX 是后来才加的位,默认语义是"可执行"）。
 *
 * 提示：
 *   static uint64_t translate_flags(uint32_t flags)
 *   {
 *       uint64_t bits = PTE_P;
 *       if (flags & PTE_FLAG_WRITABLE) {
 *           bits |= PTE_RW;
 *       }
 *       if (flags & PTE_FLAG_USER) {
 *           bits |= PTE_US;
 *       }
 *       if (!(flags & PTE_FLAG_EXECUTABLE)) {
 *           bits |= PTE_NX;
 *       }
 *       return bits;
 *   }
 */


/* TODO 2：walk() —— 走到 vaddr 对应的 PT（第 4 级)里那一项的指针。
 *
 * alloc_missing 控制"中间某一级页表项还不存在时怎么办"：true
 * （pagetable_map 传)就现场 kalloc_page() 一个新节点、清零、挂上去；
 * false（pagetable_lookup 传)就直接返回 NULL,表示"这段地址目前没有
 * 映射,不要凑合创建一个"——查询操作不应该有"顺手把页表建起来"这种
 * 副作用。
 *
 * 中间层（PML4E/PDPTE/PDE)新建页表节点时用的标志位固定是 P|RW|US
 * （不管最终叶子权限是什么都开着写权限)：x86_64 的权限检查是"一路
 * 各级取 AND"，真正收紧权限的是最后一级 PT 的叶子项,中间层放宽反而
 * 是正确做法。
 *
 * PML4 -> PDPT -> PD 这三级结构完全一样,可以用循环处理；只有最后一级
 * （PD 里的项指向 PT)处理完之后,函数返回的是 PT 本身的基址,不是 PT
 * 里某一项——调用者（map/lookup)自己再算 PT 内的索引、直接读写那一项。
 *
 * 提示（循环从 shift=39 开始,每轮减 9,直到 shift=12 之前停下——正好
 * 走完 PML4/PDPT/PD 三级,PT 这一级交给调用者处理）：
 *   static uint64_t *walk(uintptr_t root, uintptr_t vaddr, bool alloc_missing)
 *   {
 *       uintptr_t table_phys = root;
 *
 *       for (uint32_t shift = 39; shift > 12; shift -= 9) {
 *           uint64_t *table = table_ptr(table_phys);
 *           uint32_t idx = pte_index(vaddr, shift);
 *           uint64_t entry = table[idx];
 *
 *           if (!(entry & PTE_P)) {
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
 *               entry = (new_table & PTE_ADDR_MASK) | PTE_P | PTE_RW | PTE_US;
 *               table[idx] = entry;
 *           }
 *
 *           table_phys = entry & PTE_ADDR_MASK;
 *       }
 *
 *       return table_ptr(table_phys);
 *   }
 */


/* TODO 3：pagetable_map() —— 调 walk(root, vaddr, true) 拿到 PT 基址,
 * 算出 vaddr 在 PT 里的索引（pte_index(vaddr, 12)),把 paddr 和翻译
 * 后的 flags 写进那一项。
 *
 * void pagetable_map(uintptr_t root, uintptr_t vaddr, uintptr_t paddr,
 *                     uint32_t flags)
 * {
 *     ...
 * }
 */


/* TODO 4：pagetable_lookup() —— 调 walk(root, vaddr, false) 拿到 PT
 * 基址（如果返回 NULL,说明这段地址没有映射,直接返回 0)，算出 PT 里
 * 的索引,读出那一项,检查 PTE_P 位（没设就是"没映射",返回 0),否则
 * 返回 entry & PTE_ADDR_MASK。
 *
 * uintptr_t pagetable_lookup(uintptr_t root, uintptr_t vaddr)
 * {
 *     ...
 * }
 */


/* TODO 5：pagetable_create() —— kalloc_page() 一页当 PML4 根,清零
 * 512 项（全零 = 全部 Present=0,即"空页表"),返回它的物理地址。
 *
 * uintptr_t pagetable_create(void)
 * {
 *     ...
 * }
 */


/* TODO 6：pagetable_activate() —— 把 root 写进 CR3。
 *
 * x86_64 从 Lab2 起分页就已经是开着的（长模式本身要求分页硬件必须
 * 打开),所以这里不是"第一次开分页",是"换根"：只要新表里当前 PC
 * 所在的地址有有效映射,`mov root, %cr3` 这一条指令执行完,下一条
 * 指令照样能正常取指,不会有任何"中间状态"的问题——x86_64 换 CR3
 * 是原子的。
 *
 * `mov %reg, %cr3` 本身就会隐式刷新整个 TLB（除了标了 Global 位的
 * 项,本课程不用 Global 位)——这是 x86_64 架构规定的行为,不需要
 * 额外的 invlpg 序列。
 *
 * void pagetable_activate(uintptr_t root)
 * {
 *     __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
 * }
 */
