/* Lab9 x86_64：kernel_boot() 的初始化序列跟 Lab7/Lab8 相比*没有新增
 * 步骤*——本 Lab 的全部新东西（ELF 加载、管道、多用户程序、shell）都
 * 活在 proc_alloc()/exec_load()/用户态代码里，kernel_boot() 只是把
 * 控制权交给它们的地方，序列本身保持 Lab8 定下的形状：
 *   1. 内存探测 + 建内核自己的根页表（跟 Lab4-6 完全一样）。
 *   2. pagetable_set_kernel_root()——必须在 pagetable_activate() 之前
 *      调用（pagetable.h 对这个函数的注释明确要求这个顺序），因为
 *      proc_alloc_skeleton() 里 pagetable_copy_kernel_range() 需要
 *      读取这份记录，而第一个进程在 gdt_init() 之后、scheduler() 之前
 *      就会被创建。
 *   3. gdt_init() + tss_init() + idt_init() + syscall_init() + pit_init()，
 *      跟 Lab7/Lab8 完全一样。
 *   4. blk_init() + fs_init()——跟 Lab8 完全一样，Lab9 的 /init、/sh 等
 *      六个用户程序和 /initrc 脚本都是通过这层从磁盘镜像里读出来的，
 *      不再是内嵌进内核镜像的 blob（跟 Lab6/Lab7 的关键差异，完整说明
 *      见 proc.c INIT_PATH 上方注释）。
 *   5. proc_init() 清空进程表，proc_alloc() 创建*一个*初始进程——跟
 *      Lab7/Lab8 一样只创建一个，但这个进程现在运行的是从磁盘加载的
 *      /init（见 user/init.c），不是内嵌的占位程序：它会自己 fork+exec
 *      出 /initrc 指定的一系列命令，之后循环启动交互式 shell。
 *   6. scheduler()——noreturn，不会再被回到。
 *
 * 不变的关键简化（跟 Lab7/Lab8 一致，这里不重复展开，见 proc.c 顶部
 * 模块注释）：所有进程共用同一份*内核*页表内容（pagetable_copy_kernel_
 * range() 共享节点，不是各自深拷贝），但每个进程有自己独立的*用户*
 * 地址空间映射（fork()/exec() 因此才有意义）。
 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "string.h"
#include "pit.h"
#include "proc.h"
#include "blk.h"  /* Lab8: blk_init——块设备驱动（本架构是 ide.c 的 ATA PIO） */
#include "fs.h"   /* Lab8: fs_init/fs_lookup/fs_read——只读文件系统层 */

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
    console_puts_line("Hello OS from x86_64 (Lab9: shell and userspace)");

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
    tss_init();

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
    console_puts_line("timer armed, syscall entry armed, mounting filesystem");

    /* 块设备 + 文件系统初始化——跟 Lab8 完全一样，Lab9 没有改动这一段。
     *
     * 位置的两个约束：
     *
     *   1. 必须在 proc_init()/scheduler() *之前*。fs_init() 会做挂载
     *      自检，要读十几个块；这些读走的是轮询，中断已经开着，如果此时
     *      已经有进程在跑，时钟中断会在自检中途 yield() 走。虽然本 Lab
     *      的 fs.c 所有缓冲区都在栈上、被打断也是安全的（见 fs.c 里
     *      read_inode() 的注释），但"内核初始化阶段被调度器打断"这件事
     *      本身会让启动顺序变得难以推理。初始化一次性做完再进调度器。
     *
     *   2. 必须在 kalloc_set_phys_to_virt_offset() 之后——虽然 fs.c/ide.c
     *      都不调 kalloc_page()，但它们会 panic，panic 路径要打印，而
     *      整条初始化链上 pagetable_activate() 之后的规则是统一的：
     *      任何事都在偏移量设好之后做。
     *
     * blk_init() 在前、fs_init() 在后：文件系统建立在块设备之上，块设备
     * 不在的话 fs_init() 第一次 blk_read(0) 就会撞进驱动的"没有盘"分支。
     * 这个顺序让"盘不存在"在 blk_init() 里就被报出来，错误信息说的是
     * "总线上没有硬盘"，而不是"超级块魔数不对"——后者会把人引向错误的
     * 排查方向（以为镜像格式有问题）。 */
    blk_init();
    fs_init();

    /* 内核态自检：绕开系统调用直接读一个文件。
     *
     * 这一步刻意不经过 open/read——它验证的是 fs.c 这一层本身是通的。
     * 如果这里能读到内容、而用户程序读不到，问题一定在 syscall 路径
     * （参数传递、fd 表）而不在文件系统；反过来这里就失败的话，用户
     * 态那边再怎么调也没用。把一条长链路拆成两段分别验证，是内核调试
     * 里最省时间的习惯。
     *
     * Lab9 起 /initrc 里也有一条 `cat /motd.txt`，看起来像是重复检查
     * 同一个文件——但这两次读走的是完全不同的两条代码路径（这里是
     * kernel_boot() 直接调 fs_read()，initrc 那次要经过 sys_open/
     * sys_read/文件描述符表/用户指针拷贝),留着这一步的价值没有变：
     * 如果这里的检查过了但 initrc 那条挂了，问题范围立刻从"整个文件
     * 系统"收窄到"系统调用这一层"。 */
    uint32_t motd_inum = fs_lookup("motd.txt");
    if (motd_inum == 0) {
        panic("kernel_boot: 镜像里找不到 motd.txt——fsroot/ 的内容和 mkfs 的输出不一致？");
    }
    char probe[64];
    uint32_t probe_n = fs_read(motd_inum, 0, probe, sizeof(probe) - 1);
    probe[probe_n] = '\0';

    /* 只打印到第一个换行为止。
     *
     * 两个原因，第二个是踩出来的：
     *   1. 一行的启动日志更好看，也更好被 tests/expect-*.txt 抓。
     *   2. motd.txt 第一行之后就是中文，而 UTF-8 里一个汉字占 3 个字节。
     *      按固定字节数截断（最初这里是 31 字节）会切在一个汉字的中间，
     *      终端拿到半个字符，打出来是一个替换符 `?`。这不是文件系统读错了
     *      ——字节完全正确——是"按字节截断 UTF-8 文本"这件事本身的问题。
     *      文件系统只认字节、不认字符边界，这一课在 motd.txt 的正文里也
     *      写了一遍。真要按字符截断，得在更上层做 UTF-8 解码；这里选了
     *      更简单的办法：在一个已知是 ASCII 的分隔符（换行）上切。 */
    for (uint32_t i = 0; i < probe_n; i++) {
        if (probe[i] == '\n') {
            probe[i] = '\0';
            break;
        }
    }

    kprintf("fs: kernel-side read of motd.txt (inode %u, %u bytes): %s\n",
            motd_inum, fs_size(motd_inum), probe);

    console_puts_line("filesystem ready, loading /init");

    proc_init();
    struct proc *init_proc = proc_alloc();
    if (init_proc == NULL) {
        panic("kernel_boot: proc_alloc() failed for the initial process");
    }

    console_puts_line("init loaded (pid 1), entering scheduler");

    scheduler();
}
