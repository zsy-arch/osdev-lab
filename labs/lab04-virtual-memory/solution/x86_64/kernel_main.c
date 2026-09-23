#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"

/* 架构专属实现，理由和 Lab3 一样：两个架构的参数类型不同，没有共同签名
 * 可以放进共享头。 */
void memmap_discover(uint32_t mb2_info_addr);

/* trap.c 没有对应的共享头——page_fault_handler 的注册方式（IDT）是
 * x86_64 专属的，riscv64 用 stvec，两者没有共同接口可抽象，就地声明。 */
void idt_init(void);

/* boot.S 的 gdt64 放在低物理地址（.text.boot），pagetable_activate()
 * 撤掉低身份映射之后就变成悬空指针——gdt_init() 用一份高 VMA 的新 GDT
 * 重新 lgdt，详见 trap.c 里 gdt_init() 的完整注释。 */
void gdt_init(void);

/* linker.ld 定义的链接期符号：__kernel_end 是内核镜像（.text 到
 * .bss/栈）在高 VMA 视角下的结束地址，用来决定自映射要覆盖多大范围。
 * 链接器符号在 C 里只能声明成数组类型的 extern，取它的地址（不是内容）
 * 才是真正想要的值——这是标准做法，不是这里独创的技巧。
 *
 * 注意：不用 linker.ld 里的 __kernel_phys_end——那个符号按其注释所写
 * 是专门给 boot.S 在阶段 A/B（分页还没开，取绝对物理地址）用的常量。
 * 这里虽然因为 -mcmodel=kernel 对外部符号取地址走的是绝对立即数
 * （mov $imm32, reg，R_X86_64_32S，能装下任何 32 位有符号立即数，
 * 不区分是高 VMA 还是低物理地址），两个符号在 x86_64 上其实都能用、
 * 编译链接都不会报错，但同样的写法搬到 riscv64（-mcmodel=medany，
 * 外部符号取地址走 PC 相对的 auipc+addi，只能装下"离当前 PC ±2GiB"的
 * 值）就会因为 __kernel_phys_end 是低物理地址而链接失败（真的构建
 * 触发过 `relocation truncated to fit: R_RISCV_PCREL_HI20`）。两边保持
 * "C 代码只用高 VMA 符号"这个统一规则，不依赖 x86_64 这边偶然能忍受
 * 混用的addressing mode。 */
extern char __kernel_end[];
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ull
#define KERNEL_LOAD_ADDR 0x100000ull

/* 用来演示"非身份映射"的一段测试用虚拟地址：故意选一个不落在内核自
 * 映射范围内、也不是任何身份映射范围内的高地址，纯粹用来验证
 * pagetable_map()/pagetable_lookup() 的映射关系是不是"VA 和 PA 可以
 * 不相等"，不代表这段地址在真实系统里有什么意义。 */
#define TEST_VA 0xFFFFFFFF90000000ull

/* 演示用：故意访问一个从未映射过的地址，触发 #PF，验证 idt_init() 注册
 * 的 page_fault_handler 真的会被调用到——用 volatile 指针读一次，避免
 * 编译器把这次"看起来毫无意义"的读操作优化掉。 */
static volatile uint8_t *const fault_addr = (volatile uint8_t *)0xdead0000ull;

/* GRUB 把 Multiboot2 info 结构体的物理地址通过 %ebx 交给 _start，
 * boot.S 的 boot_stage2_high 把它原样转交进 %edi——具体见 boot.S 里
 * _start64 那条 mov %ebx, %r15d 上面的注释。 */
void kernel_boot(uint32_t mb2_info_addr)
{
    console_puts_line("Hello OS from x86_64 (Lab4: virtual memory)");

    /* 阶段 D：CR3 现在还是 boot.S 阶段 B 留下的临时表（同时有低 1GiB
     * 身份映射和高地址前 1GiB 自映射），memmap_discover() 要读的
     * Multiboot2 info 结构体在低物理地址，靠这份临时表的低身份映射
     * 部分就能读到，不需要等正式页表建好——这正是 boot.S 顶部注释里
     * "阶段 D" 那段说的时序。 */
    memmap_discover(mb2_info_addr);

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

    /* 自映射范围故意比内核镜像本身（到 kernel_end_phys 为止)多留一段：
     * pagetable.c 里 walk() 需要把 root/中间层节点的物理地址翻译成
     * phys+KERNEL_VIRT_BASE 这个别名才能解引用（见那边的详细注释),
     * 而这些中间层节点是 kalloc_page() 现场分配的,物理地址落在
     * kernel_end_phys 之后——如果自映射只覆盖到 kernel_end_phys,
     * pagetable_activate() 切换过去之后,walk() 第一次需要访问
     * pagetable_create()/上面几次 pagetable_map() 已经分配好的节点
     * （比如下面 pagetable_lookup(TEST_VA))时,那段地址在新表里根本
     * 没有映射,会直接 page fault——riscv64 版本已经在 QEMU 下实测
     * 触发过这个 bug（同一个 walk() 设计,同一个漏洞),x86_64 这里是
     * 提前把同一个坑填掉,不是等实测踩到了才修。2MiB 和 riscv64 版本
     * 用的边界保持一致（那边受限于阶段 A 临时页表只给了 2MiB 的
     * RWX 窗口，这边阶段 A/B 的临时页表其实给了整 1GiB,可以选更大
     * 的边界,但没必要——教学场景下这里只会分配个位数的页表节点,
     * 2MiB 已经是远超实际需要的余量,选它只是为了两个架构的 Lab4
     * 代码在这一点上看起来是同一个设计决定,不是各自随便挑了个数)。 */
    uintptr_t self_map_end = KERNEL_LOAD_ADDR + 0x200000ull;
    for (uintptr_t pa = phys_end; pa < self_map_end; pa += PAGE_SIZE) {
        uintptr_t va = pa + KERNEL_VIRT_BASE;
        pagetable_map(root, va, pa, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
    }

    /* 再手写映射一页"非身份"关系：VA=TEST_VA，PA 借用内核本身第一页
     * 的物理地址（内容不重要，只是要一个已知合法的物理页），验证
     * pagetable_lookup() 翻译出来的物理地址和我们写进去的一致，且
     * TEST_VA 本身不等于 phys_start——这就是"VA 和 PA 可以不相等"的
     * 直接证据，不是靠自映射范围内 VA=PA+常量 这种规律性关系去混淆。 */
    pagetable_map(root, TEST_VA, phys_start, PTE_FLAG_WRITABLE);

    pagetable_activate(root);

    /* 必须紧跟在 pagetable_activate() 之后、任何可能触发异常/中断的
     * 操作之前完成——boot.S 加载的 GDT base 是低物理地址，低身份映射
     * 一撤销就变成悬空指针，而"递送异常需要重读 GDT 取 CS descriptor"
     * 这件事不受任何显式调用触发，是 CPU 硬件在异常发生那一刻自动做的
     * （详见 trap.c 里 gdt_init() 的注释），没法通过"晚一点再调用
     * gdt_init()"来错开，只能保证它比第一次可能的异常更早执行。 */
    gdt_init();

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

    /* 注册 #PF 处理函数，然后故意触发一次——这是本 Lab 新增的陷阱基础
     * 设施的最小验证：确认 IDT 真的生效、page_fault_handler 真的被
     * CPU 调用到，而不是随便挂了个函数指针从来没被用过。 */
    idt_init();
    console_puts_line("idt_init() done, about to trigger a deliberate page fault");

    uint8_t value = *fault_addr;
    (void)value;

    panic("unreachable: page_fault_handler should have panicked already");
}
