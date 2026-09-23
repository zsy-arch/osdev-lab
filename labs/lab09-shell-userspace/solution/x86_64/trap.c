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
#include "fs.h"   /* Lab8: fs_lookup/fs_read——sys_open/sys_read 的实际工作在那边 */
#include "pipe.h" /* Lab9: pipe_alloc/read/write/close/dup——fd 表的第三种类型 */

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
/* bit 3 RSVD：页表项里置了保留位。【Lab8 新增】
 *
 * 加这一位是因为 Lab8 被它坑过一次：错误码 0xa（RSVD | WRITE，PRESENT=0）
 * 被下面的解码器打成 "not-present,write"，于是排查方向完全跑偏——去找
 * "为什么这一页没映射"，而那一页其实映射得好好的，问题是页表项里置了
 * bit 63（NX）却没打开 EFER.NXE（boot.S 的 EFER 那段有完整记录）。
 *
 * RSVD 和 not-present 是两种完全不同的故障："没有这一项"vs"这一项本身
 * 非法"。少打印一位，就会把人引向错误的假设——诊断信息漏掉一个维度的
 * 代价，往往比缺少这个功能本身大得多。 */
#define PF_ERR_RSVD    (1u << 3)
/* bit 4 I/D：取指令导致的页错误。NXE 打开之后这一位才有意义（不可执行
 * 页上取指令会是 present + instruction-fetch），顺手一起打印。 */
#define PF_ERR_INSTR   (1u << 4)

static uintptr_t read_cr2(void)
{
    uintptr_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

void page_fault_handler(uint64_t error_code)
{
    uintptr_t fault_addr = read_cr2();

    /* RSVD 单独先判、并且盖过 present/not-present 的措辞：置了保留位时
     * PRESENT 位是不可信的（硬件在这种情况下不保证它的含义），照常打印
     * "not-present"就是上面注释里说的那个误导。 */
    kprintf("page fault: addr=%p error_code=%lx (%s%s%s%s)\n",
            (void *)fault_addr, error_code,
            (error_code & PF_ERR_RSVD)
                ? "reserved-bit-set-in-pte"
                : ((error_code & PF_ERR_PRESENT) ? "protection-violation" : "not-present"),
            (error_code & PF_ERR_WRITE) ? ",write" : ",read",
            (error_code & PF_ERR_USER) ? ",user" : ",kernel",
            (error_code & PF_ERR_INSTR) ? ",instruction-fetch" : "");

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

/* ---------------------------------------------------------------------------
 * 文件描述符层：write / read / open / close / pipe / dup
 * ---------------------------------------------------------------------------
 * Lab8 建立了"fd 是一个小整数，内核替你记住它指向哪个 inode、读到哪里"
 * 这个抽象。Lab9 把它扩展到三种对象（控制台/磁盘文件/管道），于是这一层
 * 从"查表拿 inum"变成了真正的*分派*：查表拿到类型，再决定走哪条路。
 *
 * 这个改动的分量比代码量看起来大。Lab8 的 sys_write 压根不查 fd 表——它
 * 看见什么 fd 都往串口打（其实连 fd 参数都没有）。那在只有控制台和只读
 * 文件的世界里够用。Lab9 的 `cat /motd.txt | grep lab` 要求 cat 的 fd 1
 * 是管道写端、grep 的 fd 0 是管道读端，而两个程序的代码里写的就是普通的
 * write(1,...) / read(0,...)。"fd 1 现在指向什么"必须是运行时可以被 shell
 * 改掉的状态，也就必须真的存在表里、必须真的每次查。
 *
 * 换句话说，本 Lab 真正实现的是 Unix 那句"一切皆文件"——它的含义不是
 * "所有东西都是磁盘文件"，而是"所有东西都通过同一张表、同一组 read/write
 * 来访问，差别藏在表项的类型标签后面"。下面这六个函数就是那句话的代码。
 *
 * 为什么这一层放在 trap.c 而不是 fs.c：fs.c 是"磁盘上的数据结构"那一层,
 * 它不知道进程的存在，两个架构共用同一份（见 fs.c 顶部注释）。而 fd 表
 * 长在 struct proc 里，属于"进程"这一层。把两者分开，fs.c 才能保持架构
 * 无关*且*进程无关——这条边界跟 Lab4 起 page_fault_handler 留在 trap.c、
 * 页表操作留在 pagetable.c 是同一个划分方式。
 *
 * ── 用户指针：本 Lab 一律直接解引用，不校验 ──────────────────────
 *
 * 下面每个函数拿到的用户指针（buf/name/fds）都是直接当地址用的，没有
 * 检查"它真的落在调用者自己的用户地址空间里、真的可读/可写"。
 *
 * 真实内核必须校验，否则用户程序可以传一个内核地址，骗内核帮它读或写
 * 任意内存——这是一整类真实的特权提升漏洞。read 方向尤其危险，因为它是
 * *往*用户给的地址*写*：传一个内核数据结构的地址进来，内核就替你把文件
 * 内容覆盖到内核内存里，这是一个直接可用的提权原语。
 *
 * 本 Lab 不做校验是教学简化，README 的简化清单和挑战任务里都有这一条
 * （copy_from_user / access_ok）。需要注意的是这个简化在 Lab9 比在 Lab8
 * 更"假"：Lab6/Lab7 早期用户和内核共用一份页表，用户的合法地址对内核也
 * 合法，不校验在当时真的不改变任何行为；到了 Lab9，每个进程有自己的页表,
 * 一个恶意的用户指针完全可以指向只有内核映射的地址，此时不校验就是实打
 * 实的漏洞，只是本 Lab 的用户程序都不这么干而已。
 */

/* fd -> struct file* 的解析。所有六个系统调用共用，把"fd 合法性检查"收在
 * 一处，而不是在每个调用里各写一遍 if。
 *
 * Lab9 的变化：判据从 Lab8 的 `used` 字段变成 `type != FD_NONE`。这不只是
 * 换个名字——FD_NONE 被定为 0，于是"清零的 PCB 等于所有 fd 都关闭"这个
 * 性质是免费的，而且一个字段同时承担了"在不在用"和"是什么"两件事，不会
 * 出现 used=1 但 type 没设这种自相矛盾的中间状态。 */
static struct file *fd_lookup(struct proc *p, int fd)
{
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    if (p->ofile[fd].type == FD_NONE) {
        return NULL;
    }
    return &p->ofile[fd];
}

/* 找一个空闲 fd，返回*最小*的那个；没有空位返回 -1。
 *
 * "最小"不是审美偏好，是 POSIX 明确规定的行为（open/dup 都必须返回当前
 * 最小的可用 fd），而且 shell 的重定向直接依赖它。user/sh.c 里那句
 *
 *     close(0); if (dup(fd) != 0) { ...报错... }
 *
 * 的全部正确性就建立在这上面：先把 0 关掉，此时 0 成为最小的空位，于是
 * dup 一定返回 0，新打开的文件就顶替到了 fd 0 的位置上。如果这里改成
 * "从头扫但返回第一个碰到的"以外的任何策略（比如轮转、或者从高往低），
 * dup 会返回别的数字，sh 那个 if 会报错——这是刻意留下的、会大声失败的
 * 检查，而不是静默的错误重定向。 */
static int fd_alloc(struct proc *p)
{
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd].type == FD_NONE) {
            return fd;
        }
    }
    return -1;
}

/* 把一个 fd 槽位清空。
 *
 * 全部字段归零、而不只是把 type 设成 FD_NONE：残留的 inum/pipe 指针会让
 * "用了一个已关闭的 fd"这类 bug 看起来像在正常工作（表项里还有个像样的
 * inode 号），而清零之后同样的 bug 会立刻撞上 fd_lookup 的 FD_NONE 检查。
 * 同样是"让错误尽早变成可观察的失败"那条纪律。
 *
 * 注意这个函数*不*管管道的引用计数——调用者负责在清空之前先调 pipe_close。
 * 分成两步而不是在这里一并处理，是因为 sys_exit_proc 那边也要走同一条
 * 清理逻辑，而它的调用时机和 sys_close 不同。 */
static void fd_clear(struct file *f)
{
    f->type = FD_NONE;
    f->inum = 0;
    f->off = 0;
    f->pipe = NULL;
    f->writable = 0;
}

/* 从控制台读。至少读到一个字节才返回，读到换行就停。
 *
 * ── 为什么这里必须阻塞，而 console_getc() 必须不阻塞 ──────────────
 *
 * console_getc() 是硬件层：查一下 LSR，有就拿走，没有就说没有。它不能等,
 * 因为在那一层"等"只有忙转一种写法，而单核 + 系统调用期间关中断意味着
 * 忙转会卡死整个系统——定时器打不进来，调度器永远没机会跑。
 *
 * 这里是策略层，可以等，因为这里能 yield()：把 CPU 让给别的进程，下次
 * 轮到自己再看一眼。代价是忙等（这个进程会被反复调度、反复检查），但
 * 系统整体还在动。这就是"底层提供机制、上层决定策略"这句话的一个具体
 * 例子——同一个 console_getc()，中断驱动的实现会把这里换成"挂进等待
 * 队列"，而下面那一层一行都不用改。
 *
 * ── 为什么读到换行就停，而不是凑满 len ───────────────────────────
 *
 * 行缓冲是终端的标准行为（POSIX 的 canonical mode）。sh.c 其实是一个字节
 * 一个字节读的（len 恒为 1），所以这个判断对它没有影响；但对任何用大
 * 缓冲区读一行的程序来说，没有这个判断就得等到缓冲区满才返回，用户按了
 * 回车却没反应。
 *
 * ── 永不返回 0 ──────────────────────────────────────────────────
 *
 * 控制台没有 EOF 这个概念：串口那头的人可能只是还没开始打字。所以这个
 * 函数在没有输入时永远等下去，绝不返回 0。这件事有一个直接的后果，值得
 * 记住：交互式 sh 在 read(0) 上会永远挂住，于是自动测试跑到最后不是
 * "程序结束"，而是 QEMU 超时退出（exit code 124），test-lab.sh 把 124
 * 当成合法退出码正是为了这个。见 user/init.c 末尾的注释。 */
static int64_t console_read(char *user_buf, uint64_t len)
{
    if (len == 0) {
        return 0;
    }

    uint64_t got = 0;
    while (got < len) {
        int c = console_getc();
        if (c < 0) {
            /* 没有输入。已经读到东西就先交付（不让用户等下一个字节），
             * 一个字节都没读到就让出 CPU 继续等。 */
            if (got > 0) {
                break;
            }
            yield();
            continue;
        }

        /* 回车当换行。串口那头按回车发的是 '\r'（CR），而程序里判断行尾
         * 用的是 '\n'——终端驱动本来负责这个转换，本 Lab 没有终端驱动,
         * 所以在这里做。不转换的症状是"按回车没反应"，因为 sh 的 readline
         * 在等一个永远不会来的 '\n'。
         *
         * sh.c 的 readline 其实两个都收（见那边注释），所以这个转换对它
         * 不是必需的；留着是因为它让"控制台交给上层的字节流"这件事有一个
         * 统一的形状：行尾一律是 '\n'，上层不必知道串口的习惯。 */
        if (c == '\r') {
            c = '\n';
        }

        /* 回显。串口那头的终端不会自己显示你打的字——它把字节发给我们就
         * 完事了，屏幕上出现什么由我们决定。不回显的症状是"打字看不见"。
         *
         * 这也是为什么回显是内核的职责而不是 shell 的：所有从控制台读的
         * 程序都需要它，而且需要在字节被读走的那一刻就发生，不能等到某个
         * 程序决定把它打出来。 */
        console_putc((char)c);

        user_buf[got++] = (char)c;

        if (c == '\n') {
            break;
        }
    }

    return (int64_t)got;
}

/* sys_write：往 fd 写 len 字节。
 *
 * Lab8 的版本没有 fd 参数，看见什么都往串口打。Lab9 加上 fd 并真正分派——
 * 这一个改动就是管道能工作的全部内核侧前提。
 *
 * FD_INODE 返回 -1：本 Lab 的文件系统是只读的（fs.c 里没有任何写路径），
 * 所以"往文件写"是一个诚实的失败，不是没实现完。shell 的 `>` 重定向因此
 * 不存在，README 的已知限制里有记录。 */
static int64_t sys_write(int fd, const char *user_buf, uint64_t len)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_write: 没有当前进程——系统调用只能来自用户进程");
    }

    struct file *f = fd_lookup(p, fd);
    if (f == NULL) {
        return -1;
    }

    switch (f->type) {
    case FD_CONSOLE:
        for (uint64_t i = 0; i < len; i++) {
            console_putc(user_buf[i]);
        }
        return (int64_t)len;

    case FD_PIPE:
        if (!f->writable) {
            /* 往读端写。用户程序拿错了 fd，返回 -1。 */
            return -1;
        }
        return (int64_t)pipe_write(f->pipe, user_buf, (uint32_t)len);

    case FD_INODE:
        return -1;

    default:
        panic("sys_write: fd 表项的类型标签是未知值——PCB 被写坏了");
    }
}

int64_t sys_open(const char *user_name)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_open: 没有当前进程——系统调用只能来自用户进程");
    }

    uint32_t inum = fs_lookup(user_name);
    if (inum == 0) {
        /* 文件不存在。返回 -1 而不是 panic：这是*用户程序*可能犯的错，
         * 不是内核的错误。区分这两类失败是内核设计里一条重要的纪律——
         * 用户程序的任何行为（包括传一个不存在的文件名）都不应该能把
         * 内核搞停；只有内核自己的不变式被破坏时才 panic。 */
        return -1;
    }

    int fd = fd_alloc(p);
    if (fd < 0) {
        /* fd 表满了。真实 Unix 在这里返回 EMFILE。 */
        return -1;
    }

    p->ofile[fd].type = FD_INODE;
    p->ofile[fd].inum = inum;
    p->ofile[fd].off = 0;
    p->ofile[fd].pipe = NULL;
    p->ofile[fd].writable = 0;
    return fd;
}

int64_t sys_read(int fd, void *user_buf, uint64_t len)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_read: 没有当前进程——系统调用只能来自用户进程");
    }

    struct file *f = fd_lookup(p, fd);
    if (f == NULL) {
        return -1;
    }

    switch (f->type) {
    case FD_CONSOLE:
        return console_read((char *)user_buf, len);

    case FD_INODE: {
        /* 一次最多读多少：本 Lab 不限制，直接把 len 交给 fs_read()。
         * fs_read() 内部按块循环，用的是它自己栈上的 512 字节缓冲区，
         * 不会因为 len 很大而爆栈——数据是直接拷进用户缓冲区的，内核
         * 侧的临时空间始终只有一块。 */
        uint32_t got = fs_read(f->inum, f->off, user_buf, (uint32_t)len);

        /* 推进偏移。这一行就是"文件描述符携带状态"的全部实现：用户程序
         * 连续调用 read() 能读到文件的后续内容，靠的就是内核在这里替它
         * 记住了位置。 */
        f->off += got;

        /* got == 0 表示 EOF，跟 POSIX read(2) 一致——返回 0 不是错误，
         * 用户程序据此结束循环。 */
        return (int64_t)got;
    }

    case FD_PIPE:
        if (f->writable) {
            /* 从写端读。用户程序拿错了 fd。 */
            return -1;
        }
        return (int64_t)pipe_read(f->pipe, (char *)user_buf, (uint32_t)len);

    default:
        panic("sys_read: fd 表项的类型标签是未知值——PCB 被写坏了");
    }
}

int64_t sys_close(int fd)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_close: 没有当前进程——系统调用只能来自用户进程");
    }

    struct file *f = fd_lookup(p, fd);
    if (f == NULL) {
        /* 关一个没打开的 fd：返回 -1。 */
        return -1;
    }

    /* 管道要先减引用计数，再清槽位。顺序反了就拿不到 pipe 指针了。
     *
     * 这一行是 Lab9 相对 Lab8 在 close 上唯一的变化，但它承载了整条管道
     * 生命周期链：sh 的六次 close 里每一次都走到这里，管道的两个计数被
     * 一次次减下去，最后归零的那一次触发回收。更重要的是中间那些次——
     * `cat /motd.txt | grep lab` 里 cat 退出时写端计数从 2 减到 1 还不算
     * EOF（sh 手上还有一份），sh 也关掉之后才归零，grep 这才读到 EOF。
     * 少任何一次 close，grep 就永远挂住。 */
    if (f->type == FD_PIPE) {
        pipe_close(f->pipe, f->writable);
    }

    fd_clear(f);
    return 0;
}

/* sys_pipe：建一个管道，把读端和写端的 fd 写进 user_fds[0]/[1]。
 *
 * 一次系统调用要返回两个值，而返回值只有一个寄存器——POSIX 的解法是让
 * 调用者传一个数组进来，内核往里写。这是"输出参数"这个模式在系统调用
 * 界面上的典型用法（另一个例子是 wait 的 status 指针，本 Lab 的 wait
 * 简化掉了）。
 *
 * 两个 fd 的分配顺序有讲究：先读端后写端，于是 fds[0] < fds[1]。POSIX
 * 没有要求这一点，但所有实现都这么做，用户代码里也常有人依赖（比如假设
 * fds[0] 是读端）。保持一致没有坏处。 */
int64_t sys_pipe(int *user_fds)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_pipe: 没有当前进程——系统调用只能来自用户进程");
    }

    struct pipe *pi = pipe_alloc();
    if (pi == NULL) {
        return -1;
    }

    int rfd = fd_alloc(p);
    if (rfd < 0) {
        /* fd 表满了。管道已经分配出来了，得还回去——两端各关一次，
         * 计数归零，槽位释放。
         *
         * 这是本 Lab 里少见的"部分成功之后回滚"的路径。写对它的关键是
         * 认清 pipe_alloc() 把两个计数都初始化成了 1（假定调用者马上会
         * 把两端放进 fd 表），所以回滚就是把那两个假定撤销掉。 */
        pipe_close(pi, 0);
        pipe_close(pi, 1);
        return -1;
    }

    /* 先占住读端的槽位，再找写端的——否则 fd_alloc 会两次返回同一个 fd
     * （它只看 type == FD_NONE，而读端还没填进去）。 */
    p->ofile[rfd].type = FD_PIPE;
    p->ofile[rfd].inum = 0;
    p->ofile[rfd].off = 0;
    p->ofile[rfd].pipe = pi;
    p->ofile[rfd].writable = 0;

    int wfd = fd_alloc(p);
    if (wfd < 0) {
        /* 读端已经占了一个槽位，连它一起回滚。fd_clear 之前先减计数,
         * 跟 sys_close 里同样的顺序理由。 */
        pipe_close(pi, 0);
        fd_clear(&p->ofile[rfd]);
        pipe_close(pi, 1);
        return -1;
    }

    p->ofile[wfd].type = FD_PIPE;
    p->ofile[wfd].inum = 0;
    p->ofile[wfd].off = 0;
    p->ofile[wfd].pipe = pi;
    p->ofile[wfd].writable = 1;

    user_fds[0] = rfd;
    user_fds[1] = wfd;
    return 0;
}

/* sys_dup：复制一个 fd，返回新的那个（最小可用的）。
 *
 * 新旧两个 fd 指向同一个东西。对管道来说这是真的共享（pipe 指针相同,
 * 引用计数加一）；对磁盘文件来说本 Lab 只是把 inum/off 拷了一份，于是
 * 两个 fd 的偏移各自独立——这不是 POSIX 语义（真正的 dup 要求两个 fd
 * 共享偏移），是 struct file 直接嵌在 PCB 里这个简化的连带后果，见
 * proc.h 里 struct file 的注释。
 *
 * sh 只在管道和重定向里用 dup，两处都不依赖偏移共享，所以这个偏差在本
 * Lab 观察不到。它是 README 挑战任务"全局 file 表 + 引用计数"要修的东西。
 *
 * ── dup 为什么必须返回最小可用 fd ────────────────────────────────
 *
 * 因为它是重定向的唯一手段。sh 想让某个文件变成子进程的 fd 0，做法是
 * close(0) 然后 dup(fd)——没有"指定目标 fd"的接口（那是 dup2，本 Lab
 * 没有，留作挑战任务）。所以"dup 一定返回刚刚腾出来的那个最小的 fd"
 * 就是重定向能成立的全部机制。sh.c 里那几个 `if (dup(...) != 0)` 的
 * 检查就是在守卫这条性质。 */
int64_t sys_dup(int fd)
{
    struct proc *p = proc_current();
    if (p == NULL) {
        panic("sys_dup: 没有当前进程——系统调用只能来自用户进程");
    }

    struct file *f = fd_lookup(p, fd);
    if (f == NULL) {
        return -1;
    }

    int nfd = fd_alloc(p);
    if (nfd < 0) {
        return -1;
    }

    /* 整个表项拷过去，然后按类型补引用计数。
     *
     * 结构体赋值把 pipe 指针也拷过去了，看着"已经对了"——但计数没加。
     * 漏掉下面那个 pipe_dup 的症状：新 fd 一 close 就把计数减到比实际
     * 引用数还少，另一个仍在使用的 fd 突然看到 EOF 或者 -1。这是本 Lab
     * 里最容易犯、也最难查的一类 bug，因为出错的地方（另一个 fd 上的
     * read 提前返回 0）离原因（这里少加了一次）很远。 */
    p->ofile[nfd] = *f;
    if (f->type == FD_PIPE) {
        pipe_dup(f->pipe, f->writable);
    }

    return nfd;
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
 * 上，这两个值就是 syscall_entry 栈帧里 push 过的 rcx 槽位、以及同一个
 * 栈帧里 push 过的用户 RSP 槽位——trap_entry.S 已经把这两者的*地址*算好，
 * 通过 r8/r9 传进来，这里原样转发给 proc.c（sys_write/sys_exit_proc/
 * sys_wait 不需要它们，因为它们不修改 rip/rsp）。
 *
 * Lab9 订正：用户 RSP 那个槽位在 Lab7/Lab8 里是 syscall_saved_user_rsp
 * 这个全局变量本身,Lab9 挪到了本进程内核栈上。原因见 trap_entry.S 里
 * syscall_entry 的函数头注释——本 Lab 第一次出现会 yield、之后还要正常
 * 返回的系统调用,全系统一份的全局变量存不住每个进程各自的用户栈指针。
 * 对 syscall_dispatch 本身没有影响,它转发的一直只是一个可写的地址。 */
/* Lab8 新增第三个参数 a2（用户态的 rdx）：sys_read(fd, buf, len) 需要
 * 三个参数，Lab6/Lab7 的两个参数位不够了。trap_entry.S 里的寄存器搬移
 * 序列跟着改成了四步旋转，见那边的注释——这是"系统调用参数个数"这件事
 * 第一次真正约束到汇编层，之前两个参数是因为恰好够用。 */
/* Lab9 新增第七个参数 gpr_snapshot：修复 fork() 丢失父进程 rbx/rbp/
 * r12-r15 的 bug，详细症状、根因、修法见 trap_entry.S 顶部那段很长的
 * "Lab9 修复：fork() 丢失父进程 callee-saved 寄存器的 bug"模块注释，
 * 这里只重复接口层面的约定：gpr_snapshot 指向 syscall_entry 换栈之后
 * 立刻 push 下来的六个寄存器快照，类型是 struct context *（借用 proc.h
 * 已有的类型，字段顺序 rbx/rbp/r12-r15 跟这份快照的内存布局完全一致，
 * 见 trap_entry.S 里 push 顺序的注释），只有 SYS_FORK 需要读它——其余
 * 系统调用不修改 rip/rsp 之外的任何寄存器，子进程也不存在，没有"给谁
 * 恢复现场"的问题。 */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                          uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot,
                          struct context *gpr_snapshot)
{
    switch (num) {
    case SYS_WRITE:
        /* Lab9：多了 fd 参数。Lab8 的 sys_write 只有 (buf, len)，看见什么
         * fd 都往串口打；现在 fd 决定往哪写，这是管道能工作的前提。 */
        return sys_write((int)a0, (const char *)a1, a2);
    case SYS_EXIT:
        sys_exit_proc((int64_t)a0);
        return 0; /* 不可达——sys_exit_proc() 内部 swtch() 回调度器，
                   * 见 proc.c 自己的注释；写 return 只是让这个 switch
                   * 分支在类型上完整，不依赖编译器能推断出它不可达。 */
    case SYS_FORK:
        return sys_fork(user_rip_slot, user_rsp_slot, gpr_snapshot);
    case SYS_EXEC:
        /* Lab9：exec 从"加载内核里那份写死的用户程序"变成"按路径加载
         * 文件系统里的 ELF"，于是多了 path/argv 两个参数。a0/a1 是用户
         * 传来的指针，这里只做类型转换、不做校验——校验是 exec_load() 的
         * 事（它比这里更清楚什么样的 path 算合法）。
         *
         * 真实内核在这一层还要做一件本 Lab 省掉的事：确认 a0/a1 确实
         * 指向调用者*自己*的用户地址空间。少了这个检查，用户程序可以
         * 传一个内核地址进来让内核去读——这是一整类权限漏洞的源头。
         * 本 Lab 统一省掉这类检查（sys_write/sys_read/sys_wait 也一样），
         * 补上它是 README 里的挑战任务（copy_from_user/access_ok）。 */
        return sys_exec((const char *)a0, (char *const *)a1,
                        user_rip_slot, user_rsp_slot);
    case SYS_WAIT:
        return sys_wait((int64_t *)a0);
    case SYS_OPEN:
        return sys_open((const char *)a0);
    case SYS_READ:
        return sys_read((int)a0, (void *)a1, a2);
    case SYS_CLOSE:
        return sys_close((int)a0);
    case SYS_PIPE:
        return sys_pipe((int *)a0);
    case SYS_DUP:
        return sys_dup((int)a0);
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
