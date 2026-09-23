/* Lab6 x86_64：在 Lab5"IDT 只处理 #PF + 定时器"的基础上，加入真正的
 * 特权级切换机制——SYSCALL/SYSRET。这是全课程唯一一个不通过 IDT 的
 * 陷入路径：IDT/中断门是"CPU 硬件自动查表跳转"，SYSCALL 是"CPU 直接
 * 从固定的 MSR（IA32_LSTAR）里取 RIP"，两者互不相关，共存不冲突——
 * 本 Lab 的 #PF/定时器仍然走 Lab5 原样的 IDT 路径，SYSCALL 是新增的
 * 第三条陷入路径,不是对前两条的替换。
 *
 * 这个文件新增的内容分两部分：
 *   1. GDT 从 2 项扩到 5 项——不是"随便加几个描述符"，SYSCALL/SYSRET
 *      按 IA32_STAR 的位布局用固定算式推导 CS/SS selector（不查表，
 *      CPU 从 STAR 里取出 16 位 base 直接加偏移量拼出 selector），
 *      GDT 里对应位置必须真的有正确 DPL 的描述符，否则装载 CS/SS 那
 *      一步（SYSRET 是"设置 CS/SS 的段选择子，但不真的按 GDT 描述符
 *      重新加载 limit/属性 cache"——Intel/AMD 两家手册对这一点的描述
     * 略有出入，但都要求 GDT 里对应槅位有效，不能是 0/不存在）会立刻
     * #GP，且这个 #GP 发生在 sysret 指令本身，此时已经处于"正在切换
     * 特权级"的中间状态，调试起来比普通 #GP 更难定位。
   2. IA32_EFER.SCE / IA32_STAR / IA32_LSTAR / IA32_FMASK 四个 MSR 的
      编程——SYSCALL/SYSRET 指令本身不接受任何操作数，全部参数都来自
      这几个 MSR，这是"固定寄存器的秘密协议"而不是常规的函数调用
      约定，本 Lab 的核心教学点之一就是把这个隐藏协议显式地摆出来。
 *
 * Lab7 新增第三部分：TSS（Task State Segment）+ TSS.RSP0。
 *
 * 这不是本 Lab 顺手加的可选优化，是补一个 Lab6 遗留至今、真实存在的
 * 崩溃坑：Lab1-6 全程从未 `ltr` 过任何 TSS，TR 寄存器一直是复位后的
 * 无效值。IDT 中断门（#PF/定时器走的那条路径）如果在 CPL3 触发、需要
 * 提权到 CPL0，硬件必须知道"提权之后用哪个内核栈"——这个信息只能来自
 * TSS.RSP0，没有装载有效 TSS 时这个字段是垃圾/未定义。实测验证过：
 * Lab6 的用户程序在 sys_exit 之后陷入 ring3 忙等死循环，只要有一次
 * PIT 定时器 tick 恰好落在这段 ring3 执行期间（很快就会发生——100Hz，
 * 循环体只有一条 jmp），中断门提权那一刻硬件会把中断帧压到一个几乎是
 * 地址 0 附近的位置，直接 #PF，再 #PF 递归成 #DF，最终三重故障、QEMU
 * 复位——但复位之后的固件输出走的是 VGA 而不是串口（这门课全程用
 * `-serial stdio -display none`），所以串口日志看起来"干净地卡在
 * hlt"，实际是"已经三重故障重启进了看不见的固件"，两者从串口的角度
 * 完全无法区分（`-d cpu_reset` 才能看见真正发生的 CPU Reset）。
 *
 * Lab7 引入的多进程/调度场景，用户态代码会长时间运行、被定时器反复
 * 打断（这正是"轮转调度"的机制本身），不装 TSS.RSP0 等于每个进程迟早
 * 会撞上这个坑，不是"可能触发的边界情况"，是"教学场景下必然会触发的
 * 主路径"，所以本 Lab 必须补上：
 *   - GDT 从 6 项扩到 8 项（TSS 描述符在 64 位模式下占两个 GDT 槅位，
 *     不是一个——64 位 TSS 描述符本身就是 16 字节，是 8 字节描述符的
 *     两倍，GDT 里"一项"的槅位大小是固定 8 字节，所以要占两项）。
 *   - `struct tss64`（只填 RSP0，其余字段本 Lab 不用，符合 Intel 手册
 *     "64 位模式下大部分 TSS 字段被硬件忽略,只有 RSP0-2/IST1-7/IOPB
 *     还有意义"这一事实，本课程只用得到 RSP0)。
 *   - `tss_init()`：填 TSS 描述符、`ltr` 装载 TR。
 *   - `tss_set_rsp0(uintptr_t rsp0)`：调度器在 swtch() 到某个进程之前
 *     调用，把 TSS.RSP0 更新成"这个进程自己的内核栈顶"——这样下一次
 *     它在用户态被中断门打断时，硬件会自动切到它自己的内核栈，而不是
 *     上一个进程的（或者像 Lab6 那样,压根没有任何有效栈）。
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "pit.h"
#include "syscall.h"
#include "proc.h"

/* TODO 1：把 GDT 从 Lab6 的 6 项扩到 8 项（新增 index 6-7 两项 TSS
 * 描述符占位），并补一个 GDT_SEL_TSS 宏。前 6 项和其余 selector 宏跟
 * Lab6 完全一样,直接沿用即可。
 *
 * GDT 布局——顺序不是随便排的，SYSCALL/SYSRET 的 selector 算式把这个
 * 顺序焊死了，改变顺序 = 改变 SYSCALL/SYSRET 的目标 selector,不是纯粹
 * 的代码风格选择：
 *
 *   index 0: 空描述符（x86 架构强制要求，selector 0 永远无效）。
 *   index 1: 内核代码段 CS0（selector 0x08，Lab4/5 就有，DPL=0）。
 *   index 2: 内核数据段 DS0（selector 0x10，Lab5 及之前从来没建过——
 *            Lab4/5 全程 CPL0，%ds/%es/%ss 从 boot.S 起就是 null
 *            selector 从没重新加载过，64 位模式下数据段 base/limit
 *            基本不生效，null selector 硬件不会主动挑错，能跑不代表
 *            这是对的；SYSCALL 进入内核时会把 SS 设成
 *            STAR[47:32]+8，也就是这一项，所以必须是一个真实存在、
 *            DPL=0 的数据段描述符，不能再是 null）。
 *   index 3: 32 位兼容模式用户代码段占位（selector 0x18，DPL=3）——
 *            本课程/QEMU 目标从不运行 32 位用户程序，这一项永远不会
 *            被真正当 CS 用，存在的唯一理由是"占住 SYSRET 算 64 位
 *            用户 CS 时要跳过的那个位置"（SYSRET 算 64 位模式的用户
 *            CS = STAR[63:48]+16，SS = STAR[63:48]+8——SS 和 CS 中间
 *            隔着一个位置，如果不占位，紧接着 SS 后面那一项会被
 *            SYSRET 误当成 CS 用）。这是 Intel/AMD 手册明确要求的
 *            固定间隔，不是本课程自创的排布，见 SYSRET 指令参考手册。
 *   index 4: 用户数据段 SS3（selector 0x20 | 3，DPL=3——SYSRET 算出
 *            的用户 SS）。
 *   index 5: 用户代码段 CS3（selector 0x28 | 3，DPL=3——SYSRET 算出
 *            的用户 CS，64 位长模式，L 位置 1）。
 *   index 6-7: Lab7 新增，TSS 描述符（selector=0x30）——64 位模式下
 *            系统段描述符（TSS 属于这一类，跟 code/data 描述符是两种
 *            不同的格式）是 16 字节宽,占两个 GDT 槅位：低 8 字节是
 *            base[0:23]/limit[0:19]/type/DPL 等,高 8 字节是
 *            base[32:63]（低 8 字节里还有 base[24:31]）+ 保留位。这跟
 *            前面 6 项"每项都是独立的 8 字节 code/data 描述符"不是同
 *            一种格式,不能简单地在数组里再摆一个 64 位常量了事,需要
 *            运行时根据 tss64 结构体的实际地址填,所以这两项在数组里
 *            先占位为 0,`tss_init()` 里用 `gdt[6]`/`gdt[7]` 的地址
 *            当作一个 16 字节缓冲区,运行时填入正确的 base/limit。
 *
 * 描述符的 64 位打包格式跟 Lab4 的内核 CS 那一项完全一样（flat
 * segment，base=0/limit=0xFFFFF/G=1，区别只在 DPL 和 Type/S 位）。
 *
 * 提示：
 * static uint64_t gdt[8] = {
 *     0x0000000000000000ull, // 0: null
 *     0x00AF9A000000FFFFull, // 1: 内核代码段 CS0，selector=0x08，DPL=0，L=1
 *     0x00AF92000000FFFFull, // 2: 内核数据段 DS0，selector=0x10，DPL=0
 *     0x00CFFA000000FFFFull, // 3: 32 位兼容模式用户代码段占位，selector=0x18
 *     0x00CFF2000000FFFFull, // 4: 用户数据段 SS3，selector=0x20，DPL=3
 *     0x00AFFA000000FFFFull, // 5: 用户代码段 CS3，selector=0x28，DPL=3，L=1
 *     0x0000000000000000ull, // 6: TSS 描述符低 8 字节，tss_init() 运行时填
 *     0x0000000000000000ull, // 7: TSS 描述符高 8 字节，tss_init() 运行时填
 * };
 *
 * #define GDT_SEL_KERNEL_CS 0x08
 * #define GDT_SEL_KERNEL_DS 0x10
 * #define GDT_SEL_USER_CS32 0x18
 * #define GDT_SEL_USER_SS   0x20
 * #define GDT_SEL_USER_CS   0x28
 * #define GDT_SEL_TSS       0x30   // Lab7 新增
 * #define GDT_SEL_USER_SS_RPL3 (GDT_SEL_USER_SS | 3)
 * #define GDT_SEL_USER_CS_RPL3 (GDT_SEL_USER_CS | 3)
 */

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* TODO 2：实现 gdt_init。
 *
 * 为什么：跟 Lab6 完全一样，一个字都没改——sizeof(gdt) 自动从 6 项的
 * 48 字节变成 8 项的 64 字节，limit 跟着自动对，所以 Lab7 扩容 GDT 这件
 * 事不需要动 gdt_init() 里的任何一行。这也是"limit 写 sizeof(x) - 1 而
 * 不是写死 47"这个习惯的回报。
 *
 * 提示：
 * void gdt_init(void)
 * {
 *     static struct gdt_pointer gdtp;
 *     gdtp.limit = sizeof(gdt) - 1;
 *     gdtp.base = (uintptr_t)gdt;
 *
 *     __asm__ volatile("lgdt %0" : : "m"(gdtp));
 *
 *     // lgdt 只换了 GDTR，不会自动重新加载 %ds/%es/%ss——这几个段寄存器
 *     // 从 boot.S 起就是 null selector。SYSCALL 只换 CS/SS，不换
 *     // DS/ES/FS/GS，所以这里显式把它们设成内核数据段 selector，不依赖
 *     // "64 位模式碰巧不检查数据段 base/limit"这个事实继续裸奔。
 *     __asm__ volatile(
 *         "mov %0, %%ds\n"
 *         "mov %0, %%es\n"
 *         "mov %0, %%ss\n"
 *         :
 *         : "r"((uint16_t)GDT_SEL_KERNEL_DS));
 * }
 */

/* TODO 3：定义 struct tss64 / static struct tss64 tss，并实现 tss_init。
 *
 * 为什么：这是本 Lab x86_64 侧最核心的新机制，也是 Lab6 能"侥幸"不需要
 * TSS、Lab7 一定需要的分水岭。Lab6 的用户程序只会主动 syscall 进内核，
 * SYSCALL 指令换栈是靠 MSR_LSTAR/软件自己 mov，完全不碰 TSS；Lab7 一旦
 * 有了定时器抢占，用户态会在任意一条指令上被 IDT 中断门打断，而 IDT
 * 中断门做 CPL3→CPL0 提权时，新栈顶只有一个来源：硬件自动去 TR 指向的
 * TSS 结构体里读 RSP0。没有有效的 TSS，第一次定时器中断就是三重故障。
 *
 * 字段布局是 Intel 手册规定的固定偏移，不是本课程自己设计的——reserved0
 * 存在的唯一目的就是把 rsp0 顶到 offset 4，rsp1/rsp2/ist1-7/iomap_base
 * 本 Lab 完全不用（本课程只有"用户态→内核态"这一种提权路径，硬件永远
 * 只读 RSP0），保留为 0 即可。
 *
 * tss_init() 里最容易漏、并且漏了之后极难 debug 的一行是 base[31:24]：
 * 16 字节 TSS 描述符把 base 拆成三段，`(base & 0xFFFFFF) << 16` 只覆盖
 * base[23:0]，剩下的 base[31:24] 单独落在低 8 字节的 bit[63:56]。漏掉
 * 这 8 位等于把 base 的 bit31 清零，而 tss 这个内核全局变量的链接地址是
 * 0xffffffff80109040（bit31=1），截断后变成 0xffffffff00109040，从内核
 * 高地址映射范围里掉出去落进一个完全没映射的 PML4 槽位。ltr 只校验描述符
 * 格式、不校验 base 对不对，所以这个错误在启动阶段一声不响，直到第一次
 * 定时器中断真的要去读 RSP0 才爆发：读 0xffffffff00109044 触发 #PF，而
 * 这次 #PF 发生在"提权换栈"的中途、硬件还没有可用内核栈去压异常帧，于是
 * 同样的 #PF 再来一次（日志里 old:0xe new:0xe），升级 #DF 时又 #GP
 * （old:0x8 new:0xd），最后三重故障 QEMU 复位。如果你看到 CR2 是一个
 * "只差 bit31"的诡异地址，先回来查这一行，别去怀疑页表或调度器。
 *
 * 提示：
 * struct tss64 {
 *     uint32_t reserved0;
 *     uint64_t rsp0;
 *     uint64_t rsp1;
 *     uint64_t rsp2;
 *     uint64_t reserved1;
 *     uint64_t ist1;
 *     uint64_t ist2;
 *     uint64_t ist3;
 *     uint64_t ist4;
 *     uint64_t ist5;
 *     uint64_t ist6;
 *     uint64_t ist7;
 *     uint64_t reserved2;
 *     uint16_t reserved3;
 *     uint16_t iomap_base;
 * } __attribute__((packed));
 *
 * static struct tss64 tss;
 *
 * void tss_init(void)
 * {
 *     // TSS 描述符是系统段描述符，跟前面 code/data 描述符的 8 字节格式
 *     // 不是同一种：64 位模式下它是 16 字节，位域布局是
 *     //   低 8 字节：limit[0:15] | base[0:23] | type(0x9) | DPL | P |
 *     //              limit[16:19] | flags | base[24:31]
 *     //   高 8 字节：base[32:63]，其余 32 位保留（必须是 0）
 *     // S 位（描述符类型位，code/data 描述符里恒为 1）在系统段描述符里
 *     // 是 0——这是 CPU 区分普通段描述符和系统段描述符的方式。
 *     // limit 填 sizeof(tss)-1 而不是 0xFFFFF：TSS 不是 flat segment，
 *     // 它的 limit 就是自己结构体的大小。
 *     uint64_t base = (uintptr_t)&tss;
 *     uint32_t limit = sizeof(tss) - 1;
 *
 *     uint64_t low = 0;
 *     low |= (uint64_t)(limit & 0xFFFFull);
 *     low |= (base & 0xFFFFFFull) << 16;
 *     low |= (uint64_t)0x9ull << 40;   // Type=0x9：可用的 64 位 TSS
 *     low |= (uint64_t)0x0ull << 45;   // DPL=0：只有内核会 ltr
 *     low |= (uint64_t)0x1ull << 47;   // P=1：段存在
 *     low |= ((uint64_t)(limit >> 16) & 0xFull) << 48;
 *     low |= ((base >> 24) & 0xFFull) << 56;  // 别漏这一行，见上面
 *
 *     uint64_t high = (base >> 32) & 0xFFFFFFFFull;
 *
 *     gdt[6] = low;
 *     gdt[7] = high;
 *
 *     tss.rsp0 = 0;                 // scheduler() 会在真正切换前填上
 *     tss.iomap_base = sizeof(tss); // 指向结构体末尾之外 = 没有 IOPB
 *
 *     __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_SEL_TSS));
 * }
 */

/* TODO 4：实现 tss_set_rsp0。
 *
 * 为什么：调度器在 swtch() 切换到某个进程之前调用它，把 TSS.RSP0 指向
 * "这个进程自己的内核栈顶"。之后这个进程在用户态被任何 IDT 中断门打断、
 * 需要 CPL3→CPL0 提权时，硬件会自动用这个 RSP0 当新栈顶——这就是"每个
 * 进程有自己独立内核栈"在硬件层面真正落地的那一行。不调用它，所有进程
 * 共享同一个（或者像 Lab6 一样根本无效的）RSP0，多进程一旦被定时器打断
 * 就是栈踩踏或者 TODO 3 里描述的三重故障。
 *
 * 函数体只有一行，真正的难点在于"调度器什么时候调用它"——见 proc.c 的
 * scheduler()。
 *
 * 提示：
 * void tss_set_rsp0(uintptr_t rsp0)
 * {
 *     tss.rsp0 = rsp0;
 * }
 */

/* 已经写好，不是 TODO：整个 IDT 部分（含 page_fault_handler/idt_set_entry
 * /idt_init）跟 Lab5/Lab6 逐字节一样，本 Lab 不新增 IDT 向量——SYSCALL
 * 走 MSR 完全不经过 IDT，定时器仍然是向量 32。33 不是 32：IDT_ENTRIES
 * 必须能容纳"最大用到的下标+1"，见 Lab5 trap.c 对应位置的完整注释。 */
#define IDT_ENTRIES    33
#define IDT_TYPE_INT64 0x8E /* Present(1) | DPL(00) | 0 | type(1110) */
#define IDT_VEC_PAGE_FAULT 14
#define IDT_VEC_TIMER 32

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

static struct idt_entry idt[IDT_ENTRIES];

#define PF_ERR_PRESENT (1u << 0)
#define PF_ERR_WRITE   (1u << 1)
#define PF_ERR_USER    (1u << 2)

static uintptr_t read_cr2(void)
{
    uintptr_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

void page_fault_handler(uint64_t error_code)
{
    uintptr_t fault_addr = read_cr2();

    kprintf("page fault: addr=%p error_code=%lx (%s%s%s)\n",
            (void *)fault_addr, error_code,
            (error_code & PF_ERR_PRESENT) ? "protection-violation" : "not-present",
            (error_code & PF_ERR_WRITE) ? ",write" : ",read",
            (error_code & PF_ERR_USER) ? ",user" : ",kernel");

    panic("page_fault_handler: unrecoverable page fault (Lab4/5/6 do not implement fault recovery)");
}

extern void timer_interrupt_handler(void);

extern void page_fault_stub(void);
extern void timer_stub(void);

static void idt_set_entry(int vector, void (*handler)(void))
{
    uintptr_t addr = (uintptr_t)handler;

    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = GDT_SEL_KERNEL_CS;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = IDT_TYPE_INT64;
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    idt_set_entry(IDT_VEC_PAGE_FAULT, page_fault_stub);
    idt_set_entry(IDT_VEC_TIMER, timer_stub);

    static struct idt_pointer idtp;
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uintptr_t)idt;

    __asm__ volatile("lidt %0" : : "m"(idtp));
}

/* ---------------------------------------------------------------------
 * SYSCALL/SYSRET：MSR 编程 + 系统调用分发。
 * --------------------------------------------------------------------- */

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u

#define EFER_SCE (1ull << 0) /* SYSCALL Enable。 */

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

extern void syscall_entry(void);

/* 已经写好，不是 TODO：SYSCALL 不会自动切栈（这是它跟"经过 IDT+TSS
 * 的中断/异常"最大的区别——IDT 那条路径可以靠 TSS 的 RSP0 在跨特权级
 * 时让硬件自动换到已知的内核栈，SYSCALL 完全没有这个机制，进入内核
 * 的第一条指令时 %rsp 仍然是用户栈指针）。教学取向的最简单做法：一个
 * 固定的、内核私有的临时槽位，进内核第一件事就是把用户 RSP 存起来、
 * 换上内核栈；SYSRET 前反过来。这在单核（本课程从不引入 SMP，SMP 是
 * Lab10 的话题）场景下是安全的——没有第二个 CPU 会同时踩这个槽位，真实
 * 内核会用 per-CPU 的 swapgs+GS-relative 存储，那是"同一个问题在 SMP
 * 下的正确解"，在本课程的教学阶段属于不必要的额外复杂度。 */
uint64_t syscall_saved_user_rsp;

/* 已经写好，不是 TODO（这个变量本身要留着，trap_entry.S 里
 * `.extern syscall_kernel_rsp` 直接引用它；要你写的是下面 TODO 5 的
 * setter）：Lab7 新增的 syscall_entry 换栈目标栈顶，每个进程一份，跟 TSS.RSP0
 * 的角色完全对称，但服务两条不同的陷入路径——这是本 Lab 第一次实际
 * 跑通调度器之后才暴露的一个真实 bug 的修复,值得完整记录下来，不只是
 * 补一行代码：
 *
 * Lab6 遗留的旧设计（syscall_entry 硬编码 `lea __stack_top(%rip), %rsp`,
 * 换到 boot.S/linker.ld 里那个全局唯一的内核栈)在 Lab6 是完全正确的——
 * Lab6 没有 scheduler()/swtch(),唯一的执行流是"内核主循环+偶尔陷入的
 * 用户程序",一次 SYSCALL 处理期间不会有别的内核执行流也在用这个栈。
 *
 * Lab7 引入 scheduler() 之后这个前提不再成立：scheduler() 本身用
 * swtch() 切换到某个进程时,这次 swtch() 调用是在 __stack_top 这个栈
 * 上发起的（scheduler() 就是跑在这个栈上的一条内核执行流),swtch()
 * 会把 scheduler() 这一刻的现场（rbx/rbp/r12-r15+隐式返回地址)保存在
 * *这个栈上某个深度*,然后才跳过去执行这个进程——也就是说,只要这个
 * 进程还在跑（还没有通过 yield()/sys_exit_proc() 切回调度器),
 * __stack_top 这个栈的某一段内存事实上还"属于"被挂起的 scheduler()，
 * 不是空闲可以随便复用的。
 *
 * 如果这个进程这时触发一次 SYSCALL（比如 sys_exit_proc()：用户程序
 * 执行 SYS_EXIT,syscall_entry 被调用),旧设计会执行
 * `lea __stack_top(%rip), %rsp`——直接把 %rsp 重置到 __stack_top *这个
 * 固定符号地址本身*,跟"scheduler() 当前實际用到哪个深度"完全无关。
 * 如果这次 SYSCALL 处理过程中调的函数（syscall_dispatch→sys_exit_proc
 * →swtch())压栈的深度,恰好覆盖到 scheduler() 挂起时保存现场的那段
 * 内存,就会直接覆盖掉 scheduler() 保存的 rbx/rbp/r12-r15/返回地址——
 * 本 Lab 开发过程中在 QEMU 下实测触发过这个 bug：sys_exit_proc() 里
 * `swtch(&p->context, g_scheduler_context)` 试图"跳回调度器"时,读到
 * 的其实是被 sys_exit_proc() 自己这次 SYSCALL 调用链覆盖过的垂圾数据,
 * 表现为 ret 跳到 RIP=0,触发 #PF（error_code=(not-present,read,kernel),
 * CR2=0——指令取指失败,不是普通的数据访问越界)。
 *
 * 正确修复：syscall_entry 换栈换到"当前正在跑的这个进程自己的内核栈顶"
 * （跟 tss_set_rsp0() 已经在做的事完全对称——两条不同的陷入路径,同一个
 * "每个进程需要自己独立内核栈"的需求),不是一个所有执行流共用的固定
 * 地址。这样 scheduler() 挂起时保存现场用的 __stack_top,和某个进程
 * SYSCALL 处理期间用的自己的 kstack,是两块完全不重叠的内存,不会再
 * 互相覆盖。 */
uint64_t syscall_kernel_rsp;

/* TODO 5：实现 syscall_set_kernel_rsp。
 *
 * 为什么：跟 tss_set_rsp0() 是一对——同一个"每个进程要有自己的内核栈"
 * 需求，服务两条不同的陷入路径（TSS.RSP0 管 IDT 中断门，
 * syscall_kernel_rsp 管 SYSCALL）。scheduler() 每次切进程都要同时更新
 * 这两个，漏掉任何一个，那条路径就会退化成 Lab6 的"全局共享一个栈"，
 * 撞上上面注释里那个 RIP=0 / CR2=0 的取指失败。
 *
 * 提示：
 * void syscall_set_kernel_rsp(uintptr_t rsp)
 * {
 *     syscall_kernel_rsp = rsp;
 * }
 */

/* TODO 6：实现 sys_write。
 *
 * 为什么：跟 Lab6 完全一样，一个字没改，照抄即可。本 Lab 唯一"真正做事"
 * 的系统调用——把用户传进来的字符串打到串口，验证参数传递（rdi=第一个
 * 参数）+ 返回值（rax=写了多少字节）这条路径是通的。
 *
 * 注意 user_ptr 直接当内核地址解引用、没有做"这个地址真的属于调用者"的
 * 校验：真实内核必须校验（否则用户程序可以拿一个内核地址骗内核帮它读写
 * 任意内存，这是一整类真实的特权提升漏洞）。本 Lab 里每个进程虽然已经有
 * 自己的页表，但用户映射和内核映射仍然共存于同一份页表里（见
 * kernel_main.c），用户合法地址对内核也合法，校验不会改变任何行为。这条
 * 简化的边界在真正做地址空间隔离时就不再成立。
 *
 * 提示：
 * static int64_t sys_write(const char *user_ptr, uint64_t len)
 * {
 *     for (uint64_t i = 0; i < len; i++) {
 *         console_putc(user_ptr[i]);
 *     }
 *     return (int64_t)len;
 * }
 */

/* Lab7：proc.c 里真正的进程管理实现——sys_exit 不再只是打印退出码
 * （那是 Lab6 没有进程概念时的占位版本，已被移除），fork/exec/wait
 * 三个新系统调用也都落在那边（原型见 proc.h，本文件顶部已经
 * #include）。这几个函数需要"当前进程"这个 Lab6 完全没有的概念
 * （trapframe 指针、进程表槽位），不属于 trap.c 的职责范围（trap.c
 * 只管"陷入路径本身"：怎么从用户态安全进入内核态、怎么分发到具体
 * 处理函数——分发目标是谁的内部实现是另一个模块的事，这条边界从
 * Lab4 的 page_fault_handler 起就是这样划分的）。
 *
 * sys_fork/sys_exec 在 proc.h 里的返回类型是 int（不是 int64_t）——
 * syscall_dispatch 返回值是 int64_t，两者隐式转换没有精度损失（本
 * Lab 的 pid/错误码都在 int 范围内），不需要在这里强转。 */

/* TODO 7：实现 syscall_dispatch。
 *
 * 为什么：Lab6 已经写过它的骨架（switch on num，SYS_WRITE/SYS_EXIT 两个
 * 分支），Lab7 在两处改动：
 *   1. SYS_EXIT 不再是"打印退出码"的占位版本，改调 proc.c 的
 *      sys_exit_proc()——它会把当前进程标成 ZOMBIE 然后 swtch() 回调度器，
 *      永不返回；
 *   2. 新增 SYS_FORK/SYS_EXEC/SYS_WAIT 三个分支（实现都在 proc.c，原型见
 *      proc.h，本文件顶部已经 #include）。
 *
 * 函数签名也跟着变了：多出 user_rip_slot/user_rsp_slot 两个参数。为什么
 * 需要它们——SYSCALL 这条陷入路径*没有* proc.h 意义上的 struct trapframe
 * （那是 IDT 路径 timer_stub/page_fault_stub 落在 p->tf 上的现场格式，
 * syscall_entry 全程不碰 p->tf，见 trap_entry.S 顶部模块注释）。而
 * SYS_FORK 要让子进程从"fork 返回后的下一条用户指令"开始跑、SYS_EXEC 要
 * 把返回地址改成新程序入口，两者都必须读/写"这次系统调用返回后 rip/rsp
 * 应该是什么"。在 SYSCALL 路径上这两个值分别住在 syscall_entry 栈帧里
 * push 过的 rcx 槽位、以及 syscall_saved_user_rsp 这个全局变量里——
 * trap_entry.S 已经把它们的*地址*算好通过 rcx/r9 传进来，这里原样转发给
 * proc.c 即可（sys_write/sys_exit_proc/sys_wait 不需要，它们不改 rip/rsp）。
 *
 * num/a0/a1 对应 SYSCALL 传参约定里的 rax（调用号）/rdi/rsi（前两个参数）；
 * 返回值通过 rax 回用户态，这是沿用 Linux syscall ABI 的软件约定，不是
 * 硬件规定。本 Lab 的系统调用最多用到两个参数，没有实现更多参数的转发。
 *
 * 提示：
 * int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
 *                           uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot)
 * {
 *     switch (num) {
 *     case SYS_WRITE:
 *         return sys_write((const char *)a0, a1);
 *     case SYS_EXIT:
 *         sys_exit_proc((int64_t)a0);
 *         return 0; // 不可达——sys_exit_proc() 内部 swtch() 回调度器；
 *                   // 写 return 只是让这个分支在类型上完整，不依赖
 *                   // 编译器能推断出它不可达
 *     case SYS_FORK:
 *         return sys_fork(user_rip_slot, user_rsp_slot);
 *     case SYS_EXEC:
 *         return sys_exec(user_rip_slot, user_rsp_slot);
 *     case SYS_WAIT:
 *         return sys_wait((int64_t *)a0);
 *     default:
 *         kprintf("syscall_dispatch: unknown syscall number %lu\n", num);
 *         return -1;
 *     }
 * }
 */

/* TODO 8：实现 syscall_init。
 *
 * 为什么：跟 Lab6 完全一样，一个字没改，照抄即可。Lab7 没有引入任何新的
 * MSR 编程需求——syscall_kernel_rsp 那个换栈改动全都发生在 trap_entry.S
 * 和 scheduler() 里，跟"SYSCALL 指令本身怎么配置"这件事无关。
 *
 * 三个 MSR 各自的坑（尤其是 STAR 那个 +8/+16 反推）在 Lab6 已经完整讲过，
 * 这里把要点重列一遍，方便你不必翻回去：
 *
 * 提示：
 * void syscall_init(void)
 * {
 *     // EFER.SCE：SYSCALL/SYSRET 默认"存在但执行即 #UD"，必须先显式置位
 *     // SCE 才能真正使用。跟 boot.S 里为了进 long mode 置过的 EFER.LME
 *     // 是同一个 MSR 的两个不同 bit，所以这里要读-改-写，别覆盖 LME。
 *     uint64_t efer = rdmsr(MSR_EFER);
 *     wrmsr(MSR_EFER, efer | EFER_SCE);
 *
 *     // IA32_STAR 布局（仅 64 位模式 SYSCALL/SYSRET 用到的部分）：
 *     //   bits [47:32] = SYSCALL 进内核时用的 CS，SS 隐含算成这个值+8
 *     //   bits [63:48] = SYSRET 回用户态的"基准值"，CS=这个值+16、
 *     //                  SS=这个值+8（这就是 GDT 里 SS3 和 CS3 中间要空出
 *     //                  USER_CS32 占位项的原因）
 *     //   bits [31:0]  = 仅 32 位模式用到的 EIP，64 位模式忽略，写 0
 *     //
 *     // 64 位 SYSRET 的精确算式：
 *     //   CS.Selector = (STAR[63:48] + 16) OR 3
 *     //   SS.Selector = (STAR[63:48] +  8) OR 3
 *     // 同一个基准值，CS 用 +16、SS 用 +8，所以基准值必须落在"比 SS 还
 *     // 往前一格"的位置，正好是 USER_CS32（0x18）：0x18+8=0x20=USER_SS，
 *     // 0x18+16=0x28=USER_CS。这里如果直觉性地填 GDT_SEL_USER_SS 本身
 *     // （0x20）就错了——那样 SS 会算成 0x28（CS 的位置）、CS 算成 0x30。
 *     uint64_t star = ((uint64_t)GDT_SEL_USER_CS32 << 48) |
 *                      ((uint64_t)GDT_SEL_KERNEL_CS << 32);
 *     wrmsr(MSR_STAR, star);
 *
 *     // LSTAR：SYSCALL 执行时直接把这个 MSR 的值当 RIP 跳过去，不经过
 *     // IDT，也不经过 GDT 里的 CS 描述符找 base（64 位模式代码段 base
 *     // 恒为 0，RIP 就是线性地址本身）。
 *     wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
 *
 *     // FMASK：SYSCALL 进内核时硬件会把 RFLAGS 与上 ~FMASK 再存进 R11，
 *     // 即 FMASK 里置 1 的位进内核后都被清零。bit 9 就是 IF，这里只清它，
 *     // 让系统调用处理期间不可被定时器打断——不是因为会出错，而是更容易
 *     // 讲清楚。
 *     wrmsr(MSR_FMASK, (1ull << 9));
 * }
 */

