/* Lab7 riscv64：在 Lab6"单个用户程序、内核态忙等"的基础上，换成真正的
 * 多进程轮转调度——跟 x86_64 版本（../x86_64/kernel_main.c）同一个
 * 重构方向：kernel_boot() 不再自己 memcpy 用户程序、手工搭 trapframe、
 * 手工跳用户态，这些工作全部下沉进 proc.c 的 proc_alloc()/proc_alloc_
 * skeleton()（Lab6 这个文件里那段"分配用户程序页+用户栈页+映射"逻辑
 * 原样搬进了 proc.c 的 map_user_prog()，行为不变，只是从"内联在这里、
 * 只跑一次"变成"proc_alloc()/sys_exec() 都能调用的函数"）。kernel_boot()
 * 自己的职责收缩成纯粹的初始化序列：
 *   1. 建内核自己的根页表（跟 Lab4-6 完全一样）。注意 memmap_discover()
 *      *不*在这里调用——boot.S 阶段 A 已经调过一次了（原因见 boot.S
 *      文件顶部说明：DTB 物理地址在真实的 QEMU virt 配置下落在"RAM 顶部
 *      往下 2MiB"，远超这里即将建的正式页表覆盖范围，只有 boot.S 那次
 *      "分页完全没打开、bare mode 任意物理地址直接可解引用"的调用时机
 *      是安全的）。最初这里重复调了一次 memmap_discover(dtb_paddr)——
 *      构建、链接都不报错，但实测在 QEMU 下会在打印完"Hello OS"之后立刻
 *      卡死/复位：这次调用发生在 pagetable_activate() 已经生效之后，
 *      dtb_paddr 指向的物理地址不在下面刚建好的正式页表映射范围内
 *      （正式表只映射了内核镜像+UART 一页），memmap_discover() 内部
 *      `(const uint8_t *)(uintptr_t)dtb_paddr` 那次解引用会触发缺页
 *      异常，而此刻 trap_init() 还没跑，stvec 还是 boot.S/OpenSBI 交接
 *      时遗留的默认值，没有内核装的处理入口——表现正是"静默卡死，QEMU
 *      -d guest_errors 也看不出任何异常"，用 `-d in_asm` 抓完整指令
 *      轨迹、对照 nm 符号表反查地址才确认根因是重复调用，不是猜测。
 *      教训：这个函数的调用时机是跨文件契约（boot.S 阶段 A 调一次，
 *      仅此一次），不能凭"这里读起来像是该做内存探测的地方"就补一次
 *      看似无害的重复调用。
 *   2. pagetable_set_kernel_root()——必须在 pagetable_activate() 之前
 *      调用（pagetable.h 对这个函数的注释明确要求这个顺序），因为
 *      proc_alloc_skeleton() 里 pagetable_copy_kernel_range() 需要
 *      读取这份记录，而第一个进程在 trap_init()/timer_enable() 之后、
 *      scheduler() 之前就会被创建。
 *   3. trap_init() + timer_enable()，跟 Lab6 一样,不需要额外的 Lab7
 *      新增初始化步骤——riscv64 没有 TSS 等价物,不存在 x86_64 版本
 *      "Lab7 新增 tss_init()"这一步,per-process 内核栈顶完全通过
 *      trap_kernel_sp_top 这个全局变量表达（trap.c/trap_entry.S/
 *      proc.c scheduler() 已经处理),不需要在这里额外初始化任何硬件
 *      结构。
 *   4. proc_init() 清空进程表，proc_alloc() 创建*一个*初始进程——跟
 *      x86_64 版本同一个教学范围（只需要演示"从一个进程开始，通过
 *      fork() 长出更多进程"这条路径）。
 *   5. scheduler()——noreturn，此后所有执行都在"调度器 swtch() 进某个
 *      进程 / 进程 yield() 或 exit 切回调度器"这个循环里，kernel_boot()
 *      不会再被回到,原来 Lab6 版本末尾的 enter_user_mode()调用和
 *      `for (;;) { wfi; }` 忙等循环一起被移除——enter_user_mode 本身
 *      已经在 trap_entry.S 里被删除（Lab7 的 trap_return 统一取代了
 *      它的角色,见 trap_entry.S 对应注释),这里继续调用它会链接失败,
 *      必须同步移除,不是可选的清理。
 *
 * 不变的关键简化（跟 Lab6 一致，见 proc.c 顶部模块注释）：exec 是"重新
 * 映射成内嵌镜像的新拷贝"，fork 是"完整复制，不是 COW"。
 *
 * 跟 Lab6 相比新出现的地址空间隔离：每个进程有自己独立的页表（不再
 * 像 Lab6 那样全程共用同一份 satp),但内核代码本身通过 pagetable_
 * copy_kernel_range() 共享同一批内核页表节点——见 pagetable.h 对应
 * 函数的完整注释。 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "string.h"
#include "sbi.h"
#include "proc.h"

void trap_init(void);
void timer_enable(void);

extern char __kernel_end[];

#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull
#define KERNEL_LOAD_ADDR 0x80200000ull

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

    console_puts_line("Hello OS from riscv64 (Lab7: processes & scheduling)");

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

    /* 自映射范围延伸到 kernel_end_phys 往后一段——跟 x86_64 版本同一个
     * 理由（proc_alloc_skeleton() 会给每个进程分配独立页表节点+内核栈,
     * NPROC=4 叠加 fork() 出的子进程,kalloc 空闲池实际使用量比 Lab6
     * 单个用户程序时更大),这里维持 Lab6 就选定的 2MiB 窗口——教学范围
     * 内远不会用满。 */
    uintptr_t self_map_end = KERNEL_LOAD_ADDR + 0x200000ull;
    for (uintptr_t pa = phys_end; pa < self_map_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    /* UART MMIO：必须保留这条映射，理由跟 Lab4/5/6 完全一致，不重复
     * 展开。 */
    pagetable_map(root, 0x10000000ull, 0x10000000ull, PTE_FLAG_WRITABLE);

    /* 必须在 pagetable_activate() 之前——pagetable.h/pagetable.c 对
     * pagetable_set_kernel_root() 的注释明确要求这个顺序,proc_alloc()
     * 稍后调用 pagetable_copy_kernel_range() 时需要这份记录已经就位,
     * 跟 x86_64 版本同一处调用顺序完全一致。 */
    pagetable_set_kernel_root(root);

    pagetable_activate(root);

    /* 见 kalloc.h 里 kalloc_set_phys_to_virt_offset() 的完整注释——
     * Lab3-5 从未在 pagetable_activate() 之后调用过 kalloc_page()，
     * 这个偏移量一直保持默认值 0（恒等）也没出过问题；Lab6 起需要
     * 现场分配用户程序页/用户栈页，不补这一步会在 kalloc_pages()
     * 内部读 free_run_t 节点时 #PF。必须紧跟在 pagetable_activate()
     * 之后、下面第一次 kalloc_page() 调用之前完成。 */
    kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE);

    console_puts_line("switched to page table, low identity map gone");

    trap_init();
    timer_enable();
    sbi_set_timer(read_time() + 10000000ull / 100ull);
    console_puts_line("timer armed, ecall entry armed, creating initial process");

    proc_init();
    struct proc *init_proc = proc_alloc();
    if (init_proc == NULL) {
        panic("kernel_boot: proc_alloc() failed for the initial process");
    }

    console_puts_line("initial process created, entering scheduler");

    scheduler();
}
