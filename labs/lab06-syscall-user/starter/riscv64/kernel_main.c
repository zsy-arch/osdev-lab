/* Lab6 riscv64：在 Lab5"页表+中断+定时器都齐备"的基础上，加入用户态。
 * 跟 x86_64 版本（../x86_64/kernel_main.c)对着读——整体三步骤是一样的
 * （分配+映射用户程序页、分配+映射用户栈页、跳进用户态),但第三步用的
 * 机制完全不同：x86_64 是 iretq（sysretq 不能用于第一次切换,理由见
 * ../x86_64/trap_entry.S 的 enter_user_mode 注释),riscv64 是 sret
 * （可以直接用于第一次切换——sret 读的全部是软件可写的 CSR,不像
 * sysretq 那样依赖"之前发生过一次 SYSCALL"这个硬件前提,理由见本目录
 * trap_entry.S 的 enter_user_mode 注释)。
 *
 * 关键简化（跟 x86_64 版本相同,再强调一次）：用户程序和内核共用*同一
 * 份*页表（同一个 satp),不是两个独立地址空间——真正的多地址空间要到
 * 后面的 Lab 才引入。 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "string.h"
#include "sbi.h"

void trap_init(void);
void timer_enable(void);

extern char __kernel_end[];
extern char __user_prog_start[];
extern char __user_prog_end[];

#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull
#define KERNEL_LOAD_ADDR 0x80200000ull

/* 已经写好，不是 TODO：必须跟 user_prog.ld 里的 USER_PROG_VADDR 一致
 * ——理由跟 x86_64 版本同一行注释完全一样：两份独立构建产物之间没有
 * 单一数据源，需要人肉保持同步。数值上特意跟 x86_64 版本取成一样
 * （都是 0x400000），纯粹是方便对照阅读，不是架构要求。 */
#define USER_PROG_VADDR 0x400000ull

/* TODO 1：定义 USER_STACK_VADDR。
 *
 * 跟 x86_64 版本一样定义成"栈页顶端地址"，不是"栈页起始地址"，差一个
 * PAGE_SIZE——见 ../x86_64/kernel_main.c 里 USER_STACK_VADDR 那段
 * 注释的完整推导（当时在 x86_64 侧写成 +0x1000 导致两次 pagetable_map
 * 覆盖同一个页表项，QEMU 里实测触发过：两次映射写到了同一个虚拟地址，
 * 后一次覆盖前一次，用户程序跳过去执行时实际访问到的是清零后的栈页
 * 内容，CPU 尝试从全零内容取指直接 page fault）。riscv64 这边直接
 * 采用已经验证过的正确公式，不重复踩同一个坑。
 *
 * 提示：
 * #define USER_STACK_VADDR (USER_PROG_VADDR + 0x2000ull)
 */

static uint64_t read_time(void)
{
    uint64_t value;
    __asm__ volatile("csrr %0, time" : "=r"(value));
    return value;
}

void kernel_boot(uint64_t hartid, uint64_t dtb_paddr)
{
    (void)hartid;
    (void)dtb_paddr;

    console_puts_line("Hello OS from riscv64 (Lab6: syscall & user mode)");

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

    /* 已经写好，不是 TODO：UART MMIO 必须保留这条映射，否则一旦切到
     * 新页表，console_putc 立刻从能工作变成对着一个没映射的地址写
     * -> page fault（Lab4/5 已经验证过这条映射的必要性，这里原样
     * 保留，不是新增）。 */
    pagetable_map(root, 0x10000000ull, 0x10000000ull, PTE_FLAG_WRITABLE);

    pagetable_activate(root);

    /* TODO 2：调用 kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE)。
     *
     * 见 kalloc.h 里 kalloc_set_phys_to_virt_offset() 的完整注释，也见
     * x86_64 版本 kernel_main.c 同一处注释——Lab3-5 从未在
     * pagetable_activate() 之后调用过 kalloc_page()，这个偏移量一直
     * 保持默认值 0（恒等）也没出过问题；Lab6 是第一个在这之后还要现场
     * 分配用户程序页/用户栈页的 Lab，不补这一步会在 kalloc_pages()
     * 内部读 free_run_t 节点时触发 page fault。必须紧跟在
     * pagetable_activate() 之后、下面第一次 kalloc_page() 调用之前
     * 完成。
     *
     * 提示：
     * kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE);
     */

    console_puts_line("switched to page table, low identity map gone");

    trap_init();
    timer_enable();
    sbi_set_timer(read_time() + 10000000ull / 100ull);
    console_puts_line("timer armed, ecall entry armed, building user program");

    /* TODO 3：分配、清零、拷贝、映射用户程序页。
     *
     * 跟 x86_64 版本（../x86_64/kernel_main.c 的同一处 TODO）完全同样
     * 的四步——kalloc_page() 返回物理地址，拷贝内容前必须先经过内核
     * 自己的高半自映射（pa + KERNEL_VIRT_BASE）才能被 CPU 解引用
     * （分页已经激活，低物理地址的身份映射早就撤了，直接把物理地址
     * 当指针用会立刻 page fault），检查长度不超过 PAGE_SIZE，memset
     * 清零再 memcpy 拷贝，最后 pagetable_map(root, USER_PROG_VADDR,
     * 物理地址, PTE_FLAG_USER | PTE_FLAG_EXECUTABLE)。跟 x86_64 版本
     * 同一段逻辑完全一致，只是 KERNEL_VIRT_BASE 的具体数值不同。
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
     * 不需要拷贝内容，但同样需要先清零——理由跟 x86_64 版本一致：新
     * 分配的物理页内容是上一个使用者留下的垃圾，不应该依赖"这次凑巧
     * 无害"。映射地址用 USER_STACK_VADDR - PAGE_SIZE 当栈页的*起始*
     * 地址（USER_STACK_VADDR 本身是栈顶，见上面 TODO 1 的坑）。
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
     * 用 sret 完成第一次 S→U 特权级切换（为什么 riscv64 的 sret 能
     * 直接用于第一次切换、不需要像 x86_64 那样区分"第一次用 iretq、
     * 后续用 sysretq"，见 trap_entry.S 里 enter_user_mode 的注释）。
     * 这里只需要 extern 声明它的签名，然后传入程序入口地址和栈顶地址。
     *
     * 提示：
     * extern void enter_user_mode(uintptr_t entry, uintptr_t stack);
     * enter_user_mode(USER_PROG_VADDR, USER_STACK_VADDR);
     */

    /* enter_user_mode 不会返回（跳进用户态之后，唯一的回路是 ecall，
     * 那条路径完全在 trap_entry.S/trap.c 里处理，不会走回这个函数）
     * ——跟 x86_64 版本同一处注释一样的道理：用户程序的 sys_exit 只是
     * 打印退出码，不会让 hart 停下来，用户态代码在那之后落进它自己的
     * 忙等死循环（`j 1b`，见 user_prog.S——原本用的是 wfi，但 U-mode
     * 执行 wfi 会因为 mstatus.TW 触发 illegal instruction，实测改成忙等，
     * 理由见 user_prog.S 对应注释），内核这边的主循环理论上还能继续跑
     * 定时器 tick，但本 Lab 的用户程序触发两次 ecall 之后就永久留在
     * 用户态自己的循环里，控制权不会再回到这里——这是本 Lab 教学范围
     * 内可以接受的简化，Lab7 的调度器会引入"定时器中断时把 CPU 从
     * 用户态抢回来"这个机制。 */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
