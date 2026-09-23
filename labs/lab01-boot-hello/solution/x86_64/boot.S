/* Lab1 x86_64 solution: Multiboot2 header + 32 位保护模式 -> 64 位长模式。
 *
 * GRUB 交给我们的 CPU 状态：32 位保护模式，分页关闭，%eax = 0x36D76289
 * （Multiboot2 魔数，用来确认真的是被 Multiboot2 引导器调用的），%ebx 指向
 * Multiboot2 info 结构体（Lab1 不解析它，Lab3 拿它读内存映射）。
 *
 * 长模式（Long Mode）在 x86_64 上不是"打开一个开关"就行，是一串强制的
 * 前置条件，少一步都进不去：
 *   1. 开 PAE（Physical Address Extension，CR4.PAE）
 *   2. 必须先有页表并把 CR3 指向它——PAE 分页下如果 CR3=0 或指向垃圾，
 *      下一步开 CR0.PG 的瞬间就会触发它自己都处理不了的三重故障（Triple
 *      Fault，CPU 直接复位，QEMU 表现为“瞬间重启”），因为这时候还没有
 *      任何异常处理机制
 *   3. 用 wrmsr 写 EFER.LME（Long Mode Enable，MSR 0xC0000080 的 bit 8）
 *   4. 开 CR0.PG（分页），这一步完成后 CPU 进入"IA-32e 兼容子模式"——
 *      仍然在执行 32 位代码，但已经具备了 64 位的硬件条件
 *   5. 用 lgdt 换一张带 64 位代码段描述符（L 位=1）的 GDT
 *   6. ljmp 到那个 64 位段——这一条跳转指令执行完，CPU 才真正开始按
 *      64 位模式解码后续指令
 *
 * 页表选择：只映射前 1GiB，用 2MiB 大页（PDE 直接设 PS 位指向物理页，不需要
 * PT 这一级），是为了让 Lab1 的页表尽量简单——4 级页表的完整遍历逻辑要到
 * Lab4 才作为教学内容展开。这里的映射是恒等映射（虚拟地址=物理地址），
 * 内核暂时不需要"虚拟地址和物理地址不一样"这个能力。
 */
.section .multiboot, "a"
.align 8
multiboot_header:
    .long 0xE85250D6            /* Multiboot2 魔数 */
    .long 0                     /* architecture: 0 = i386/x86_64 保护模式 */
    .long multiboot_header_end - multiboot_header  /* header_length */
    .long -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))  /* checksum，四者之和须为 0 */
    /* 结束标签（end tag）：type=0, flags=0, size=8，Multiboot2 规范要求
     * header 必须以这个标签结束，否则 GRUB 认为 header 不完整 */
    .align 8
    .word 0
    .word 0
    .long 8
multiboot_header_end:

.section .bss, "aw", @nobits
.align 4096
/* PAE 三级页表：PML4 -> PDPT -> PD（用 2MiB 大页，不需要 PT 这一级）。
 * 每级都是 4KiB、512 个 8 字节表项，恒等映射 [0, 1GiB)。 */
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 4096

.align 16
stack_bottom:
    .skip 16384   /* 16KiB 内核启动栈，Lab1 阶段够用，Lab2 会重新规划栈布局 */
stack_top:

/* .text.boot 是特意起的名字，不是 .text——目的是让链接脚本能保证 _start
 * 排在链接结果最前面。Multiboot2 规范里 GRUB 是读 ELF 头的 e_entry 字段来
 * 决定跳去哪，理论上不依赖节的摆放顺序，但这里仍然明确控制布局，
 * 不依赖"多个目标文件混在一起时 *(.text) 恰好按我想要的顺序摆放"这个
 * 没有保证的假设——riscv64 那边就是因为这个假设不成立而排查了一阵子，
 * 见 solution/riscv64/linker.ld 的注释。 */
.section .text.boot
.code32

.global _start
.type _start, @function
_start:
    /* GRUB 留给我们的栈指针状态未定义，第一件事就是给自己一个已知的栈，
     * 哪怕接下来的代码大多是纯寄存器操作、暂时用不上栈——万一编译器生成
     * 的代码或后续调用需要栈，不能让它指向垃圾地址。 */
    mov $stack_top, %esp

    /* 逐级填页表。地址算术全在 32 位下做（这里还没进长模式，寄存器用的
     * 是 %eax/%ebx 这些 32 位名字），最终值本身没有超过 32 位范围。 */

    /* PML4[0] = pdpt 的物理地址 | Present | Writable */
    mov $pdpt, %eax
    or $0x3, %eax
    mov %eax, (pml4)

    /* PDPT[0] = pd 的物理地址 | Present | Writable */
    mov $pd, %eax
    or $0x3, %eax
    mov %eax, (pdpt)

    /* PD 的 512 个表项，每项映射一块 2MiB 大页，直接覆盖 [0, 1GiB)。
     * PDE 的 PS 位（bit 7）= 1 表示"这是大页，不用再往下一级 PT 找"，
     * 表项里存的直接是 2MiB 对齐的物理基址。 */
    xor %ecx, %ecx
fill_pd:
    mov $0x200000, %eax     /* 2MiB */
    mul %ecx                /* eax = ecx * 2MiB，即第 ecx 块 2MiB 页的物理基址 */
    or $0x83, %eax          /* Present | Writable | PS(大页) */
    mov $pd, %edx
    mov %eax, (%edx, %ecx, 8)
    inc %ecx
    cmp $512, %ecx
    jl fill_pd

    /* 步骤 1：开 PAE（CR4 bit 5） */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    /* 步骤 2：CR3 指向 PML4 */
    mov $pml4, %eax
    mov %eax, %cr3

    /* 步骤 3：EFER.LME（MSR 0xC0000080 的 bit 8） */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    /* 步骤 4：开分页（CR0 bit 31）。这一条执行完，前面填的页表如果有错，
     * CPU 会立刻三重故障重启——这是调试这段代码时"什么输出都没有,
     * QEMU 却不停重启"的典型信号，见本 Lab README 里的踩坑说明。 */
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    /* 步骤 5+6：加载 64 位 GDT，远跳转进 64 位代码段。
     * ljmp 的段选择子 0x08 对应下面 gdt64 里的第二个描述符（第一个是必须
     * 存在的 null descriptor，索引 0）。 */
    lgdt gdt64_pointer
    ljmp $0x08, $_start64

.size _start, . - _start

.align 16
gdt64:
    .quad 0                              /* null descriptor，x86 段机制强制要求 */
    .quad 0x00AF9A000000FFFF             /* 64 位代码段：L=1, DPL=0, 可执行, 可读 */
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.code64
.global _start64
.type _start64, @function
_start64:
    /* 进入 64 位模式后段寄存器的值在硬件层面已经不重要了（64 位模式下
     * CS/DS/SS 的段基址被强制视为 0），但仍然显式清一下数据段选择子，
     * 避免遗留的 32 位段值造成阅读上的困惑。 */
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov $stack_top, %rsp
    call kernel_main

    /* kernel_main 正常不会返回；如果它意外返回了，让 CPU 停在这里，
     * 而不是往后越界执行到未知内存。 */
1:  hlt
    jmp 1b
.size _start64, . - _start64
