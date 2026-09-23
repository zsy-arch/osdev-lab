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
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "pit.h"
#include "syscall.h"

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
 *
 * 描述符的 64 位打包格式跟 Lab4 的内核 CS 那一项完全一样（flat
 * segment，base=0/limit=0xFFFFF/G=1，区别只在 DPL 和 Type/S 位）——
 * 具体每个字段的位置见下面每个描述符常量后面的行内注释。 */
static uint64_t gdt[6] = {
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
};

#define GDT_SEL_KERNEL_CS 0x08
#define GDT_SEL_KERNEL_DS 0x10
#define GDT_SEL_USER_CS32 0x18 /* 占位用，本 Lab 不会被真正装载。 */
#define GDT_SEL_USER_SS   0x20
#define GDT_SEL_USER_CS   0x28
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

/* 内核栈：复用 boot.S/linker.ld 里 Lab1 就存在的 __stack_top——本 Lab
 * 还没有"每个任务一个独立内核栈"的概念（那是 Lab7 进程/调度引入 PCB
 * 之后才有意义的东西），当前唯一的执行上下文就是"内核主循环 + 偶尔
 * 陷入的用户程序"，共用一个内核栈没有问题：SYSCALL 处理期间用户程序
 * 完全不运行，不存在"内核栈被两个同时活跃的上下文并发使用"的场景。 */
extern char __stack_top[];

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

/* sys_exit：用户程序主动结束执行。本 Lab 没有进程概念（Lab7 才有），
 * "结束"能做的最有意义的事就是把退出码打印出来、然后让内核主循环继续
 * 转——不 panic（用户程序正常调用 exit 退出不是一个错误条件），也不
 * 试图真的回收什么资源（本 Lab 唯一的用户程序是内核自己在 boot 时
 * 分配的一个固定页面，没有额外资源需要释放）。 */
static void sys_exit(int64_t code)
{
    kprintf("user program exited with code %ld\n", code);
}

/* syscall_dispatch：syscall_entry（trap_entry.S）保存完 callee-saved
 * 之外需要保护的寄存器之后，用 C 调用约定调这个函数——num/a0/a1 对应
 * SYSCALL 传参约定里的 rax（调用号）/rdi/rsi（前两个参数），本 Lab
 * 两个系统调用加起来最多用到两个参数，没有实现 a2-a5（对应
 * rdx/r10/r8/r9）的转发，需要更多参数的系统调用留给以后的 Lab。
 *
 * 返回值通过 rax 传回用户态——这是 SYSCALL/SYSRET 约定的一部分（跟
 * IDT 中断门不一样，中断处理函数没有"返回值"这个概念，SYSCALL 的
 * 返回值传递完全是软件约定，不是硬件规定，但沿用 Linux syscall ABI
 * 的"rax 传返回值"是业界统一做法，本课程直接采用，不发明新约定）。 */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1)
{
    switch (num) {
    case SYS_WRITE:
        return sys_write((const char *)a0, a1);
    case SYS_EXIT:
        sys_exit((int64_t)a0);
        return 0;
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
