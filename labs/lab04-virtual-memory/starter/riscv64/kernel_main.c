/* Lab4 starter (riscv64)：把内核切到自己管理的 Sv39 页表,
 * 挂上异常处理,再故意触发一次页故障验证效果。
 */
#include "console.h"
#include "panic.h"
#include "kalloc.h"
#include "pagetable.h"

/* trap.c 没有对应的共享头,理由和 x86_64 那份 kernel_main.c 里
 * idt_init() 一样：riscv64 用 stvec 注册陷入入口,x86_64 用 IDT,
 * 两者没有共同接口可抽象,就地声明。 */
void trap_init(void);

/* 和 x86_64 版本一样,不能用 __kernel_phys_end——riscv64 用
 * -mcmodel=medany,访问离当前 PC 超过 2GiB 的符号时,编译器生成的
 * auipc+addi 寻址范围不够,链接时会报
 * "relocation truncated to fit: R_RISCV_PCREL_HI20"（这是实测触发
 * 过的真实报错,不是猜测)。__kernel_end 本身没有这个问题——它就在
 * 当前代码附近,PC 相对寻址够用。已经写好,不是 TODO。 */
extern char __kernel_end[];

#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull
#define KERNEL_LOAD_ADDR 0x80200000ull

#define TEST_VA (KERNEL_VIRT_BASE + 0x10000000ull)

static volatile uint32_t * const fault_addr = (volatile uint32_t *)(TEST_VA + 0x1000ull);

void kernel_boot(uint64_t hartid, uint64_t dtb_paddr)
{
    /* hartid/dtb_paddr 在 boot.S 阶段 A 里已经被 memmap_discover()
     * 消费过了——这里收到的只是同样的两个值,本身不再需要用。 */
    (void)hartid;
    (void)dtb_paddr;

    kprintf("Hello OS from riscv64 (Lab4: virtual memory)\n");

    /* 注意：这里*不*调用 memmap_discover()——和 x86_64 版本不同,
     * riscv64 的 memmap_discover() 已经在 boot.S 阶段 A 里,趁临时
     * 身份映射还在、来得及处理设备树的时候调用过了。等代码执行到这
     * 里,内存探测结果已经就位,直接用就行。 */

    uintptr_t kernel_end_phys = (uintptr_t)__kernel_end;
    kprintf("kernel image: [0x%lx, 0x%lx)\n", (uint64_t)KERNEL_LOAD_ADDR, (uint64_t)kernel_end_phys);

    /* TODO 1：新建一张根页表,把内核自身的物理地址范围原样（虚拟地址
     * == 物理地址)映射进去。
     *
     * 和 x86_64 版本同样的原因：pagetable_activate() 切换页表的那
     * 一刻,CPU 正在执行的代码本身（当前 PC 指向的物理地址)必须在新
     * 页表里也有映射,否则切换瞬间自己把自己的执行路径切断。
     *
     * 提示：
     *   uintptr_t root = pagetable_create();
     *
     *   uintptr_t phys_start = (uintptr_t)KERNEL_LOAD_ADDR;
     *   uintptr_t phys_end = kernel_end_phys;
     *   for (uintptr_t phys = phys_start; phys < phys_end; phys += 0x1000ull) {
     *       pagetable_map(root, phys, phys, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
     *   }
     */

    /* TODO 2：三件事——扩大自映射窗口、映射 UART、映射一个非身份对照
     * 用的测试地址。
     *
     * 1) 扩大自映射窗口到 2MiB,和 boot.S 里临时页表覆盖的窗口大小
     *    对齐（KERNEL_LOAD_ADDR 到 KERNEL_LOAD_ADDR+0x200000)——原因
     *    与 x86_64 版本相同：table_ptr() 依赖内核自身的物理地址空间
     *    在新页表里也能通过"物理地址+KERNEL_VIRT_BASE"这条别名访问
     *    到,窗口开小了后续再分配页表节点可能落在窗口外。
     *
     * 2) 映射 UART（0x10000000,0x10000000,一一对应,只给
     *    PTE_FLAG_WRITABLE)——这一步在 x86_64 版本里*不存在*：x86_64
     *    的 UART 走端口 I/O（in/out 指令),完全不经过分页硬件,开不
     *    开分页都能直接访问;riscv64 的 UART 是 MMIO（内存映射 I/O),
     *    访问它和访问普通内存走的是同一套地址翻译流程——boot.S 阶段
     *    A 的临时身份映射本来能访问到它,但 pagetable_activate()
     *    切换到这张新页表之后,临时映射就作废了,如果新页表里没有
     *    UART 的映射,kprintf() 会在切换后的第一次输出尝试里直接
     *    fault。
     *
     * 3) 映射 TEST_VA -> phys_start（非身份映射,用来后面验证"一个
     *    虚拟地址查出来的物理地址确实是我们指定的那个,不是巧合")。
     *
     * 提示：
     *   uintptr_t self_map_end = (uintptr_t)KERNEL_LOAD_ADDR + 0x200000ull;
     *   for (uintptr_t phys = phys_end; phys < self_map_end; phys += 0x1000ull) {
     *       pagetable_map(root, phys, phys, PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE);
     *   }
     *
     *   pagetable_map(root, 0x10000000ull, 0x10000000ull, PTE_FLAG_WRITABLE);
     *
     *   pagetable_map(root, TEST_VA, phys_start, PTE_FLAG_WRITABLE);
     */

    /* TODO 3：切换到新页表。
     *
     * 和 x86_64 版本相比少一步：riscv64 没有 GDT 这种东西,不需要
     * 额外的"gdt_init()"式收尾调用——satp 写完、sfence.vma 刷完,
     * 地址翻译立刻用新规则,不涉及任何和分段相关的状态。
     *
     * 提示：
     *   pagetable_activate(root);
     *   kprintf("switched to Lab4 page table (Sv39, root=0x%lx)\n", root);
     */

    /* TODO 4：验证 TEST_VA 确实翻译到 phys_start,不是别的地址。
     *
     * 提示：
     *   uintptr_t looked_up = pagetable_lookup(root, TEST_VA);
     *   if (looked_up != phys_start) {
     *       panic("kernel_boot: pagetable_lookup(TEST_VA) returned 0x%lx, expected 0x%lx", looked_up, phys_start);
     *   }
     *   kprintf("pagetable_lookup(TEST_VA) = 0x%lx (matches phys_start)\n", looked_up);
     */

    /* TODO 5：验证内核自映射区间里随便一个地址,查出来的物理地址和
     * 虚拟地址相等（因为这是身份映射)。
     *
     * 提示：
     *   uintptr_t self_check_va = phys_start + 0x1000ull;
     *   uintptr_t self_check_phys = pagetable_lookup(root, self_check_va);
     *   if (self_check_phys != self_check_va) {
     *       panic("kernel_boot: identity self-map check failed, va=0x%lx looked up to 0x%lx", self_check_va, self_check_phys);
     *   }
     *   kprintf("identity self-map check passed at 0x%lx\n", self_check_va);
     */

    /* TODO 6：开异常处理,然后故意踩一次页故障验证整条链路。
     *
     * fault_addr 指向 TEST_VA+0x1000,这个地址不在任何已建立的映射
     * 范围内（TEST_VA 本身映射了一页,+0x1000 越到了下一页,没映射
     * 过),对它解引用必然触发 load page fault,交给
     * supervisor_trap_handler() 处理、打印诊断、panic。panic() 之后
     * 不会返回,所以这里不需要（也不能)再写后续代码。
     *
     * 提示：
     *   trap_init();
     *   kprintf("trap_init() done, triggering a deliberate page fault...\n");
     *   uint32_t value = *fault_addr;
     *   (void)value;
     */
}
