/* Lab6 x86_64：在 Lab5"页表+中断+定时器都齐备"的基础上，加入用户态。
 * 新增的核心动作只有三步：(1) 把嵌入内核镜像的用户程序字节拷贝到一个
 * 新分配的物理页，用 PTE_FLAG_USER 映射进当前页表；(2) 用同样的方式
 * 分配并映射一个用户栈页；(3) 用一条精心构造的 iretq（不是 sysretq——
 * 见下面 enter_user_mode 的注释）跳进用户态，此后的路径完全交给
 * SYSCALL/SYSRET（trap_entry.S 的 syscall_entry）。
 *
 * 关键简化（在 trap.c sys_write 注释里也提到过，这里再强调一次）：
 * 用户程序和内核共用*同一份*页表（同一个 CR3），不是两个独立地址
 * 空间——这是本 Lab 刻意选择的简化，真正的多地址空间/fork 要到后面
 * 的 Lab 才引入。好处是 SYSCALL 进内核、SYSRET 回用户态全程不需要
 * 换 CR3（真实内核如果用户/内核地址空间分离，至少也会通过"内核映射
 * 出现在每个用户页表的高地址部分"来避免这一步，本课程更进一步，
 * 直接就是同一份页表，教学取向不引入 TLB 刷新/地址空间标识符这类
 * 只有多地址空间才需要考虑的复杂度）。
 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "string.h"
#include "pit.h"

void memmap_discover(uint32_t mb2_info_addr);
void idt_init(void);
void gdt_init(void);
void syscall_init(void);

extern char __kernel_end[];
extern char __user_prog_start[];
extern char __user_prog_end[];

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull
#define KERNEL_LOAD_ADDR 0x100000ull

/* 已经写好，不是 TODO：必须跟 user_prog.ld 里的 USER_PROG_VADDR 一致——
 * 两份独立的构建产物（内核 vs 用户程序）在这一个数值上必须手工保持
 * 同步，这是 Lab6 用户态最小实现里唯一一处"没有单一数据源、需要人肉
 * 对齐"的地方。 */
#define USER_PROG_VADDR 0x400000ull

/* TODO 1：定义 USER_STACK_VADDR。
 *
 * 用户栈单独一页，放在用户程序页正上方（中间不留 guard page——真实
 * 内核会留一页不映射来抓栈溢出，本 Lab 的用户程序只有两条 syscall+
 * 一个死循环，教学取向不需要这层保护）。栈指针要初始化到这一页*顶端*
 * （x86_64 调用约定：栈从高地址往低地址长），不是页起始地址。
 *
 * 关键坑：USER_STACK_VADDR 应该定义成"栈页的顶端地址"，即"栈页起始
 * 地址 + 一整页"，不是"程序页起始地址 + 一整页"——这两者看起来只差
 * 一个词，但差 PAGE_SIZE：栈页起始地址本身已经是
 * USER_PROG_VADDR + PAGE_SIZE（紧跟在程序页后面），栈页顶端还要再加
 * 一个 PAGE_SIZE。如果直接写成 USER_PROG_VADDR + 0x1000，映射时用
 * USER_STACK_VADDR - PAGE_SIZE 当栈页起始地址，算出来会正好是
 * USER_PROG_VADDR 本身——把栈页错误地映射到了程序页那个虚拟地址上，
 * 两次 pagetable_map() 调用会互相覆盖同一个页表项。实测触发的现象：
 * lookup(USER_PROG_VADDR) 和 lookup(栈页任意地址) 返回同一个物理
 * 地址（后一次 pagetable_map 覆盖了前一次的页表项），用户程序一旦
 * 跳过去执行 entry point 所在的那一页，实际访问到的物理内容其实是
 * 清零后的栈页（从来没被 memcpy 写过用户程序字节），CPU 尝试从这段
 * 全零内容取指直接 #PF。根因是映射覆盖，不是取指译码逻辑本身的问题。
 *
 * 提示：
 * #define USER_STACK_VADDR (USER_PROG_VADDR + 0x2000ull)
 */

void kernel_boot(uint32_t mb2_info_addr)
{
    console_puts_line("Hello OS from x86_64 (Lab6: syscall & user mode)");

    memmap_discover(mb2_info_addr);

    uintptr_t kernel_end_phys = (uintptr_t)__kernel_end - KERNEL_VIRT_BASE;
    kprintf("kernel image: phys [%p, %p)\n", (uintptr_t)KERNEL_LOAD_ADDR,
            kernel_end_phys);

    uintptr_t root = pagetable_create();

    uintptr_t phys_start = KERNEL_LOAD_ADDR;
    uintptr_t phys_end = kernel_end_phys;
    for (uintptr_t pa = phys_start; pa < phys_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    uintptr_t self_map_end = KERNEL_LOAD_ADDR + 0x200000ull;
    for (uintptr_t pa = phys_end; pa < self_map_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    pagetable_activate(root);
    gdt_init();

    /* TODO 2：调用 kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE)。
     *
     * 见 kalloc.h 里 kalloc_set_phys_to_virt_offset() 的完整注释——
     * Lab3-5 从未在 pagetable_activate() 之后调用过 kalloc_page()，
     * 这个偏移量一直保持默认值 0（恒等）也没出过问题；Lab6 是第一个
     * 在这之后还要现场分配用户程序页/用户栈页的 Lab，不补这一步会在
     * kalloc_pages() 内部读 free_run_t 节点时 #PF（分页已经激活，低
     * 物理地址身份映射早就撤了，kalloc.c 内部还在用裸物理地址当指针
     * 解引用，必须先告诉它"物理地址 + 这个偏移量才是能解引用的虚拟
     * 地址"）。必须紧跟在 pagetable_activate() 之后、下面第一次
     * kalloc_page() 调用之前完成——这是本 Lab 里唯一一处调用顺序错了
     * 会直接 #PF 崩溃、而不是"跑起来但结果不对"的地方。
     *
     * 提示：
     * kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE);
     */

    console_puts_line("switched to page table, low identity map gone");

    idt_init();
    syscall_init();
    pit_init();
    __asm__ volatile("sti");
    console_puts_line("timer armed, syscall entry armed, building user program");

    /* TODO 3：分配、清零、拷贝、映射用户程序页。
     *
     * 步骤（跟用户栈页的处理方式类似，但这里需要拷贝内容，栈页不需要）：
     *   1. kalloc_page() 分配一个物理页，失败要 panic。
     *   2. kalloc_page() 返回的是物理地址，必须先加上
     *      KERNEL_VIRT_BASE 换算成内核能直接解引用的虚拟地址（分页
     *      已经激活，直接把物理地址当指针用会立刻 #PF——"switched to
     *      page table, low identity map gone" 这行印证的正是这件事）。
     *   3. 用 memset 清零整页（避免暴露上一个使用者留下的垃圾内容），
     *      再用 memcpy 把 __user_prog_start..__user_prog_end 之间的
     *      字节（也就是 user_blob.S 用 .incbin 吸进来的用户程序机器码）
     *      拷贝过去。拷贝前检查长度不超过 PAGE_SIZE，超了要 panic。
     *   4. pagetable_map(root, USER_PROG_VADDR, 物理地址,
     *      PTE_FLAG_USER | PTE_FLAG_EXECUTABLE) ——注意这里传的是
     *      *物理*地址（pagetable_map 的第三个参数语义是物理地址，
     *      不是上一步算出来的内核虚拟地址），PTE_FLAG_USER 是本 Lab
     *      新引入的标志位，没有它 U-mode 访问这页会直接 #PF（哪怕
     *      R/W/X 位都对）。
     *
     * 提示：
     * void *user_prog_page_phys = kalloc_page();
     * if (user_prog_page_phys == NULL) {
     *     panic("kernel_boot: failed to allocate user program page");
     * }
     * uint8_t *user_prog_page_kva =
     *     (uint8_t *)((uintptr_t)user_prog_page_phys + KERNEL_VIRT_BASE);
     *
     * size_t user_prog_len = (size_t)(__user_prog_end - __user_prog_start);
     * if (user_prog_len > PAGE_SIZE) {
     *     panic("kernel_boot: embedded user program does not fit in one page");
     * }
     * memset(user_prog_page_kva, 0, PAGE_SIZE);
     * memcpy(user_prog_page_kva, __user_prog_start, user_prog_len);
     *
     * pagetable_map(root, USER_PROG_VADDR, (uintptr_t)user_prog_page_phys,
     *               PTE_FLAG_USER | PTE_FLAG_EXECUTABLE);
     */

    /* TODO 4：分配、清零、映射用户栈页。
     *
     * 跟 TODO 3 几乎一样，但内容不需要拷贝任何东西（栈从空白页开始
     * 即可）——新分配的物理页内容是上一个使用者留下的垃圾（跟
     * pagetable_create() 内部清零页表根节点是同一个理由），仍然要
     * memset 清零，不能依赖"这次凑巧无害"。
     *
     * 映射地址要用 USER_STACK_VADDR - PAGE_SIZE 当栈页的*起始*地址
     * （USER_STACK_VADDR 本身是栈顶，见上面 TODO 1 的坑）。标志位用
     * PTE_FLAG_USER | PTE_FLAG_WRITABLE（栈需要可写，不需要可执行）。
     *
     * 提示：
     * void *user_stack_page_phys = kalloc_page();
     * if (user_stack_page_phys == NULL) {
     *     panic("kernel_boot: failed to allocate user stack page");
     * }
     * uint8_t *user_stack_page_kva =
     *     (uint8_t *)((uintptr_t)user_stack_page_phys + KERNEL_VIRT_BASE);
     * memset(user_stack_page_kva, 0, PAGE_SIZE);
     *
     * pagetable_map(root, USER_STACK_VADDR - PAGE_SIZE,
     *               (uintptr_t)user_stack_page_phys,
     *               PTE_FLAG_USER | PTE_FLAG_WRITABLE);
     */

    console_puts_line("user program mapped, entering user mode");

    /* TODO 5：调用 enter_user_mode 跳进用户态。
     *
     * enter_user_mode 在 trap_entry.S 里实现（本 Lab 的另一个 TODO），
     * 用一条手工构造的 iretq 完成第一次 S→U 特权级切换（为什么第一次
     * 不能用 sysretq，见 trap_entry.S 里 enter_user_mode 的注释）。
     * 这里只需要 extern 声明它的签名，然后传入程序入口地址和栈顶地址。
     *
     * 提示：
     * extern void enter_user_mode(uintptr_t entry, uintptr_t stack);
     * enter_user_mode(USER_PROG_VADDR, USER_STACK_VADDR);
     */

    /* enter_user_mode 不会返回（跳进用户态之后，唯一的回路是 SYSCALL，
     * 那条路径完全在 trap_entry.S/syscall_entry 里处理，不会走回这个
     * 函数）——但用户程序的 sys_exit 只是打印退出码，并不会让 CPU 停
     * 下来，用户态代码在那之后落进它自己的死循环（见 user_prog.S），
     * 内核这边的主循环仍然要继续跑定时器 tick，两者通过时间片的方式
     * "共存"（本 Lab 没有调度器，用户程序占用 CPU 期间内核主循环确实
     * 完全不会运行，只有 hlt 等下一次中断，或者用户程序自己再触发一次
     * SYSCALL 陷入内核时，控制权才会回到内核这边——本 Lab 的用户程序
     * 触发了两次 SYSCALL 就永久卡进自己的死循环，所以内核主循环从这里
     * 往后事实上不会再被执行到,这是本 Lab 教学范围内可以接受的简化,
     * Lab7 的调度器会引入"定时器中断时把 CPU 从用户态抢回来"这个机制）。 */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
