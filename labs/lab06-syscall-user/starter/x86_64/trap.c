/* Lab6 x86_64：在 Lab5"IDT 只处理 #PF + 定时器"的基础上，加入真正的
 * 特权级切换机制——SYSCALL/SYSRET。这是全课程唯一一个不通过 IDT 的
 * 陷入路径：IDT/中断门是"CPU 硬件自动查表跳转"，SYSCALL 是"CPU 直接
 * 从固定的 MSR（IA32_LSTAR）里取 RIP"，两者互不相关，共存不冲突——
 * 本 Lab 的 #PF/定时器仍然走 Lab5 原样的 IDT 路径，SYSCALL 是新增的
 * 第三条陷入路径,不是对前两条的替换。
 *
 * 这个文件新增的内容分两部分：
 *   1. GDT 从 2 项扩到 6 项——不是"随便加几个描述符"，SYSCALL/SYSRET
 *      按 IA32_STAR 的位布局用固定算式推导 CS/SS selector（不查表，
 *      CPU 从 STAR 里取出 16 位 base 直接加偏移量拼出 selector），
 *      GDT 里对应位置必须真的有正确 DPL 的描述符，否则装载 CS/SS 那
 *      一步会立刻 #GP，且这个 #GP 发生在 sysret 指令本身，此时已经
 *      处于"正在切换特权级"的中间状态，调试起来比普通 #GP 更难定位。
 *   2. IA32_EFER.SCE / IA32_STAR / IA32_LSTAR / IA32_FMASK 四个 MSR 的
 *      编程——SYSCALL/SYSRET 指令本身不接受任何操作数，全部参数都来自
 *      这几个 MSR，这是"固定寄存器的秘密协议"而不是常规的函数调用
 *      约定，本 Lab 的核心教学点之一就是把这个隐藏协议显式地摆出来。
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "pit.h"
#include "syscall.h"

/* TODO 1：把 GDT 从 Lab5 的 2 项（null + 内核 CS0）扩到 6 项，并定义
 * 对应的 selector 宏。
 *
 * 顺序不是随便排的，SYSCALL/SYSRET 的 selector 算式把这个顺序焊死了，
 * 改变顺序 = 改变 SYSCALL/SYSRET 的目标 selector，不是纯粹的代码风格
 * 选择：
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
 * segment，base=0/limit=0xFFFFF/G=1，区别只在 DPL 和 Type/S 位）。
 *
 * 提示：
 * static uint64_t gdt[6] = {
 *     0x0000000000000000ull, // 0: null
 *     0x00AF9A000000FFFFull, // 1: 内核代码段 CS0，selector=0x08，DPL=0
 *     0x00AF92000000FFFFull, // 2: 内核数据段 DS0，selector=0x10，DPL=0
 *     0x00CFFA000000FFFFull, // 3: 32 位兼容模式用户代码段占位，selector=0x18
 *     0x00CFF2000000FFFFull, // 4: 用户数据段 SS3，selector=0x20，DPL=3
 *     0x00AFFA000000FFFFull, // 5: 用户代码段 CS3，selector=0x28，DPL=3，L=1
 * };
 *
 * #define GDT_SEL_KERNEL_CS 0x08
 * #define GDT_SEL_KERNEL_DS 0x10
 * #define GDT_SEL_USER_CS32 0x18
 * #define GDT_SEL_USER_SS   0x20
 * #define GDT_SEL_USER_CS   0x28
 * #define GDT_SEL_USER_SS_RPL3 (GDT_SEL_USER_SS | 3)
 * #define GDT_SEL_USER_CS_RPL3 (GDT_SEL_USER_CS | 3)
 */

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* TODO 2：实现 gdt_init()。
 *
 * 除了 Lab4/5 就有的 lgdt 部分，本 Lab 新增：lgdt 只换了 GDTR，不会
 * 自动重新加载 %ds/%es/%ss——这几个段寄存器从 boot.S 起就是 null
 * selector（Lab4/5 全程没人管过），Lab5 能正常跑是因为 64 位模式下
 * 数据段基本不做 base/limit 检查；但本 Lab 需要 %ss 变成一个真正指向
 * 内核数据段（GDT_SEL_KERNEL_DS）的 selector，因为 SYSCALL 进入内核
 * 后，尽管 CPU 会把 SS 设成 STAR[47:32]+8 对应的 selector，%ds/%es
 * 仍然维持用户态那次遗留的值不变（SYSCALL 只换 CS/SS，不换
 * DS/ES/FS/GS）——如果 %ds/%es 一直是 null，在 32 位保护模式思维下
 * 会出问题，虽然当前 64 位 long mode 下这仍然基本不生效，但把它们
 * 显式设成内核数据段 selector 是"正确"的写法，不依赖"64 位模式碰巧
 * 不检查"这个事实继续裸奔下去。
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
 *     __asm__ volatile(
 *         "mov %0, %%ds\n"
 *         "mov %0, %%es\n"
 *         "mov %0, %%ss\n"
 *         :
 *         : "r"((uint16_t)GDT_SEL_KERNEL_DS));
 * }
 */

/* 已经写好，不是 TODO：33，不是 32——见 Lab5 trap.c 对应位置的完整
 * 注释（IDT_ENTRIES 必须能容纳"最大用到的下标+1"），本 Lab 沿用 Lab5
 * 的向量分配，不新增 IDT 向量——SYSCALL 走的是 MSR，完全不经过 IDT，
 * 这里维持 Lab5 原样。 */
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
 * 固定的、内核私有的临时槅位，进内核第一件事就是把用户 RSP 存起来、
 * 换上内核栈；SYSRET 前反过来。这在单核场景下是安全的——没有第二个
 * CPU 会同时踩这个槅位，真实内核会用 per-CPU 的 swapgs+GS-relative
 * 存储，那是"同一个问题在 SMP 下的正确解"，在本课程的教学阶段属于
 * 不必要的额外复杂度。 */
uint64_t syscall_saved_user_rsp;

/* 已经写好，不是 TODO：内核栈复用 boot.S/linker.ld 里 Lab1 就存在的
 * __stack_top——本 Lab 还没有"每个任务一个独立内核栈"的概念（那是
 * Lab7 进程/调度引入 PCB 之后才有意义的东西），当前唯一的执行上下文
 * 就是"内核主循环 + 偶尔陷入的用户程序"，共用一个内核栈没有问题。 */
extern char __stack_top[];

/* TODO 3：实现 sys_write。
 *
 * 本 Lab 唯一真正做事的系统调用——把用户传进来的字符串打到串口。
 * ROADMAP 明确要求的"最小系统调用"就是这一个：验证参数传递（rdi=第
 * 一个参数，指向用户地址空间里的字符串）+ 返回值（rax=写了多少字节）
 * 这一整条路径是通的。
 *
 * user_ptr 直接当内核地址解引用，不需要做"这个地址真的属于调用者的
 * 地址空间"这类校验——本 Lab 的用户页表和内核页表是*同一份*（见
 * kernel_main.c 关于"为什么不做地址空间隔离"的说明），用户合法地址
 * 在当前页表下对内核也是合法地址，校验在本 Lab 的架构下不会改变任何
 * 行为。这一简化的边界会在后续 Lab（真正的多地址空间/fork 出现之后）
 * 变得不再成立，届时必须补上。
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

/* TODO 4：实现 sys_exit。
 *
 * 用户程序主动结束执行。本 Lab 没有进程概念（Lab7 才有），"结束"能做
 * 的最有意义的事就是把退出码打印出来、然后让内核主循环继续转——不
 * panic（用户程序正常调用 exit 退出不是一个错误条件）。
 *
 * 提示：
 * static void sys_exit(int64_t code)
 * {
 *     kprintf("user program exited with code %ld\n", code);
 * }
 */

/* TODO 5：实现 syscall_dispatch。
 *
 * syscall_entry（trap_entry.S）保存完 callee-saved 之外需要保护的
 * 寄存器之后，用 C 调用约定调这个函数——num/a0/a1 对应 SYSCALL 传参
 * 约定里的 rax（调用号）/rdi/rsi（前两个参数）。返回值通过 rax 传回
 * 用户态——沿用 Linux syscall ABI 的"rax 传返回值"是业界统一做法。
 *
 * 提示：
 * int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1)
 * {
 *     switch (num) {
 *     case SYS_WRITE:
 *         return sys_write((const char *)a0, a1);
 *     case SYS_EXIT:
 *         sys_exit((int64_t)a0);
 *         return 0;
 *     default:
 *         kprintf("syscall_dispatch: unknown syscall number %lu\n", num);
 *         return -1;
 *     }
 * }
 */

/* TODO 6：实现 syscall_init()，编程 EFER.SCE / STAR / LSTAR / FMASK
 * 四个 MSR。
 *
 * EFER.SCE：SYSCALL/SYSRET 指令默认是"存在但执行即 #UD"，必须先显式
 * 置位 EFER 里的 SCE 位才能真正使用——这跟 boot.S 里为了进 long mode
 * 已经置位过的 EFER.LME 是同一个 MSR 的两个不同 bit，互不影响，
 * boot.S 那次写 EFER 时 SCE 还是 0，这里需要单独再置一次（读-改-写，
 * 不覆盖 LME 已经置好的状态）。
 *
 * STAR 布局（仅 64 位模式下 SYSCALL/SYSRET 用到的部分）：
 *   bits [47:32] = SYSCALL 进内核时用的 CS，SS 隐含算成这个值+8。
 *   bits [63:48] = SYSRET 回用户态时的"基准值"，CS 算成这个值+16，
 *                  SS 算成这个值+8（这就是为什么 GDT 里 SS3 和 CS3
 *                  中间要空出 USER_CS32 那个占位项——16 和 8 之间
 *                  正好差 8，对应一个描述符的间隔）。
 *   bits [31:0]  = 仅 32 位模式 SYSCALL 用到的 EIP，64 位模式下这
 *                  部分被忽略，这里写 0。
 *
 * 64 位模式 SYSRET 的精确算式：
 *   CS.Selector = (STAR[63:48] + 16) OR 3
 *   SS.Selector = (STAR[63:48] +  8) OR 3
 * 这个基准值必须填 GDT_SEL_USER_CS32（0x18），不能直觉性地填
 * GDT_SEL_USER_SS（0x20）——0x18+8=0x20=USER_SS，0x18+16=0x28=USER_CS，
 * 跟 GDT 表里的实际布局对上；如果错填成 0x20，算出来 SS 会落在
 * USER_CS 的位置、CS 会落在 GDT 根本没有的第 7 项，装载 CS/SS 时
 * 立刻 #GP。
 *
 * LSTAR：SYSCALL 执行时直接把这个 MSR 的值当 RIP 跳过去（不经过 IDT，
 * 不经过 GDT 里的 CS 描述符找 base——64 位模式下代码段 base 恒为 0），
 * syscall_entry 是 trap_entry.S 里新增的汇编入口。
 *
 * FMASK：SYSCALL 进内核时，RFLAGS 会先与上 ~FMASK 再存进 R11（硬件
 * 自动做的）——FMASK 里置 1 的每一位，进内核后都会被清零。这里只清
 * IF（中断标志），让 SYSCALL 处理期间不被定时器中断打断。
 *
 * 提示：
 * void syscall_init(void)
 * {
 *     uint64_t efer = rdmsr(MSR_EFER);
 *     wrmsr(MSR_EFER, efer | EFER_SCE);
 *
 *     uint64_t star = ((uint64_t)GDT_SEL_USER_CS32 << 48) |
 *                      ((uint64_t)GDT_SEL_KERNEL_CS << 32);
 *     wrmsr(MSR_STAR, star);
 *
 *     wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
 *
 *     wrmsr(MSR_FMASK, (1ull << 9)); // IF
 * }
 */
