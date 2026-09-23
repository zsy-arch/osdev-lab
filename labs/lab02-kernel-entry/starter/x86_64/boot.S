/* Lab2 starter (x86_64)：长模式切换的部分和 Lab1 完全一样，已经原样
 * 给好——这不是本 Lab 的教学内容,不重复考。你要填的是两处新内容：
 * 用链接脚本给的 __stack_top 设初始栈,以及跳进 C 代码之前清零 BSS。
 * 参照 README.md「核心概念」和「分步实现步骤」两节。
 */
.section .multiboot, "a"
.align 8
multiboot_header:
    .long 0xE85250D6
    .long 0
    .long multiboot_header_end - multiboot_header
    .long -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))
    .align 8
    .word 0
    .word 0
    .long 8
multiboot_header_end:

.section .bss, "aw", @nobits
.align 4096
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 4096

/* 注意：这里不再有 stack_bottom/stack_top——栈的空间由链接脚本的
 * __stack_bottom/__stack_top 定义，见 linker.ld。 */

.section .text.boot
.code32

.global _start
.type _start, @function
_start:
    /* TODO 1：把 %esp 设成链接脚本定义的 __stack_top。
     * 和 Lab1 的 mov $stack_top, %esp 是同一个动作，只是符号名换了。
     *
     * 提示：mov $__stack_top, %esp
     */


    /* TODO 2：清零 __bss_start 到 __bss_end 这段内存（逐字节即可，
     * 不追求性能）。
     *
     * 为什么要自己清零：ELF 的 .bss 段（SHT_NOBITS）只是"预留空间"，
     * ELF 规范本身并不保证加载器会把它清零——GRUB 这么做只是实现上的
     * 惯例，不是必须遵守的契约。本 Lab 的 kernel_main.c 会读一个
     * 未显式初始化的静态变量来验证这段清零逻辑是否真的生效。
     *
     * 伪代码（%edi 走地址，%ecx 走剩余字节数）：
     *   edi = __bss_start
     *   ecx = __bss_end - __bss_start
     *   while (ecx != 0) {
     *       *edi = 0
     *       edi += 1
     *       ecx -= 1
     *   }
     *
     * 提示：
     *   mov $__bss_start, %edi
     *   mov $__bss_end, %ecx
     *   sub %edi, %ecx
     *   clear_bss:
     *   cmp $0, %ecx
     *   je bss_cleared
     *   movb $0, (%edi)
     *   inc %edi
     *   dec %ecx
     *   jmp clear_bss
     *   bss_cleared:
     */


    mov $pdpt, %eax
    or $0x3, %eax
    mov %eax, (pml4)

    mov $pd, %eax
    or $0x3, %eax
    mov %eax, (pdpt)

    xor %ecx, %ecx
fill_pd:
    mov $0x200000, %eax
    mul %ecx
    or $0x83, %eax
    mov $pd, %edx
    mov %eax, (%edx, %ecx, 8)
    inc %ecx
    cmp $512, %ecx
    jl fill_pd

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    mov $pml4, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    lgdt gdt64_pointer
    ljmp $0x08, $_start64

.size _start, . - _start

.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF
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

    /* 64 位模式下重新加载一次栈指针，原因和 TODO 1 用的符号一样，
     * 只是这次要用 %rsp（64 位寄存器）。 */
    mov $__stack_top, %rsp
    call kernel_main

1:  hlt
    jmp 1b
.size _start64, . - _start64
