/* Lab7 x86_64：在 Lab6"单个用户程序、内核态忙等"的基础上，换成真正的
 * 多进程轮转调度——kernel_boot() 不再自己 memcpy 用户程序、手工搭
 * trapframe、iretq 跳过去，这些工作全部下沉进 proc.c 的 proc_alloc()/
 * proc_alloc_skeleton()（Lab6 kernel_boot() 里那段"分配用户程序页+
 * 用户栈页+映射"逻辑原样搬进了 proc.c 的 map_user_prog()，行为不变，
 * 只是从"内联在这里、只跑一次"变成"proc_alloc()/sys_exec() 都能调用
 * 的函数"）。kernel_boot() 自己的职责收缩成纯粹的初始化序列：
 *   1. 内存探测 + 建内核自己的根页表（跟 Lab4-6 完全一样）。
 *   2. pagetable_set_kernel_root()——必须在 pagetable_activate() 之前
 *      调用（pagetable.h 对这个函数的注释明确要求这个顺序），因为
 *      proc_alloc_skeleton() 里 pagetable_copy_kernel_range() 需要
 *      读取这份记录，而第一个进程在 gdt_init() 之后、scheduler() 之前
 *      就会被创建。
 *   3. gdt_init() + tss_init()（Lab7 新增，见 trap.c 顶部模块注释——
 *      本 Lab 引入的"用户态代码长时间运行、被定时器反复打断"场景，
 *      不装 TSS.RSP0 会在真实使用中必然触发三重故障）+ idt_init() +
 *      syscall_init() + pit_init()，跟 Lab6 一样，只是多了 tss_init()
 *      这一步。
 *   4. proc_init() 清空进程表，proc_alloc() 创建*一个*初始进程（本 Lab
 *      教学范围内只需要演示"从一个进程开始，通过 fork() 长出更多
 *      进程"这条路径，不需要在 kernel_boot() 里一次创建多个）。
 *   5. scheduler()——noreturn，此后所有执行都在"调度器 swtch() 进某个
 *      进程 / 进程 yield() 或 exit 切回调度器"这个循环里，kernel_boot()
 *      不会再被回到。
 *
 * 不变的关键简化（跟 Lab6 一致，这里不重复展开，见 trap.c sys_write
 * 注释和本文件 Lab6 版本原有的说明）：所有进程共用同一份*内核*页表
 * 内容（pagetable_copy_kernel_range() 共享 PDPT/PD/PT 节点，不是各自
 * 深拷贝），但每个进程有自己独立的*用户*地址空间映射——这是 Lab7
 * 相对 Lab6"用户和内核完全共用同一份页表"的真正变化：从"零地址空间
 * 隔离"变成"内核共享、用户隔离"，为 fork()/exec() 提供了意义（如果
 * 仍然像 Lab6 一样所有进程共用同一份页表，fork() 出的"子进程"会跟
 * 父进程写同一份用户内存，不构成真正的进程隔离）。
 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "string.h"
#include "pit.h"
#include "proc.h"

void memmap_discover(uint32_t mb2_info_addr);
void idt_init(void);
void gdt_init(void);
void tss_init(void);
void syscall_init(void);

extern char __kernel_end[];

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull
#define KERNEL_LOAD_ADDR 0x100000ull

void kernel_boot(uint32_t mb2_info_addr)
{
    console_puts_line("Hello OS from x86_64 (Lab7: processes & scheduling)");

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

    /* 自映射范围延伸到 kernel_end_phys 往后一段（不只是内核镜像本身占
     * 用的部分）——kalloc 空闲池紧跟在镜像后面，pagetable.c walk() 的
     * table_ptr() 需要通过这个自映射才能解引用 kalloc_page() 分配出来
     * 的页表节点物理地址，见 pagetable.c 对应位置的完整注释（Lab4 就
     * 踩过的坑，riscv64 那边先实测触发，x86_64 这边同一个设计同一个
     * 漏洞）。Lab7 比 Lab6 需要更大的自映射范围：proc_alloc_skeleton()
     * 会给每个进程分配独立页表节点+内核栈，NPROC=4 个进程叠加 fork()
     * 出的子进程，kalloc 空闲池的实际使用量比 Lab6 单个用户程序时更大，
     * 这里维持 Lab6 就选定的 2MiB（0x200000）窗口——教学范围内 4 个
     * 进程 + 少量 fork 远不会用满这个窗口，不需要现在就精确计算。 */
    uintptr_t self_map_end = KERNEL_LOAD_ADDR + 0x200000ull;
    for (uintptr_t pa = phys_end; pa < self_map_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    /* 必须在 pagetable_activate() 之前——pagetable.h/pagetable.c 对
     * pagetable_set_kernel_root() 的注释明确要求这个顺序,proc_alloc()
     * 稍后调用 pagetable_copy_kernel_range() 时需要这份记录已经就位。 */
    pagetable_set_kernel_root(root);

    pagetable_activate(root);
    gdt_init();

    /* TODO 1：调用 tss_init()。
     *
     * Lab7 新增——trap.c 顶部模块注释已经完整交代过这一步为什么不是
     * 可选的优化：Lab1-6 全程没有 `ltr` 过任何 TSS，TR 一直是无效值，
     * IDT 中断门在 CPL3 触发提权到 CPL0 时，硬件读 TSS.RSP0 决定用哪个
     * 内核栈——本 Lab 用户态代码会长时间运行、被定时器反复打断（轮转
     * 调度的机制本身），不装 TSS.RSP0 不是"可能触发的边界情况"，是
     * "教学场景下必然会触发的主路径"，触发后果是三重故障、QEMU 静默
     * 复位（完整的崩溃链路见 trap.c tss_init() 函数体注释）。必须在
     * gdt_init() 之后调用（tss_init() 内部要往 GDT 数组里第 6/7 项
     * 填 TSS 描述符，GDT 本身必须已经存在），在下面 proc_alloc()
     * 创建第一个进程之前调用（scheduler() 第一次 swtch() 进程之前会
     * 调 tss_set_rsp0()，那时 TSS 结构体本身必须已经被 tss_init()
     * 正确初始化过，不能是一个从未 ltr 过的野值）。
     *
     * 提示：
     * tss_init();
     */

    /* 见 kalloc.h 里 kalloc_set_phys_to_virt_offset() 的完整注释——
     * Lab3-5 从未在 pagetable_activate() 之后调用过 kalloc_page()，
     * 这个偏移量一直保持默认值 0（恒等）也没出过问题；Lab6 起需要
     * 现场分配用户程序页/用户栈页，不补这一步会在 kalloc_pages()
     * 内部读 free_run_t 节点时 #PF。必须紧跟在 pagetable_activate()
     * 之后、下面第一次 kalloc_page() 调用之前完成。 */
    kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE);

    console_puts_line("switched to page table, low identity map gone");

    idt_init();
    syscall_init();
    pit_init();
    __asm__ volatile("sti");
    console_puts_line("timer armed, syscall entry armed, creating initial process");

    /* TODO 2：调用 proc_init()，然后 proc_alloc() 创建初始进程，
     * proc_alloc() 失败要 panic，最后调用 scheduler()（noreturn，
     * 这个函数从此不会再返回到这里）。
     *
     * proc_init() 清空进程表（proc.c 里的全局数组，本 Lab 不依赖
     * "静态数组默认全零"这个隐含假设，显式清零一次——理由跟 Lab3-6
     * kalloc.c/pagetable.c 一路坚持的"不依赖凑巧无害"是同一个取向）。
     * proc_alloc() 分配一个全新的 struct proc（内核栈、独立页表、
     * map_user_prog() 映射好用户程序+用户栈），这是本 Lab 教学范围内
     * 唯一一次手工创建进程——后续所有进程都通过用户态 fork() 系统调用
     * 产生，不是 kernel_boot() 里再多调几次 proc_alloc()。
     *
     * 提示：
     * proc_init();
     * struct proc *init_proc = proc_alloc();
     * if (init_proc == NULL) {
     *     panic("kernel_boot: proc_alloc() failed for the initial process");
     * }
     *
     * console_puts_line("initial process created, entering scheduler");
     *
     * scheduler();
     */
}
