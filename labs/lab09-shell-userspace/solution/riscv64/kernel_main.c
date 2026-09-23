/* Lab9 riscv64：kernel_boot() 的初始化序列跟 Lab7/Lab8 相比*没有新增
 * 步骤*——跟 x86_64 版本（../x86_64/kernel_main.c）同一个理由：本 Lab
 * 的全部新东西（ELF 加载、管道、多用户程序、shell）都活在 proc_alloc()/
 * exec_load()/用户态代码里，kernel_boot() 只是把控制权交给它们的地方。
 * 职责仍然是纯粹的初始化序列：
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
 *   4. blk_init() + fs_init()——跟 Lab8 完全一样，Lab9 的 /init、/sh 等
 *      六个用户程序和 /initrc 脚本都是通过这层从磁盘镜像里读出来的，
 *      不再是内嵌进内核镜像的 blob（跟 Lab6/Lab7 的关键差异，完整说明
 *      见 proc.c INIT_PATH 上方注释）。
 *   5. proc_init() 清空进程表，proc_alloc() 创建*一个*初始进程——跟
 *      Lab7/Lab8 一样只创建一个，但这个进程现在运行的是从磁盘加载的
 *      /init（见 user/init.c），不是内嵌的占位程序：它会自己 fork+exec
 *      出 /initrc 指定的一系列命令，之后循环启动交互式 shell。
 *   6. scheduler()——noreturn，此后所有执行都在"调度器 swtch() 进某个
 *      进程 / 进程 yield() 或 exit 切回调度器"这个循环里，kernel_boot()
 *      不会再被回到,原来 Lab6 版本末尾的 enter_user_mode()调用和
 *      `for (;;) { wfi; }` 忙等循环一起被移除——enter_user_mode 本身
 *      已经在 trap_entry.S 里被删除（Lab7 的 trap_return 统一取代了
 *      它的角色,见 trap_entry.S 对应注释),这里继续调用它会链接失败,
 *      必须同步移除,不是可选的清理。
 *
 * 不变的关键简化（跟 Lab7/Lab8 一致，见 proc.c 顶部模块注释）：所有
 * 进程共用同一份*内核*页表内容（pagetable_copy_kernel_range() 共享
 * 节点，不是各自深拷贝），但每个进程有自己独立的*用户*地址空间
 * 映射——这是相对 Lab6"全程共用同一份 satp"的关键变化，为 fork()/
 * exec() 提供了意义（否则 fork() 出的"子进程"会跟父进程写同一份用户
 * 内存，不构成真正的进程隔离）。 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"
#include "string.h"
#include "sbi.h"
#include "proc.h"
#include "blk.h"  /* Lab8: blk_init——块设备驱动（本架构是 virtio.c 的 virtio-blk） */
#include "fs.h"   /* Lab8: fs_init/fs_lookup/fs_read——只读文件系统层 */

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

    console_puts_line("Hello OS from riscv64 (Lab9: shell and userspace)");

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

    /* Lab8：virtio-mmio 传输通道的 MMIO 窗口。
     *
     * virt 机器把 8 个 virtio-mmio 通道放在 0x10001000..0x10008fff，每个
     * 一页。virtio.c 要扫描全部 8 个（理由见那边 virtio_find_device()），
     * 所以这里 8 页全部映射，而不是只映射实际用到的那一页——少映射一页，
     * 扫描就会在那一页上 load page fault，而"扫描一个可能空着的槽位"是
     * 正常行为，不该触发异常。
     *
     * 恒等映射（VA == PA）跟 UART 同一个做法：MMIO 地址是硬件定死的，
     * 给它换一个虚拟地址没有任何好处，只会让"代码里写的地址"和"手册/
     * 设备树里写的地址"对不上。
     *
     * (0x10001000 >> 30) & 0x1ff == 0，跟 UART 的 0x10000000 落在
     * 同一个顶级页表项里；用户地址空间 [0x400000, 0x800000)（exec.c 的
     * USER_MIN_VADDR/USER_STACK_TOP）恰好也落在这个顶级项里
     * （0x800000 远小于 1GiB=0x40000000）——曾经在这里踩过一个坑：
     * pagetable_copy_kernel_range() 早期实现直接把内核表这个顶级项
     * （连同它下面整棵二级/三级节点子树）原样复制给每个新进程，
     * 表面上"顺便"让这 9 个 MMIO 页自动出现在每个进程页表里,但代价是
     * 每个进程的用户代码/栈映射（同样落在这个顶级项下）也变成了
     * *全系统共享同一份*物理叶子节点——父进程和它 exec 出的子进程的
     * 0x400000 代码页最终写进同一个叶子 PTE 槲位,子进程退出、
     * sys_wait() 回收它的映射时,顺手把父进程仍在用的那个槽位也清空了,
     * 父进程随后取指到 0x400000 附近直接缺页/译码出非法指令——完全
     * 确定性的结构性错误,不是时序竞争。
     *
     * 现在 pagetable_copy_kernel_range()（pagetable.c）已经改成只
     * 共享这 9 个 MMIO 页各自的*叶子* PTE（用 pagetable_map() 逐页
     * 建立,指向同一批物理页),不再共享整个顶级项/中间节点——每个进程
     * 在这个顶级项下有自己私有的中间节点,不会撞上其它进程的用户地址
     * 空间。这里改 flags 时（WRITABLE 与否、要不要加 EXECUTABLE）
     * 必须同步改 pagetable.c 里那份重复的 pagetable_map() 调用,两处
     * 权限不一致的话,新进程通过共享机制重新建出来的 UART/virtio 映射
     * 权限会跟内核自己这份不一样。如果 virtio 的地址落在别的 1GiB
     * 区间,同样要检查是否与某个进程私有区域（用户地址空间/内核镜像)
     * 冲突,换机型（比如 sifive_u）时要重新算一遍。 */
    for (uintptr_t i = 0; i < 8; i++) {
        uintptr_t mmio = 0x10001000ull + i * 0x1000ull;
        pagetable_map(root, mmio, mmio, PTE_FLAG_WRITABLE);
    }

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
    console_puts_line("timer armed, ecall entry armed, mounting filesystem");

    /* 块设备 + 文件系统初始化——跟 Lab8 完全一样，Lab9 没有改动这一段。
     * 位置的约束跟 x86_64 版本完全一样
     * （必须在 proc_init()/scheduler() 之前，必须在
     * kalloc_set_phys_to_virt_offset() 之后；blk_init() 在前、fs_init()
     * 在后），完整理由见 x86_64/kernel_main.c 同一处注释，不重复展开。
     *
     * riscv64 这边多一条约束：必须在 pagetable_activate() 之后——virtio.c
     * 要访问 0x10001000 的 MMIO 寄存器，而那条映射是上面刚建立的，只有
     * 页表生效之后才能用。更关键的是 virtio.c 还要把内核虚拟地址翻译成
     * 物理地址交给设备做 DMA，这个翻译依赖"内核 VA = PA + KERNEL_VIRT_BASE"
     * 这个自映射已经成立。x86_64 的 ATA PIO 完全不碰地址翻译，所以那边
     * 没有这一条。 */
    blk_init();
    fs_init();

    /* 内核态自检：绕开系统调用直接读一个文件——验证 fs.c 这一层本身是
     * 通的。如果这里能读到内容、而用户程序读不到，问题一定在 syscall
     * 路径而不在文件系统。把一条长链路拆成两段分别验证，是内核调试里
     * 最省时间的习惯。
     *
     * Lab9 起 /initrc 里也有一条 `cat /motd.txt`，两次读走的是完全不同
     * 的两条代码路径（完整理由见 x86_64 版本同一处注释），留着这一步
     * 的价值没变。 */
    uint32_t motd_inum = fs_lookup("motd.txt");
    if (motd_inum == 0) {
        panic("kernel_boot: 镜像里找不到 motd.txt——fsroot/ 的内容和 mkfs 的输出不一致？");
    }
    char probe[64];
    uint32_t probe_n = fs_read(motd_inum, 0, probe, sizeof(probe) - 1);
    probe[probe_n] = '\0';

    /* 只打印到第一个换行为止：一行日志更好被 tests/expect-*.txt 抓，
     * 而且按固定字节数截断会切在汉字中间（UTF-8 一个汉字 3 字节），
     * 打出来是替换符。完整的踩坑记录见 x86_64 版本同一处注释。 */
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
