/* 与 solution 完全相同——启动/长模式切换/BSS 清零/栈设置/把 Multiboot2
 * info 指针传给 kernel_main() 都不是本 Lab 的教学内容（前几个 Lab 已经
 * 讲过），本 Lab 要写的是 memmap.c 和 kernel_main.c。见
 * labs/lab03-physical-memory/solution/x86_64/boot.S 的注释。 */
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

.section .text.boot
.code32

.global _start
.type _start, @function
_start:
    mov $__stack_top, %esp

    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
clear_bss:
    cmp $0, %ecx
    je bss_cleared
    movb $0, (%edi)
    inc %edi
    dec %ecx
    jmp clear_bss
bss_cleared:

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

    mov $__stack_top, %rsp

    /* %ebx 从 _start 一路存活到这里，装的是 GRUB 给的 Multiboot2 info
     * 物理地址。用 32 位目标寄存器 %edi（不是 %rdi）是因为长模式下写
     * 32 位寄存器会自动清零上 32 位，保证 kernel_main 拿到的是一个
     * 干净的 64 位地址——详见 solution 版本这一行上面的完整注释。 */
    mov %ebx, %edi
    call kernel_main

1:  hlt
    jmp 1b
.size _start64, . - _start64
