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
    console_puts_line("Hello OS from x86_64 (Lab8: filesystem)");

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

    /* TODO 1（Lab8 新增）：依次调用 blk_init() 和 fs_init()。
     *
     * 这两行的**位置**有两个约束，比这两行本身更值得想清楚：
     *
     *   1. 必须在 proc_init()/scheduler() **之前**。fs_init() 会做挂载
     *      自检，要读十几个块；这些读走的是轮询，而中断已经开着——如果
     *      此时已经有进程在跑，时钟中断会在自检中途 yield() 走。虽然本
     *      Lab 的 fs.c 所有缓冲区都在栈上、被打断也是安全的（见 fs.c 里
     *      read_inode() 的注释），但"内核初始化阶段被调度器打断"这件事
     *      本身会让启动顺序变得难以推理。初始化一次性做完再进调度器。
     *
     *   2. 必须在 kalloc_set_phys_to_virt_offset() 之后。虽然 fs.c/ide.c
     *      都不调 kalloc_page()，但它们会 panic，panic 路径要打印——整条
     *      初始化链上 pagetable_activate() 之后的规则是统一的：任何事都
     *      在偏移量设好之后做。
     *
     * 顺序上 blk_init() 必须在前：文件系统建立在块设备之上，块设备不在的
     * 话 fs_init() 第一次 blk_read(0) 就会撞进驱动的"没有盘"分支。这个
     * 顺序让"盘不存在"在 blk_init() 里就被报出来，错误信息说的是"总线上
     * 没有硬盘"，而不是"超级块魔数不对"——后者会把人引向错误的排查方向
     * （以为镜像格式有问题）。**让错误在离原因最近的地方报出来**，比错误
     * 信息写得漂亮更重要。
     *
     * 提示：
     * blk_init();
     * fs_init();
     */

    /* TODO 2（Lab8 新增）：内核态自检——绕开系统调用直接读一个文件。
     *
     * 为什么要有这一步：它验证的是 fs.c 这一层本身是通的。如果这里能读到
     * 内容、而用户程序读不到，问题一定在 syscall 路径（参数传递、fd 表）
     * 而不在文件系统；反过来这里就失败的话，用户态那边再怎么调也没用。
     * **把一条长链路拆成两段分别验证**，是内核调试里最省时间的习惯——本
     * Lab 的链路是"驱动 -> fs.c -> 系统调用 -> 用户程序"四段，一次性全写
     * 完再开机，出问题时你会面对四个等可能的嫌疑人。
     *
     * 步骤：
     *   1. fs_lookup("motd.txt") 拿 inum，为 0 就 panic（镜像里没这个文件，
     *      说明 fsroot/ 的内容和 mkfs 的输出不一致）。
     *   2. char probe[64]; 用 fs_read(inum, 0, probe, sizeof(probe) - 1)
     *      读开头，然后 probe[n] = '\0'。留一个字节给结尾符。
     *   3. **把第一个 '\n' 换成 '\0'**，只打印第一行。
     *   4. kprintf 出 inode 号、fs_size(inum) 和这一行内容。
     *   5. console_puts_line("filesystem ready, creating initial process");
     *      （tests/expect-x86_64.txt 会 grep 第 4、5 步的输出。）
     *
     * 第 3 步有两个原因，第二个是本课程真的踩出来的：
     *   a. 一行的启动日志更好看，也更好被 expect 文件抓。
     *   b. motd.txt 第一行之后就是中文，而 **UTF-8 里一个汉字占 3 个字节**。
     *      按固定字节数截断（最初这里写的是 31 字节）会切在一个汉字的中间，
     *      终端拿到半个字符，打出来是一个替换符 `?`。这不是文件系统读错了
     *      ——字节完全正确——是"按字节截断 UTF-8 文本"这件事本身的问题。
     *      文件系统只认字节、不认字符边界；这一课在 motd.txt 的正文里也
     *      写了一遍。真要按字符截断，得在更上层做 UTF-8 解码；这里选了更
     *      简单的办法：在一个已知是 ASCII 的分隔符（换行）上切。
     *
     * 提示：
     * uint32_t motd_inum = fs_lookup("motd.txt");
     * if (motd_inum == 0) {
     *     panic("kernel_boot: 镜像里找不到 motd.txt——fsroot/ 的内容和 mkfs 的输出不一致？");
     * }
     * char probe[64];
     * uint32_t probe_n = fs_read(motd_inum, 0, probe, sizeof(probe) - 1);
     * probe[probe_n] = '\0';
     *
     * for (uint32_t i = 0; i < probe_n; i++) {
     *     if (probe[i] == '\n') {
     *         probe[i] = '\0';
     *         break;
     *     }
     * }
     *
     * kprintf("fs: kernel-side read of motd.txt (inode %u, %u bytes): %s\n",
     *         motd_inum, fs_size(motd_inum), probe);
     *
     * console_puts_line("filesystem ready, creating initial process");
     */

    proc_init();
    struct proc *init_proc = proc_alloc();
    if (init_proc == NULL) {
        panic("kernel_boot: proc_alloc() failed for the initial process");
    }

    console_puts_line("initial process created, entering scheduler");

    scheduler();
}
