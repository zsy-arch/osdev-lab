/* Lab4 starter (x86_64)：最窄的中断/异常基础设施——只为了接住 #PF
 * （page fault,向量 14）这一个异常，不是完整的 32+ 项 IDT/GDT+TSS
 * 搭建。完整的中断基础设施（时钟中断、完整 IDT、GDT+TSS）是 Lab5 的
 * 教学内容，这里提前全部搭好会让 Lab5 没有新东西可讲。
 *
 * IDT（Interrupt Descriptor Table）本身的形状（Intel SDM Vol.3A
 * "6.11 IDT Descriptors"）：
 *
 *   每个 entry 是 16 字节的 gate descriptor（64 位模式下所有 gate 都是
 *   16 字节，跟 32 位模式的 8 字节不一样）：
 *
 *     offset 0-1   : handler 地址 bits 0-15
 *     offset 2-3   : segment selector（指向 GDT，这里恒为 0x08，也就是
 *                    boot.S 里 gdt64 定义的 64 位代码段）
 *     offset 4     : IST（Interrupt Stack Table）索引，本 Lab 不用，恒 0
 *     offset 5     : type/attr byte——bit 7 是 Present，bits 0-3 是
 *                    gate type（0xE = 64 位 interrupt gate，进入时自动
 *                    关中断，这是本 Lab 唯一用到的类型）
 *     offset 6-7   : handler 地址 bits 16-31
 *     offset 8-11  : handler 地址 bits 32-63
 *     offset 12-15 : reserved，必须是 0
 *
 * 只填 32 项里的第 14 项（#PF），其余 31 项全部留空（Present bit = 0）。
 * 触发任何一个没填的向量，CPU 会发现 IDT entry 的 Present bit 是 0，
 * 转而尝试送一个 #GP（13 号），如果 #GP 那项也没填（确实没填），会
 * 再转成 double fault（8 号），同样没填，就是 triple fault——真实
 * 硬件上 triple fault 会触发 CPU 复位，QEMU 则通常直接把整个虚拟机重启。
 * 完整背景见 README.md「核心概念」与「常见坑与排查」两节。
 */
#include "types.h"
#include "console.h"
#include "panic.h"

/* gdt_init()：boot.S 的 Stage B 里 `lgdt gdt64_pointer` 加载的那份 GDT
 * （boot.S 里的 gdt64）故意放在 .text.boot 这个 VMA=LMA=低物理地址的
 * section 里——lgdt 只在那一刻执行过一次，之后 GDTR 里存的 base 一直
 * 是那个低物理地址，从来没有被重新加载过。
 *
 * pagetable_activate() 切到 Lab4 正式页表、撤掉低地址身份映射之后，
 * 这个低物理地址就成了悬空指针——Intel SDM Vol.3A "6.12.1
 * Exception-or Interrupt-Handler Procedures" 规定，通过 interrupt
 * gate 递送异常时，CPU 要用 IDT gate 里的 selector 去 GDT 里取对应的
 * segment descriptor，加载进 CS——也就是说，*任何*异常/中断的递送都
 * 会触发一次 GDT 读取，不是"侥幸没用到就没事"。
 *
 * 这是一个不容易 organically 发现的系统性陷阱（本 Lab 开发过程中已经
 * 在 QEMU 下实测触发过一次：本 Lab 故意触发的那次 #PF 递送时，CPU
 * 试图读 GDT 时正好落在"低身份映射已撤销"的范围内，直接级联成
 * triple fault，从来没能真正跑到 page_fault_handler() 里面）——已经
 * 写好，不是 TODO，让你专注在 IDT/#PF 处理这个本 Lab 真正的教学内容
 * 上。完整背景见 README.md「常见坑与排查」一节。
 *
 * 修复方式：在 pagetable_activate() 之后，用一份新的、定义在普通
 * （高 VMA）.bss/.data 里的 GDT 重新 lgdt 一次——效果和 idt_init() 一样
 * （idt[] 本来就是普通 .bss 静态数组，取地址天然就是高 VMA，从来没有
 * 这个问题）。GDT 内容和 boot.S 的 gdt64 完全一致（null descriptor +
 * 一条 64 位、非一致性代码段描述符），只是换了个存放位置。 */
static uint64_t gdt[2] = {
    0,                       /* index 0: null descriptor，架构强制要求 */
    0x00AF9A000000FFFFull,   /* index 1 (selector 0x08): 64 位代码段，
                              * 和 boot.S 里 gdt64 第二项完全相同的编码。 */
};

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void)
{
    static struct gdt_pointer gdtp;
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uintptr_t)gdt;

    __asm__ volatile("lgdt %0" : : "m"(gdtp));
}

#define IDT_ENTRIES    32
#define IDT_TYPE_INT64 0x8E /* Present(1) | DPL(00) | 0 | type(1110) */
#define IDT_VEC_PAGE_FAULT 14

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 32 项，其中 31 项恒为全 0（Present=0，触发即转 #GP/double fault），
 * 只有 IDT_VEC_PAGE_FAULT 那一项会被下面 idt_init() 填上。静态存储，
 * 已经写好，不是 TODO。 */
static struct idt_entry idt[IDT_ENTRIES];

/* #PF 的 error code 各 bit 含义（Intel SDM Vol.3A "4.7 Page-Fault
 * Exceptions"，Table 4-14）：
 *   bit 0 P    ：0 = 因为 not-present 触发，1 = 因为违反了访问权限触发
 *   bit 1 W/R  ：0 = 触发的是读操作，1 = 触发的是写操作
 *   bit 2 U/S  ：0 = 触发时 CPU 在内核态，1 = 在用户态
 * 已经写好，不是 TODO。 */
#define PF_ERR_PRESENT (1u << 0)
#define PF_ERR_WRITE   (1u << 1)
#define PF_ERR_USER    (1u << 2)

/* CPU 触发 #PF 时把出错的虚拟地址放在 CR2 里（不是通过 error code 或者
 * 栈上的参数传递——这是 x86 架构里少数几个"额外信息走专用寄存器不走
 * 调用约定"的例外，读 CR2 必须显式用内联汇编，编译器不会自动帮你读）。
 * 已经写好，不是 TODO。 */
static uintptr_t read_cr2(void)
{
    uintptr_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

/* TODO 1：page_fault_handler() —— trap_entry.S 的 page_fault_stub 会把
 * CPU 自动压栈的 error code 按 System V AMD64 调用约定传进来（第一个
 * 参数，%rdi）。
 *
 * 用 read_cr2() 读出触发异常的虚拟地址，配合 error_code 打印诊断信息，
 * 然后 panic()——本 Lab 的 #PF 处理策略是"能识别、能报告，但不修复"
 * （按需分页/写时复制之类"修复后返回重跑那条指令"的策略是后面 Lab
 * 的内容）。
 *
 * 提示：
 *   void page_fault_handler(uint64_t error_code)
 *   {
 *       uintptr_t fault_addr = read_cr2();
 *
 *       kprintf("page fault: addr=%p error_code=%lx (%s%s%s)\n",
 *               (void *)fault_addr, error_code,
 *               (error_code & PF_ERR_PRESENT) ? "protection-violation" : "not-present",
 *               (error_code & PF_ERR_WRITE) ? ",write" : ",read",
 *               (error_code & PF_ERR_USER) ? ",user" : ",kernel");
 *
 *       panic("page_fault_handler: unrecoverable page fault (Lab4 does not implement fault recovery)");
 *   }
 */


/* trap_entry.S 定义的汇编 stub 入口，唯一职责是把 CPU 送进来的信息
 * 转换成上面 page_fault_handler() 能用的 C 调用约定形式。已经写好，
 * 不是 TODO。 */
extern void page_fault_stub(void);

/* TODO 2：idt_set_entry() —— 把一个 IDT entry 填成"指向 handler 的
 * 64 位 interrupt gate"。handler 地址要拆成三段分别填进
 * offset_low/offset_mid/offset_high（IDT entry 的形状见文件头注释）。
 *
 * 提示：
 *   static void idt_set_entry(int vector, void (*handler)(void))
 *   {
 *       uintptr_t addr = (uintptr_t)handler;
 *
 *       idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
 *       idt[vector].selector    = 0x08; // boot.S gdt64 里的 64 位代码段
 *       idt[vector].ist         = 0;
 *       idt[vector].type_attr   = IDT_TYPE_INT64;
 *       idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
 *       idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
 *       idt[vector].reserved    = 0;
 *   }
 */


/* TODO 3：idt_init() —— 调 idt_set_entry() 填第 14 项（#PF）,然后
 * lidt。
 *
 * idt[] 是 .bss 里的静态数组，boot.S 的 Stage A 已经清过整段 .bss，
 * 这里不需要再手动清零——"整项全 0"正好等价于"Present=0 的空
 * entry"，这是零值恰好安全的例子。
 *
 * 提示：
 *   void idt_init(void)
 *   {
 *       idt_set_entry(IDT_VEC_PAGE_FAULT, page_fault_stub);
 *
 *       static struct idt_pointer idtp;
 *       idtp.limit = sizeof(idt) - 1;
 *       idtp.base = (uintptr_t)idt;
 *
 *       __asm__ volatile("lidt %0" : : "m"(idtp));
 *   }
 */
