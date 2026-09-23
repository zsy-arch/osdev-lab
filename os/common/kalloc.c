/* 链表式空闲物理页分配器的实现，见 kalloc.h 顶部的设计说明。
 *
 * 空闲链表按地址从低到高排序，这个不变量贯穿整个实现：
 *   - kalloc_add_region() 插入新区域时按地址找位置插入，不是直接头插。
 *   - kalloc_pages() 从某个 run 切出前面一段时，剩余部分的地址变大了
 *     （原地址+切出的字节数），但仍然小于链表里下一个节点的地址，
 *     大于前一个节点的地址——排序不变量继续成立，不需要重新定位。
 *   - kfree_pages() 释放时按地址找回插入点，然后检查前后邻居能不能
 *     合并——这一步是本 Lab 唯一依赖"按地址排序"这个不变量的地方：
 *     如果链表不按地址排序，"前后邻居"就没有意义，合并判断没法做。
 */
#include "kalloc.h"

typedef struct free_run {
    struct free_run *next;
    size_t num_pages;
} free_run_t;

static free_run_t *free_list_head;

/* 见 kalloc.h 里 kalloc_set_phys_to_virt_offset() 的注释——默认 0,
 * 表示"物理地址本身就能直接当指针用"（Lab3 恒等映射阶段、以及 Lab4/5
 * 分页开启前那一段代码的实际情况)。free_list_head 本身也是一个
 * "解引用物理地址"的指针,但它只在 kalloc_pages()/kfree_pages() 内部
 * 走 phys_to_ptr() 转换后使用（见下面 phys_to_ptr 的调用点),这里存的
 * 值仍然是物理地址,和 kalloc_page() 对外的返回值约定一致。 */
static uint64_t phys_to_virt_offset;

void kalloc_set_phys_to_virt_offset(uint64_t offset)
{
    phys_to_virt_offset = offset;
}

/* free_run_t 链表节点的唯一解引用入口——本文件内任何地方要把一个
 * "物理地址"转成"free_run_t*"去读/写 next、num_pages,都必须经过这
 * 个函数,不能直接 (free_run_t *)phys 强转。这样 kalloc_set_phys_to_
 * virt_offset() 只需要在一处生效,不用在 kalloc_add_region/kalloc_pages/
 * kfree_pages 三处分别记住"这里也要加偏移量"。 */
static inline free_run_t *phys_to_ptr(uint64_t phys)
{
    return (free_run_t *)(uintptr_t)(phys + phys_to_virt_offset);
}

static uint64_t align_up(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

static uint64_t align_down(uint64_t v, uint64_t align)
{
    return v & ~(align - 1);
}

void kalloc_add_region(uint64_t base, uint64_t len)
{
    uint64_t start = align_up(base, PAGE_SIZE);
    uint64_t end = align_down(base + len, PAGE_SIZE);

    /* base+len 本身可能比 base 还小（len 异常）或者取整后 end <= start
     * （区域太小，不够放一页）——这两种情况教学取向直接丢弃，不当错误处理，
     * 见 kalloc.h 里对应的注释。 */
    if (end <= start) {
        return;
    }

    /* run_phys 是这个节点的物理地址,存进链表（free_list_head、prev、
     * run->next 这些字段里)的一律是这个物理地址值本身,不是转换后的
     * 指针——整个链表的"地址排序"不变量、kfree_pages 里的邻居合并
     * 判断,都是拿物理地址直接比较大小,和 phys_to_virt_offset 无关
     * （offset 只影响"怎么解引用",不影响"存的是什么值")。要读写这个
     * 节点自己的 next/num_pages 字段,才需要 phys_to_ptr() 转换一次。 */
    uint64_t run_phys = start;
    phys_to_ptr(run_phys)->num_pages = (end - start) / PAGE_SIZE;

    /* 按地址找插入位置：prev 停在第一个地址 >= start 的节点之前。
     * *prev 里存的是"前一个节点的 next 字段"当前的值,也就是物理地址
     * （或者 free_list_head 本身),类型仍然声明成 free_run_t* 只是为了
     * 复用指针语法写"取地址"（&(*prev)->next),从来不会真的对 *prev
     * 做解引用去读 next/num_pages——每次要读 next 字段本身的值,都是
     * 先转成 free_run_t* 再 ->next,读到的还是下一个物理地址,不是解
     * 引用下一个物理地址。 */
    free_run_t **prev = &free_list_head;
    while (*prev != NULL && (uintptr_t)*prev < start) {
        prev = &phys_to_ptr((uint64_t)(uintptr_t)*prev)->next;
    }
    phys_to_ptr(run_phys)->next = *prev;
    *prev = (free_run_t *)(uintptr_t)run_phys;
}

void *kalloc_pages(size_t num_pages)
{
    if (num_pages == 0) {
        return NULL;
    }

    /* run/prev 里存的都是物理地址（或者指向物理地址的字段),声明成
     * free_run_t* 只是复用指针语法,真正要读 run 自己的 next/num_pages
     * 字段时一律先过 phys_to_ptr()——道理和 kalloc_add_region() 里
     * 那段注释一样,这里不重复。 */
    free_run_t **prev = &free_list_head;
    free_run_t *run = free_list_head;

    while (run != NULL) {
        uint64_t run_phys = (uint64_t)(uintptr_t)run;
        free_run_t *run_ptr = phys_to_ptr(run_phys);

        if (run_ptr->num_pages >= num_pages) {
            if (run_ptr->num_pages == num_pages) {
                /* 整段刚好用完，直接从链表摘掉。 */
                *prev = run_ptr->next;
            } else {
                /* 从 run 前面切出 num_pages 页交出去，剩余部分往后挪
                 * num_pages*PAGE_SIZE 字节，元数据在新地址原地重写。
                 * 剩余部分的地址比原来大、仍然比链表下一个节点小
                 * （run 本来就在正确的排序位置上，只是往它自己的区间里
                 * 收缩），排序不变量不需要额外调整。 */
                uint64_t remainder_phys = run_phys + num_pages * PAGE_SIZE;
                free_run_t *remainder_ptr = phys_to_ptr(remainder_phys);
                remainder_ptr->num_pages = run_ptr->num_pages - num_pages;
                remainder_ptr->next = run_ptr->next;
                *prev = (free_run_t *)(uintptr_t)remainder_phys;
            }
            return (void *)(uintptr_t)run_phys;
        }
        prev = &run_ptr->next;
        run = run_ptr->next;
    }

    return NULL;
}

void kfree_pages(void *addr, size_t num_pages)
{
    if (addr == NULL || num_pages == 0) {
        return;
    }

    uint64_t start = (uint64_t)(uintptr_t)addr;
    uint64_t end = start + num_pages * PAGE_SIZE;

    free_run_t **prev = &free_list_head;
    while (*prev != NULL && (uintptr_t)*prev < start) {
        prev = &phys_to_ptr((uint64_t)(uintptr_t)*prev)->next;
    }
    uint64_t next_phys = (uint64_t)(uintptr_t)*prev;
    bool has_next = (*prev != NULL);

    /* 先检查能不能跟后一个空闲 run 合并：如果释放的这段正好紧贴着
     * next 的起始地址，把 next 的页数并进来，用 next 原来的位置继续
     * 表示合并后的整段（不需要移动，起始地址就是 start）。 */
    uint64_t merged_phys = start;
    free_run_t *merged_ptr = phys_to_ptr(merged_phys);
    size_t merged_pages = num_pages;
    uint64_t merged_next_phys = next_phys;
    bool merged_has_next = has_next;

    if (has_next && end == next_phys) {
        free_run_t *next_ptr = phys_to_ptr(next_phys);
        merged_pages += next_ptr->num_pages;
        merged_next_phys = (uint64_t)(uintptr_t)next_ptr->next;
        merged_has_next = (next_ptr->next != NULL);
    }

    merged_ptr->num_pages = merged_pages;
    merged_ptr->next = merged_has_next ? (free_run_t *)(uintptr_t)merged_next_phys : NULL;
    *prev = (free_run_t *)(uintptr_t)merged_phys;

    /* 再检查能不能跟前一个空闲 run 合并：如果 *prev 现在的地址前面
     * 紧挨着一个更早插入的 run，把刚插入的这段吞并到那个更早的 run
     * 里——这一步要重新从头走一次链表找"指向 merged 的那个指针"的
     * 前一个节点，因为上面那段逻辑操作的是局部变量 prev/next，不是
     * 链表里紧邻 merged 的真实前驱。 */
    free_run_t **scan = &free_list_head;
    while (*scan != NULL && (uint64_t)(uintptr_t)*scan != merged_phys) {
        uint64_t candidate_phys = (uint64_t)(uintptr_t)*scan;
        free_run_t *candidate_ptr = phys_to_ptr(candidate_phys);
        if (candidate_phys + candidate_ptr->num_pages * PAGE_SIZE == merged_phys) {
            candidate_ptr->num_pages += merged_ptr->num_pages;
            candidate_ptr->next = merged_ptr->next;
            return;
        }
        scan = &candidate_ptr->next;
    }
}

void *kalloc_page(void)
{
    return kalloc_pages(1);
}

void kfree_page(void *addr)
{
    kfree_pages(addr, 1);
}

size_t kalloc_free_pages(void)
{
    size_t total = 0;
    for (free_run_t *run = free_list_head; run != NULL;) {
        free_run_t *run_ptr = phys_to_ptr((uint64_t)(uintptr_t)run);
        total += run_ptr->num_pages;
        run = run_ptr->next;
    }
    return total;
}
