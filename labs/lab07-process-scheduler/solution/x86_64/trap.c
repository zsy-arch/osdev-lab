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

/* GDT 布局——顺序不是随便排的，SYSCALL/SYSRET 的 selector 算式把这个
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
 * segment，base=0/limit=0xFFFFF/G=1，区别只在 DPL 和 Type/S 位）——
 * 具体每个字段的位置见下面每个描述符常量后面的行内注释。 */
static uint64_t gdt[8] = {
    0x0000000000000000ull, /* 0: null */
    0x00AF9A000000FFFFull, /* 1: 内核代码段 CS0，selector=0x08，DPL=0，
                             * L=1（64 位），跟 Lab4/5 完全一样。 */
    0x00AF92000000FFFFull, /* 2: 内核数据段 DS0，selector=0x10，DPL=0。
                             * Type=0x2（可读写数据段），跟 CS0 的
                             * 区别只在 Type 字段的 bit3（代码/数据）
                             * 和 bit1（可写位，对数据段是 W 位）。 */
    0x00CFFA000000FFFFull, /* 3: 32 位兼容模式用户代码段占位，
                             * selector=0x18，DPL=3。L 位不置 1（这一项
                             * 假装是 32 位代码段，D/B 位置 1），因为
                             * 它本来就不会被真正执行，只用来占住
                             * SYSRET 算 selector 时的间隔位置。 */
    0x00CFF2000000FFFFull, /* 4: 用户数据段 SS3，selector=0x20，DPL=3。
                             * SYSRET 算出的 SS = STAR[63:48]+8，
                             * 即这一项 | 3（RPL=3）。 */
    0x00AFFA000000FFFFull, /* 5: 用户代码段 CS3，selector=0x28，DPL=3，
                             * L=1（64 位长模式用户代码）。SYSRET 算出
                             * 的 CS = STAR[63:48]+16，即这一项 | 3。 */
    0x0000000000000000ull, /* 6: TSS 描述符低 8 字节，tss_init() 运行
                             * 时填。 */
    0x0000000000000000ull, /* 7: TSS 描述符高 8 字节，tss_init() 运行
                             * 时填。 */
};

#define GDT_SEL_KERNEL_CS 0x08
#define GDT_SEL_KERNEL_DS 0x10
#define GDT_SEL_USER_CS32 0x18 /* 占位用，本 Lab 不会被真正装载。 */
#define GDT_SEL_USER_SS   0x20
#define GDT_SEL_USER_CS   0x28
#define GDT_SEL_TSS       0x30
/* RPL=3：selector 低两位是 Requested Privilege Level，装载到 CS/SS
 * 时必须显式带上 | 3，否则 CPU 会读成 RPL=0 的内核段选择子。 */
#define GDT_SEL_USER_SS_RPL3 (GDT_SEL_USER_SS | 3)
#define GDT_SEL_USER_CS_RPL3 (GDT_SEL_USER_CS | 3)

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

    /* lgdt 只换了 GDTR，不会自动重新加载 %ds/%es/%ss——这几个段寄存器
     * 从 boot.S 起就是 null selector（Lab4/5 全程没人管过），Lab5
     * 能正常跑是因为 64 位模式下数据段基本不做 base/limit 检查；
     * 但本 Lab 需要 %ss 变成一个真正指向上面 index 2（内核数据段）的
     * selector，因为 SYSCALL 进入内核后，尽管 CPU 会把 SS 设成
     * STAR[47:32]+8 对应的 selector，%ds/%es 仍然维持用户态那次
     * 遗留的值不变（SYSCALL 只换 CS/SS，不换 DS/ES/FS/GS）——如果
     * %ds/%es 一直是 null，将来任何一次访存指令用到 DS 隐式段（大部分
     * 普通访存指令都是）在 32 位保护模式思维下会出问题，虽然当前 64
     * 位 long mode 下这仍然基本不生效，但把它们显式设成内核数据段
     * selector 是"正确"的写法，不依赖"64 位模式碰巧不检查"这个事实
     * 继续裸奔下去。 */
    __asm__ volatile(
        "mov %0, %%ds\n"
        "mov %0, %%es\n"
        "mov %0, %%ss\n"
        :
        : "r"((uint16_t)GDT_SEL_KERNEL_DS));
}

/* Lab7 新增：64 位 TSS。字段布局是 Intel 手册规定的固定偏移，不是本课程
 * 自己设计的——reserved0 是为了让 rsp0 落在正确的偏移（TSS 结构体从
 * offset 4 开始才是 RSP0，前面 4 字节是保留的 reserved0），rsp1/rsp2、
 * ist1-7、iomap_base 本 Lab 完全不用（决定权在"64 位模式下这些字段除了
 * RSP0-2/IST1-7/IOPB 之外基本都被硬件忽略"这条 Intel 手册原文，本课程
 * 只需要 RSP0：每次 IDT 中断门在 CPL3 触发提权到 CPL0 时，硬件用 RSP0
 * 当作新栈顶，不会用到 RSP1/RSP2（那两个是给 IST 或者不同特权级跳转用
 * 的，本课程只有用户态→内核态这一种提权路径，永远只读 RSP0）），保留
 * 为 0 即可，因为硬件根本不会读它们。 */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss64 tss;

void tss_init(void)
{
    /* TSS 描述符是系统段描述符，跟前面 code/data 描述符的 8 字节格式
     * 不是同一种：64 位模式下它是 16 字节，位域布局是
     *   低 8 字节：limit[0:15] | base[0:23] | type(0x9=可用的32/64位
     *              TSS) | DPL | P | limit[16:19] | flags
     *   高 8 字节：base[32:63]（低 32 位），其余 32 位是保留位（必须
     *              是 0）。
     * 跟 code/data 描述符相比，type 字段的编码完全不同（0x9 不是"代码
     * /数据段"的任何 Type 值），S 位（描述符类型位，code/data 描述符
     * 里恒为 1）在系统段描述符里是 0——这是 CPU 区分"这是一个普通段
     * 描述符还是一个系统段描述符（TSS/LDT/门描述符）"的方式。
     *
     * limit 字段填 sizeof(tss)-1（TSS 的边界，硬件访问超出这个范围会
     * #GP），不是 0xFFFFF——TSS 不是 flat segment，它的"limit"就是它
     * 自己结构体的大小，跟 code/data 描述符里"整个 4GB 地址空间"的
     * 语义完全不同。 */
    uint64_t base = (uintptr_t)&tss;
    uint32_t limit = sizeof(tss) - 1;

    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFFull);
    low |= (base & 0xFFFFFFull) << 16;
    low |= (uint64_t)0x9ull << 40; /* Type=0x9：可用的 64 位 TSS。 */
    low |= (uint64_t)0x0ull << 45; /* DPL=0：只有内核会 ltr/任务切换。*/
    low |= (uint64_t)0x1ull << 47; /* P=1：段存在。 */
    low |= ((uint64_t)(limit >> 16) & 0xFull) << 48;
    /* base[31:24] 单独占低 8 字节的 bit[63:56]——上面 `(base & 0xFFFFFF)
     * << 16` 那一行只覆盖了 base[23:0]（塞进 bit[39:16]），base 剩下的
     * [31:24] 这 8 位在 16 字节 TSS 描述符格式里落在低 8 字节的最高
     * 字节，跟 code/data 描述符"base 分两段、[31:24] 在 bit[63:56]"的
     * 布局是同一个约定（这里之前漏写了这一段，是本 Lab 实测抓到的真实
     * bug：漏掉这 8 位相当于把 base 的 bit31 一并清零——tss 这个 static
     * 全局变量的链接地址是 0xffffffff80109040，bit31=1，被清成 0 之后
     * hardware 从 TR 描述符读出来的 TSS base 变成 0xffffffff00109040，
     * 从 KERNEL_PML4_INDEX=511 覆盖的规范高地址范围里掉出去，落进一个
     * 完全没有映射的 PML4 槽位。这段代码本身在开发期间从未报错，因为
     * ltr 只检查描述符格式合法性，不检查 base 对不对，直到 Lab7 第一次
     * 真正出现"CPL3 执行期间被定时器中断打断、需要硬件通过 IDT 中断门
     * 提权到 CPL0"这个场景（也是本文件顶部模块注释里明确点名的、当年
     * 促成引入 TSS 的那个场景）——硬件提权那一刻要从 TR 指向的 TSS
     * 结构体里读 RSP0（偏移+4），用的正是这个被截断过的错误 base,
     * 算出来的地址是 0xffffffff00109040+4=0xffffffff00109044,访问这个
     * 地址触发 #PF,而这次 #PF 本身又是在"提权到 CPL0"这个动作的中途
     * 发生的、硬件此时还没有一个可用的内核栈去压异常帧,于是再次
     * 触发同样的 #PF（error_code 里 old:0xffffffff new:0xe 之后紧跟着
     * old:0xe new:0xe）,双重故障升级成尝试投递 #DF 时的 #GP
     * （old:0x8 new:0xd),最终三重故障、QEMU 复位——这条链路里
     * CR2=0xffffffff00109044 那个"只差 bit31"的诡异地址,根源就是这里
     * 漏写的 8 位,不是页表/kalloc/调度器那边的问题。 */
    low |= ((base >> 24) & 0xFFull) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFFull;

    gdt[6] = low;
    gdt[7] = high;

    tss.rsp0 = 0; /* proc_alloc()/scheduler() 会在真正切换前填上。 */
    tss.iomap_base = sizeof(tss); /* 指向结构体末尾之外，等价于没有
                                    * IOPB（本 Lab 不需要 I/O 位图）。*/

    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_SEL_TSS));
}

/* 调度器在 swtch() 切换到某个进程之前调用：把 TSS.RSP0 指向"这个进程
 * 自己的内核栈顶"。之后这个进程在用户态被任何 IDT 中断门打断、需要
 * CPL3→CPL0 提权时，硬件会自动用这个 RSP0 当作新栈顶——这就是"每个
 * 进程有自己独立内核栈"在硬件层面真正生效的地方，不设置这个,所有
 * 进程共享同一个（或者像 Lab6 一样完全无效的）RSP0，多进程一旦被
 * 定时器打断就会栈踩踏或者撞上本文件开头注释里那个三重故障。 */
void tss_set_rsp0(uintptr_t rsp0)
{
    tss.rsp0 = rsp0;
}

/* 33，不是 32：见 Lab5 trap.c 对应位置的完整注释（IDT_ENTRIES 必须
 * 能容纳"最大用到的下标+1"，本 Lab 沿用 Lab5 的向量分配，不新增 IDT
 * 向量——SYSCALL 走的是 MSR，完全不经过 IDT，这里维持 Lab5 原样。 */
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

/* SYSCALL 不会自动切栈（这是它跟"经过 IDT+TSS 的中断/异常"最大的
 * 区别——IDT 那条路径可以靠 TSS 的 RSP0 在跨特权级时让硬件自动换到
 * 已知的内核栈，SYSCALL 完全没有这个机制，进入内核的第一条指令时
 * %rsp 仍然是用户栈指针）。教学取向的最简单做法：一个固定的、内核
 * 私有的临时槽位，进内核第一件事就是把用户 RSP 存起来、换上内核栈；
 * SYSRET 前反过来。这在单核（本课程从不引入 SMP，SMP 是 Lab10 的
 * 话题）场景下是安全的——没有第二个 CPU 会同时踩这个槅位，真实内核
 * 会用 per-CPU 的 swapgs+GS-relative 存储，那是"同一个问题在 SMP 下
 * 的正确解"，在本课程的教学阶段属于不必要的额外复杂度。 */
uint64_t syscall_saved_user_rsp;

/* Lab7 新增：syscall_entry 换栈用的目标栈顶，每个进程一份，跟 TSS.RSP0
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

void syscall_set_kernel_rsp(uintptr_t rsp)
{
    syscall_kernel_rsp = rsp;
}

/* sys_write：本 Lab 唯一真正做事的系统调用——把用户传进来的字符串
 * 打到串口。ROADMAP 明确要求的"最小系统调用"就是这一个：验证参数
 * 传递（rdi=第一个参数，指向用户地址空间里的字符串）+ 返回值
 * （rax=写了多少字节）这一整条路径是通的，不需要真的实现完整的
 * write(2) 语义（fd 参数、错误处理等）。
 *
 * user_ptr 这里直接当内核地址解引用，没有做"这个地址真的属于调用者
 * 的地址空间、真的可读"这类校验——真实内核在这里必须校验（否则用户
 * 程序可以拿一个内核地址骗内核帮它读/写任意内存，这是一整类真实的
 * 特权提升漏洞），但本 Lab 的用户页表和内核页表是*同一份*（见
 * kernel_main.c 关于"为什么不做地址空间隔离"的说明），用户合法地址
 * 在当前页表下对内核也是合法地址，校验在本 Lab 的架构下不会改变任何
 * 行为，教学取向不引入。这一简化的边界会在后续 Lab（真正的多地址
 * 空间/fork 出现之后）变得不再成立，届时必须补上。 */
static int64_t sys_write(const char *user_ptr, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        console_putc(user_ptr[i]);
    }
    return (int64_t)len;
}

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

/* syscall_dispatch：syscall_entry（trap_entry.S）保存完 callee-saved
 * 之外需要保护的寄存器之后，用 C 调用约定调这个函数——num/a0/a1 对应
 * SYSCALL 传参约定里的 rax（调用号）/rdi/rsi（前两个参数），本 Lab
 * 系统调用加起来最多用到两个参数，没有实现更多参数的转发，需要更多
 * 参数的系统调用留给以后的 Lab。
 *
 * 返回值通过 rax 传回用户态——这是 SYSCALL/SYSRET 约定的一部分（跟
 * IDT 中断门不一样，中断处理函数没有"返回值"这个概念，SYSCALL 的
 * 返回值传递完全是软件约定，不是硬件规定，但沿用 Linux syscall ABI
 * 的"rax 传返回值"是业界统一做法，本课程直接采用，不发明新约定）。
 *
 * user_rip_slot/user_rsp_slot 这两个额外参数是 Lab7 新增的——SYSCALL
 * 这条陷入路径*没有* proc.h 意义上的 struct trapframe（那是 IDT 路径
 * ——timer_stub/page_fault_stub——落在 p->tf 上的现场格式，syscall_
 * entry 全程不touches p->tf，见 trap_entry.S 顶部模块注释和本文件顶部
 * 模块注释"SYSCALL 是独立于 IDT 的第三条陷入路径"）。SYS_FORK/SYS_EXEC
 * 需要读/写"这次系统调用返回后 rip/rsp 应该是什么"，而在 SYSCALL 路径
 * 上，这两个值就是 syscall_entry 栈帧里那个 push 过的 rcx 槽位，以及
 * syscall_saved_user_rsp 这个全局变量本身——trap_entry.S 已经把这两者
 * 的*地址*算好，通过 rcx/r9 传进来，这里原样转发给 proc.c（sys_write/
 * sys_exit_proc/sys_wait 不需要它们，因为它们不修改 rip/rsp）。 */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                          uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot)
{
    switch (num) {
    case SYS_WRITE:
        return sys_write((const char *)a0, a1);
    case SYS_EXIT:
        sys_exit_proc((int64_t)a0);
        return 0; /* 不可达——sys_exit_proc() 内部 swtch() 回调度器，
                   * 见 proc.c 自己的注释；写 return 只是让这个 switch
                   * 分支在类型上完整，不依赖编译器能推断出它不可达。 */
    case SYS_FORK:
        return sys_fork(user_rip_slot, user_rsp_slot);
    case SYS_EXEC:
        return sys_exec(user_rip_slot, user_rsp_slot);
    case SYS_WAIT:
        return sys_wait((int64_t *)a0);
    default:
        kprintf("syscall_dispatch: unknown syscall number %lu\n", num);
        return -1;
    }
}

void syscall_init(void)
{
    /* EFER.SCE：SYSCALL/SYSRET 指令默认是"存在但执行即 #UD"，必须先
     * 显式置位 EFER 里的 SCE 位才能真正使用——这跟 boot.S 里为了进
     * long mode 已经置位过的 EFER.LME 是同一个 MSR 的两个不同 bit，
     * 互不影响，boot.S 那次写 EFER 时 SCE 还是 0，这里需要单独再置
     * 一次（读-改-写，不覆盖 LME 已经置好的状态）。 */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    /* IA32_STAR 布局（仅 64 位模式下 SYSCALL/SYSRET 用到的部分）：
     *   bits [47:32] = SYSCALL 进内核时用的 CS，SS 隐含算成这个值+8。
     *   bits [63:48] = SYSRET 回用户态时的"基准值"，CS 算成这个值+16，
     *                  SS 算成这个值+8（这就是为什么 GDT 里 SS3 和
     *                  CS3 中间要空出 USER_CS32 那个占位项——16 和 8
     *                  之间正好差 8，对应一个描述符的间隔）。
     *   bits [31:0]  = 仅 32 位模式 SYSCALL 用到的 EIP，64 位模式
     *                  下这部分被忽略（真正的入口地址来自 LSTAR），
     *                  这里写 0。
     *
     * 64 位模式 SYSRET 的精确算式（逐位核对过 AMD64 手册/asm-dude wiki
     * 转录的 SYSRET 操作性伪代码，不是转述）：
     *   CS.Selector = (STAR[63:48] + 16) OR 3
     *   SS.Selector = (STAR[63:48] +  8) OR 3
     * 同一个基准值 STAR[63:48]，CS 用 +16、SS 用 +8——也就是说这个
     * 基准值必须落在"比 SS 还要往前一格"的位置，正好是 USER_CS32
     * （0x18）那个占位项：0x18+8=0x20=USER_SS，0x18+16=0x28=USER_CS，
     * 跟 GDT 表里的实际布局对上。这里如果直觉性地填 GDT_SEL_USER_SS
     * 本身（0x20）会是错的——那样算出来 SS=0x28（变成了 CS 的位置）、
     * CS=0x30（GDT 里根本没有第 7 项），对应到 SYSCALL 侧的算法则不同：
     *   CS.Selector = STAR[47:32] AND FFFCH（强制 RPL=0）
     *   SS.Selector = STAR[47:32] + 8
     * SYSCALL 一侧没有"往前退一格"的问题，因为它的 SS 只是简单地在
     * CS 基础上 +8，不需要像 SYSRET 那样同时用一个基准值往两个不同
     * 方向的偏移量倒推——这也是为什么 GDT 里内核 CS0/DS0 是紧挨着的
     * 两项，但用户侧 SS3/CS3 中间要空出 USER_CS32 那一项间隔。 */
    uint64_t star = ((uint64_t)GDT_SEL_USER_CS32 << 48) |
                     ((uint64_t)GDT_SEL_KERNEL_CS << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR：SYSCALL 执行时直接把这个 MSR 的值当 RIP 跳过去（不经过
     * IDT，不经过 GDT 里的 CS 描述符找 base——64 位模式下代码段 base
     * 恒为 0，RIP 就是线性地址本身），syscall_entry 是 trap_entry.S
     * 里新增的汇编入口。 */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* FMASK：SYSCALL 进内核时，RFLAGS 会先与上 ~FMASK 再存进 R11
     * （硬件自动做的，不是软件行为）——也就是 FMASK 里置 1 的每一位，
     * 进内核后都会被清零。这里只清 IF（中断标志）：SYSCALL 处理期间
     * 不希望被定时器中断打断（syscall_saved_user_rsp 那个全局槅位
     * 正在被使用，如果这时候定时器中断插进来、它的 iretq 恢复流程
     * 跟 SYSCALL 自己的栅位管理是两条独立的路径，不会互相破坏，但
     * 让 SYSCALL 处理过程保持不可中断更简单、更容易讲清楚，教学
     * 取向不引入"系统调用处理期间还能被打断"这一层复杂度）。 */
    wrmsr(MSR_FMASK, (1ull << 9) /* IF */);
}
