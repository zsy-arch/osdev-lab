/* Lab1 starter (x86_64)：把下面标 TODO 的部分补完，参照 README.md
 * "分步实现步骤 / x86_64 路线" 一节，不要直接抄 solution/ ——照抄学不到
 * 长模式切换每一步为什么必须是那个顺序。
 *
 * 骨架已经搭好的部分：Multiboot2 头、页表用的 .bss 空间、GDT 数据、
 * 64 位入口跳回 C 的最后一步。你要填的是"32 位入口到长模式生效"这中间
 * 的核心步骤。
 */

.set MB2_MAGIC,        0xE85250D6
.set MB2_ARCH,          0
.set MB2_HEADER_LEN,    16
.set MB2_CHECKSUM,      -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

.section .multiboot
.align 8
multiboot_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM
    /* 结束标记：type=0, flags=0, size=8 */
    .word 0
    .word 0
    .long 8

.section .bss
.align 4096
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 4096

.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .text.boot
.code32
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp

    /* TODO 1：把 pml4 的第 0 项指向 pdpt，把 pdpt 的第 0 项指向 pd。
     * 页表项格式：物理地址（页对齐）| 标志位。本 Lab 只需要
     * Present(0x1) | Writable(0x2) 两个标志位，也就是 OR 上 0x3。
     *
     * 提示：
     *   mov $pdpt, %eax
     *   or  $0x3, %eax
     *   mov %eax, pml4
     *
     *   mov $pd, %eax
     *   or  $0x3, %eax
     *   mov %eax, pdpt
     */


    /* TODO 2：用循环填满 pd 的 512 个页表项，每项映射一个 2MiB 大页，
     * 从物理地址 0 开始连续映射，覆盖 [0, 1GiB)。
     * 2MiB 大页需要额外的 PS 标志位（bit 7，值 0x80），
     * 所以每项的标志位是 Present | Writable | PS = 0x1 | 0x2 | 0x80 = 0x83。
     *
     * 伪代码：
     *   for i in 0..512:
     *       pd[i] = (i * 0x200000) | 0x83
     *
     * 用 %ecx 做循环计数，%eax 算地址，注意每个 pd 表项是 8 字节
     * （虽然这里只用到低 32 位，寄存器名对但要记得 pd 是按 8 字节步进的）。
     */


    /* TODO 3：启用 CR4.PAE（Physical Address Extension，bit 5，值 0x20）。
     * 长模式的分页机制要求 PAE 必须先打开。
     *
     * 提示：
     *   mov %cr4, %eax
     *   or  $0x20, %eax
     *   mov %eax, %cr4
     */


    /* TODO 4：把 CR3 指向 pml4 的物理地址。
     * !!! 必须在开 CR0.PG（TODO 6）之前完成，且顺序不能反 !!!
     * 开分页那一刻如果 CR3 是无效值，CPU 会立刻三重故障重启，
     * 因为这时候还没有任何异常处理机制能接住这个错误。
     *
     * 提示：mov $pml4, %eax; mov %eax, %cr3
     */


    /* TODO 5：设置 EFER.LME（Long Mode Enable，MSR 0xC0000080 的 bit 8，
     * 值 0x100）。MSR 需要用 rdmsr/wrmsr 读写，寄存器号放在 %ecx，
     * 64 位值按 %edx:%eax（高32:低32）传递。
     *
     * 提示：
     *   mov $0xC0000080, %ecx
     *   rdmsr
     *   or  $0x100, %eax
     *   wrmsr
     */


    /* TODO 6：启用 CR0.PG（Paging，bit 31，值 0x80000000）。
     * 这一步之后 CPU 进入 IA-32e 兼容子模式，但还没有真正按 64 位解码，
     * 要等到下面的 ljmp 远跳到 64 位代码段才算真正进入长模式。
     *
     * 提示：
     *   mov %cr0, %eax
     *   or  $0x80000000, %eax
     *   mov %eax, %cr0
     */


    /* TODO 7：lgdt 加载下面定义的 gdt64_pointer，然后 ljmp 到
     * gdt64 里 64 位代码段选择子（第二个描述符，选择子值是 0x08）
     * 对应的 _start64。
     *
     * 提示：
     *   lgdt gdt64_pointer
     *   ljmp $0x08, $_start64
     */


1:  hlt
    jmp 1b
.size _start, . - _start

.align 16
gdt64:
    .quad 0                    /* 空描述符，x86 段选择子约定要求第 0 项必须是空的 */
    .quad 0x00AF9A000000FFFF   /* 64 位代码段：L 位（Long mode）已置位 */
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.code64
.global _start64
.type _start64, @function
_start64:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov $stack_top, %rsp
    call kernel_main

1:  hlt
    jmp 1b
.size _start64, . - _start64
