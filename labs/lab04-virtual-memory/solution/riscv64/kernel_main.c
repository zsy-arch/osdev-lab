#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"

/* trap.c 没有对应的共享头，理由和 x86_64 那份 kernel_main.c 里
 * idt_init() 一样：riscv64 用 stvec 注册陷入入口，x86_64 用 IDT，
 * 两者没有共同接口可抽象，就地声明。 */
void trap_init(void);

/* linker.ld 定义的链接期符号：__kernel_end 是内核镜像（.text 到
 * .bss/栈）在高 VMA 视角下的结束地址，用来决定自映射要覆盖多大范围。
 * 链接器符号在 C 里只能声明成数组类型的 extern，取它的地址（不是内容）
 * 才是真正想要的值。
 *
 * 注意：这里不能用 linker.ld 里的 __kernel_phys_end——那个符号是
 * `LOADADDR(.bss) + ...` 算出来的*绝对物理地址*常量（专门给 boot.S
 * 在阶段 A 用的，此刻分页还没开，PC 在低地址，取它的值不涉及任何跨段
 * 相对寻址）。但 kernel_main.c 是普通高 VMA 的 C 代码，-mcmodel=medany
 * 下编译器对 `extern char x[]` 这类外部符号的取地址操作，一律展开成
 * "PC 相对（auipc+addi）"——即认为这个符号的值必然落在当前代码 PC
 * 附近 ±2GiB 内。__kernel_phys_end 的值是 0x80200000 量级的低物理地址，
 * 和这段高 VMA 代码的 PC（0xFFFFFFC0...)相差整个 KERNEL_VIRT_BASE，
 * 编译进去链接时就报 `relocation truncated to fit: R_RISCV_PCREL_HI20`
 * （已经用真实构建触发过一次，不是猜测）。x86_64 版本能直接用
 * __kernel_phys_end 是因为 -mcmodel=kernel 对外部符号取地址走的是
 * "绝对立即数"（mov $imm32, reg，R_X86_64_32S，见 linker.ld 里
 * __bss_start_phys 那段注释的姊妹讨论），和 riscv64 PC 相对的寻址
 * 方式完全不是一回事，不能类比。__kernel_end 因为定义在
 * `. += KERNEL_VIRT_BASE` 之后，值本身就是高 VMA，和这段代码的 PC
 * 天然接近，可以安全地用 auipc+addi 取到。 */
extern char __kernel_end[];
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull
#define KERNEL_LOAD_ADDR 0x80200000ull

/* 用来演示"非身份映射"的一段测试用虚拟地址：故意选一个不落在内核自
 * 映射范围内、也不是任何身份映射范围内的高地址，纯粹用来验证
 * pagetable_map()/pagetable_lookup() 的映射关系是不是"VA 和 PA 可以
 * 不相等"，不代表这段地址在真实系统里有什么意义。Sv39 canonical
 * address 规则（bit 38 及以上必须全 1）要求高半区地址至少是
 * 0xFFFFFFC000000000 起——这里用 KERNEL_VIRT_BASE 加一段偏移，保证
 * 落在合法的高半区范围内。 */
#define TEST_VA (KERNEL_VIRT_BASE + 0x10000000ull)

/* 演示用：故意访问一个从未映射过的地址，触发 page fault，验证
 * trap_init() 注册的 supervisor_trap_handler 真的会被调用到——用
 * volatile 指针读一次，避免编译器把这次"看起来毫无意义"的读操作
 * 优化掉。 */
static volatile uint8_t *const fault_addr = (volatile uint8_t *)0xdead0000ull;

/* OpenSBI 交接控制权时 a0=hartid、a1=DTB 物理地址，boot.S 的
 * boot_stage2_high 把 s0/s1（阶段 A 一开始存的原始值）原样挪进
 * a0/a1 再 call kernel_boot——具体见 boot.S 里对应位置的注释。 */
void kernel_boot(uint64_t hartid, uint64_t dtb_paddr)
{
    (void)hartid;
    (void)dtb_paddr;

    console_puts_line("Hello OS from riscv64 (Lab4: virtual memory)");

    /* 和 x86_64 版本不同：memmap_discover() 已经在 boot.S 阶段 A（打开
     * 分页之前）调用过了——原因见 boot.S 顶部注释：QEMU 的 riscv64 DTB
     * 被放在 RAM 顶端往下 2MiB 的地方，阶段 A/B 的临时身份映射只覆盖
     * 低 1GiB 左右，够不到那么远，所以只能在还没开分页（satp=0，直接
     * 物理地址寻址）的时候读它。这里不需要、也不能再调用一次。 */

    uintptr_t kernel_end_phys = (uintptr_t)__kernel_end - KERNEL_VIRT_BASE;
    kprintf("kernel image: phys [%p, %p)\n", (uintptr_t)KERNEL_LOAD_ADDR,
            kernel_end_phys);

    /* 建正式页表：只做内核自映射（虚拟地址 = 物理地址 + KERNEL_VIRT_BASE，
     * 覆盖内核实际占用的物理范围），不包含任何低地址身份映射——这就是
     * "跳转后撤掉低地址映射"这个设计决定的落地方式：新表从一开始就没有
     * 写入低地址项，不是事后删除。切换过去（pagetable_activate()）之后，
     * 低地址的临时身份映射永久失效。 */
    uintptr_t root = pagetable_create();

    uintptr_t phys_start = KERNEL_LOAD_ADDR;
    uintptr_t phys_end = kernel_end_phys;
    for (uintptr_t pa = phys_start; pa < phys_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    /* 自映射范围故意比内核镜像本身（到 kernel_end_phys 为止)多留一段
     * 到 2MiB 窗口边界：pagetable.c 里 walk() 需要把 root/中间层节点
     * 的物理地址翻译成 phys+KERNEL_VIRT_BASE 这个别名才能解引用（见
     * 那边的详细注释),而这些中间层节点是 kalloc_page() 现场分配的,
     * 物理地址落在 kernel_end_phys 之后——如果自映射只覆盖到
     * kernel_end_phys,pagetable_activate() 切换过去之后,walk() 第一次
     * 需要访问 pagetable_create()/上面几次 pagetable_map() 已经分配好
     * 的节点（比如下面 pagetable_lookup(TEST_VA))时,那段地址在新表里
     * 根本没有映射,会直接 page fault——实测在 QEMU 下确认过。2MiB 是
     * 和 boot.S 阶段 A 临时页表同一个边界（那边 boot_l1_high[1]->
     * boot_leaf 整段 2MiB 都是 RWX,足够覆盖这里会用到的少数几个页表
     * 节点),不是任意选的数字,选它是为了和阶段 A 保持一致、不引入新的
     * 边界常量。 */
    uintptr_t self_map_end = KERNEL_LOAD_ADDR + 0x200000ull;
    for (uintptr_t pa = phys_end; pa < self_map_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    /* UART（物理 0x10000000)身份映射：console_putc.c 里直接解引用
     * 这个物理地址访问串口寄存器,riscv64 的串口是内存映射 I/O,不像
     * x86_64 版本走 in/out 端口指令那样天然绕开页表——一旦下面
     * pagetable_activate() 切换过去,任何没被显式映射的物理地址都
     * 不可访问,包括 UART。boot.S 阶段 A 的临时页表里已经装了同一份
     * 身份映射（为了让 pagetable_activate() 切换*之前*那段窗口期,
     * 也就是本函数开头 console_puts_line("Hello OS...") 那次调用,
     * 能正常输出),但那份临时表切换过去之后就永久失效了（本函数
     * 后面 "low identity map gone" 那行注释说的就是这件事)——这里
     * 必须在正式页表里独立再映射一次,不能指望阶段 A 的临时映射续命
     * 到这里。不给 EXECUTABLE：MMIO 寄存器不是代码。 */
    pagetable_map(root, 0x10000000ull, 0x10000000ull, PTE_FLAG_WRITABLE);

    /* 再手写映射一页"非身份"关系：VA=TEST_VA，PA 借用内核本身第一页
     * 的物理地址（内容不重要，只是要一个已知合法的物理页），验证
     * pagetable_lookup() 翻译出来的物理地址和我们写进去的一致，且
     * TEST_VA 本身不等于 phys_start——这就是"VA 和 PA 可以不相等"的
     * 直接证据，不是靠自映射范围内 VA=PA+常量 这种规律性关系去混淆。 */
    pagetable_map(root, TEST_VA, phys_start, PTE_FLAG_WRITABLE);

    pagetable_activate(root);

    console_puts_line("switched to Lab4 page table, low identity map gone");

    uintptr_t looked_up = pagetable_lookup(root, TEST_VA);
    if (looked_up != phys_start) {
        panic("pagetable_lookup(TEST_VA) did not return the mapped physical address");
    }
    kprintf("non-identity mapping OK: VA=%p -> PA=%p\n", (uintptr_t)TEST_VA,
            looked_up);

    uintptr_t self_map_check = pagetable_lookup(root, phys_start + KERNEL_VIRT_BASE);
    if (self_map_check != phys_start) {
        panic("kernel self-map lookup mismatch");
    }
    console_puts_line("kernel self-map verified");

    /* 注册陷入处理函数，然后故意触发一次页错误——这是本 Lab 新增的
     * 陷阱基础设施的最小验证：确认 stvec 真的生效、
     * supervisor_trap_handler 真的被 CPU 调用到，而不是随便挂了个
     * 函数指针从来没被用过。 */
    trap_init();
    console_puts_line("trap_init() done, about to trigger a deliberate page fault");

    uint8_t value = *fault_addr;
    (void)value;

    panic("unreachable: supervisor_trap_handler should have panicked already");
}
