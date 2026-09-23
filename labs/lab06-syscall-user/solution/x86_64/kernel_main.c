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

/* 必须跟 user_prog.ld 里的 USER_PROG_VADDR 一致——两份独立的构建产物
 * （内核 vs 用户程序）在这一个数值上必须手工保持同步，这是 Lab6
 * 用户态最小实现里唯一一处"没有单一数据源、需要人肉对齐"的地方，
 * README 会专门指出。 */
#define USER_PROG_VADDR 0x400000ull

/* 用户栈：单独一页，放在用户程序页正上方——中间不留 guard page（真实
 * 内核会留一页不映射来抓栈溢出，本 Lab 的用户程序只有两条 syscall+一个
 * 死循环，教学取向不需要这层保护，指出这个简化本身也是一种教学）。
 * 栈指针初始化到这一页*顶端*（栈从高地址往低地址长，x86_64 调用约定
 * 规定的方向），不是页起始地址。
 *
 * USER_STACK_VADDR 定义成"栈页的顶端地址"，即"栈页起始地址+一整页"，
 * 不是"程序页起始地址+一整页"——这两者看起来只差一个词，但差 PAGE_SIZE：
 * 栈页起始地址本身已经是 USER_PROG_VADDR+PAGE_SIZE（紧跟在程序页
 * 后面），栈页顶端还要再加一个 PAGE_SIZE。之前实现直接写成
 * USER_PROG_VADDR+0x1000，映射时用 USER_STACK_VADDR-PAGE_SIZE 当栈页
 * 起始地址，算出来正好是 USER_PROG_VADDR 本身——把栈页错误地映射到了
 * 程序页那个虚拟地址上，两次 pagetable_map() 调用互相覆盖同一个页表
 * 项，QEMU 里实测触发：lookup(USER_PROG_VADDR) 和 lookup(栈页任意地址)
 * 返回的是同一个物理地址（后一次 pagetable_map 覆盖了前一次的页表项），
 * 用户程序一旦跳过去执行 entry point 所在的那一页，实际访问到的物理
 * 内容其实是清零后的栈页（从来没被 memcpy 写过用户程序字节），CPU
 * 尝试从这段全零内容取指直接 #PF（error code 里 RSVD 位那个奇怪的组合，
 * 起因是把全零字节当成指令译码时的边界情况，不是本 Lab 需要深究的点，
 * 根因是映射覆盖，不是译码逻辑）。 */
#define USER_STACK_VADDR (USER_PROG_VADDR + 0x2000ull)

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

    /* 见 kalloc.h 里 kalloc_set_phys_to_virt_offset() 的完整注释——
     * Lab3-5 从未在 pagetable_activate() 之后调用过 kalloc_page()，
     * 这个偏移量一直保持默认值 0（恒等）也没出过问题；Lab6 是第一个
     * 在这之后还要现场分配用户程序页/用户栈页的 Lab，不补这一步会在
     * kalloc_pages() 内部读 free_run_t 节点时 #PF（实测触发过，CR2
     * 落在 kalloc 空闲池头部的节点地址上）。必须紧跟在 pagetable_
     * activate() 之后、下面第一次 kalloc_page() 调用之前完成。 */
    kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE);

    console_puts_line("switched to page table, low identity map gone");

    idt_init();
    syscall_init();
    pit_init();
    __asm__ volatile("sti");
    console_puts_line("timer armed, syscall entry armed, building user program");

    /* 用户程序页：kalloc_page() 返回物理地址（Lab6 前的会话已经确认过
     * 这一点，见 kalloc.c 的分配逻辑——free-run 指针直接来自
     * memmap_discover 喂进去的原始物理地址，没有做过任何偏移转换）。
     * 拷贝内容前必须先经过内核自己的高半自映射（pa + KERNEL_VIRT_BASE）
     * 才能被 CPU 解引用——分页已经激活，低物理地址身份映射早就撤了
     * （上面 "low identity map gone" 那行印证的正是这件事），直接把
     * 物理地址当指针用会立刻 #PF。 */
    void *user_prog_page_phys = kalloc_page();
    if (user_prog_page_phys == NULL) {
        panic("kernel_boot: failed to allocate user program page");
    }
    uint8_t *user_prog_page_kva =
        (uint8_t *)((uintptr_t)user_prog_page_phys + KERNEL_VIRT_BASE);

    size_t user_prog_len = (size_t)(__user_prog_end - __user_prog_start);
    if (user_prog_len > PAGE_SIZE) {
        panic("kernel_boot: embedded user program does not fit in one page");
    }
    memset(user_prog_page_kva, 0, PAGE_SIZE);
    memcpy(user_prog_page_kva, __user_prog_start, user_prog_len);

    pagetable_map(root, USER_PROG_VADDR, (uintptr_t)user_prog_page_phys,
                  PTE_FLAG_USER | PTE_FLAG_EXECUTABLE);

    /* 用户栈页：内容不需要拷贝任何东西（栈从空白页开始即可），但同样
     * 需要先清零——新分配的物理页内容是上一个使用者留下的垃圾（跟
     * pagetable_create() 内部清零页表根节点是同一个理由），不清零会让
     * 用户程序看到一段不确定的初始栈内容，虽然本 Lab 的用户程序完全
     * 不读栈上的初始值，但依赖"这次凑巧无害"不是本课程的取向。 */
    void *user_stack_page_phys = kalloc_page();
    if (user_stack_page_phys == NULL) {
        panic("kernel_boot: failed to allocate user stack page");
    }
    uint8_t *user_stack_page_kva =
        (uint8_t *)((uintptr_t)user_stack_page_phys + KERNEL_VIRT_BASE);
    memset(user_stack_page_kva, 0, PAGE_SIZE);

    pagetable_map(root, USER_STACK_VADDR - PAGE_SIZE,
                  (uintptr_t)user_stack_page_phys,
                  PTE_FLAG_USER | PTE_FLAG_WRITABLE);

    console_puts_line("user program mapped, entering user mode");

    extern void enter_user_mode(uintptr_t entry, uintptr_t stack);
    enter_user_mode(USER_PROG_VADDR, USER_STACK_VADDR);

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
