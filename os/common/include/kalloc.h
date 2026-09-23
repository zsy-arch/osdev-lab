/* 架构无关的物理页分配器：链表式空闲页管理，4KiB 粒度。
 *
 * 这是 Lab3 的核心数据结构。它只知道"哪些物理地址范围是空闲的"，完全不
 * 关心这些范围是怎么被发现的——发现可用物理内存的过程是架构专属的
 * （x86_64 解析 Multiboot2 mmap tag，riscv64 解析 DTB 的 /memory 节点，
 * 见各 Lab 的 memmap_<arch>.c），发现完之后统一调用 kalloc_add_region()
 * 喂给这里，这条边界之后的逻辑就和"内存到底是谁告诉我们的"完全无关了。
 *
 * 设计：空闲内存按"连续页的一段（run）"为单位组织成单链表，每个 run 的
 * 元数据（下一个 run 的指针 + 这个 run 有多少页）就存在这段内存自己的
 * 第一页里——空闲页反正没有别的用途，不需要另外找地方存链表节点。
 * 这也是为什么 kalloc_add_region() 传入的每个区域至少要够放一页：
 * 元数据本身需要一页来存。
 *
 * 用 run（一段连续页）而不是单页做链表节点，是因为 ROADMAP 明确要求
 * "分配/释放任意数量的 4KiB 页"——调用者可能一次要 N 个连续页（比如后面
 * Lab 给某个子系统一次性要一块连续物理内存），单页链表没法保证分配出来
 * 的多页是连续的，run 链表天然支持。
 */
#ifndef OSDEV_KALLOC_H
#define OSDEV_KALLOC_H

#include "types.h"

#define PAGE_SIZE 4096u

/* 注册"物理地址 -> 可解引用的虚拟地址"的偏移量,默认 0（恒等映射,
 * 物理地址本身就能直接当指针用)。
 *
 * 背景（Lab6 才第一次暴露、Lab3-5 从未触发的设计缺口）：free_run_t
 * 链表节点的元数据直接存在空闲物理页自己的头几个字节里（见本文件顶部
 * 模块注释),kalloc_add_region/kalloc_pages/kfree_pages 三处都会把
 * "物理地址"直接强转成 free_run_t* 解引用——这在分页开启前、或者
 * "物理地址正好也是身份映射的合法虚拟地址"这两种情况下没问题,but
 * Lab4 起 pagetable_activate() 切换到只有自映射（VA=PA+常量,不是
 * VA=PA)的正式页表之后,物理地址就不再是合法的可解引用地址了。
 *
 * Lab3-5 从未触发这个 bug,是因为它们全部的 kalloc_page()/kalloc_pages()
 * 调用都发生在 pagetable_activate() *之前*（那时低身份映射还在)——
 * Lab6 是第一个在 pagetable_activate() 之后还需要 kalloc_page()
 * 的 Lab（给用户程序页/用户栈页现场分配物理页),第一次真正走到这条
 * 路径,在 QEMU 下实测触发：kalloc_pages() 内部读 run->num_pages 时
 * 直接 #PF,CR2 落在 kalloc 空闲池头部（不是随便什么地址,是可预测
 * 的、free_run_t 节点自己的地址)。
 *
 * 调用方（各 Lab 各架构的 kernel_main.c）在 pagetable_activate() 之后、
 * 第一次可能在此之后调用 kalloc_page()/kalloc_pages() 之前,补一次
 * kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE)——之后 kalloc.c
 * 内部所有对 free_run_t 节点的解引用都会自动加上这个偏移量,不需要
 * 调用方或 kalloc_page() 的返回值做任何改变（返回值仍然是物理地址,
 * 这个约定不变,只是 kalloc.c 自己内部访问链表节点时不再假设"物理
 * 地址=可解引用地址"）。 */
void kalloc_set_phys_to_virt_offset(uint64_t offset);

/* 用物理地址初始化/扩展分配器的空闲池。
 *
 * base 会被向上取整到页边界，(base+len) 会被向下取整到页边界——调用者
 * 给的范围不保证本来就页对齐（Multiboot2/DTB 报出来的范围没有这个义务），
 * 取整之后如果剩下不够一整页（至少要能放一页的 run 元数据），这段区域
 * 直接丢弃，不会当成可用内存记进去，也不会报错——调用方是内存探测代码，
 * 探测阶段发现"这一段太小/没对齐好用不上"不是一个需要中断整个探测流程
 * 的错误，只是这一段可用内存变少了几十字节，教学取向不为这种边界情况
 * 增加返回值/错误处理的复杂度。
 *
 * 每次探测到一段可用内存就调一次，可以多次调用（比如 Multiboot2 mmap
 * 里本来就有好几条 type=1 的记录，各是独立的区域）。 */
void kalloc_add_region(uint64_t base, uint64_t len);

/* 分配 num_pages 个连续的 4KiB 物理页，返回起始地址（页对齐）。
 * num_pages 必须 >= 1。失败（没有足够大的连续空闲 run）返回 NULL——
 * 教学阶段的策略是 first-fit：从链表头开始找第一个够大的 run，不是
 * best-fit，不追求减少碎片，重点是"链表怎么正确地拆分/合并"这个机制
 * 本身讲清楚。 */
void *kalloc_pages(size_t num_pages);

/* 释放一段之前由 kalloc_pages() 返回的连续页。
 * addr 必须是 kalloc_pages() 返回的地址（页对齐），num_pages 必须和
 * 当时申请的数量一致——分配器不会替调用者记住"这块地址当初分配了多大"，
 * 由调用者自己负责传对，这是教学取向的简化（真实内核通常会在分配元数据
 * 里记录大小，本课程刻意不做，让"分配和释放的元数据在哪"这个问题保持
 * 简单，Lab3 的重点是链表本身，不是元数据管理）。
 * 释放时会尝试和链表里相邻的空闲 run 合并成更大的一段，避免长期运行后
 * 空闲内存碎片化成一堆无法满足大分配请求的小块。 */
void kfree_pages(void *addr, size_t num_pages);

/* 单页版本的 kalloc_pages(1)/kfree_pages(_, 1)，最常见的用法，
 * 单独提供只是省得每次调用都写常量 1。 */
void *kalloc_page(void);
void kfree_page(void *addr);

/* 当前空闲页总数，用于测试/调试断言——比如"分配 N 页再全部释放，空闲页
 * 数应该和分配之前完全一样"，这是本 Lab 检验"没有泄漏、没有重复计数"
 * 最直接的方式。不追求性能（真实分配器不会为了一个统计接口去遍历整个
 * 链表），教学取向的正确性检验工具。 */
size_t kalloc_free_pages(void);

#endif /* OSDEV_KALLOC_H */
