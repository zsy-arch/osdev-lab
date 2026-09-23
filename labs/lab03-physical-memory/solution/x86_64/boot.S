/* Lab3 x86_64 solution：在 Lab2 的基础上只加一处新内容——把 Multiboot2
 * info 指针传给 kernel_main()。长模式切换、BSS 清零、栈设置这几步和
 * Lab2 完全一样，原样带过来，见 labs/lab02-kernel-entry/solution/x86_64/boot.S
 * 的注释，这里不重复。
 *
 * 关键点：GRUB 按 Multiboot2 规范，在跳到 _start 时把 boot info 结构体
 * 的物理地址放在 %ebx 里（%eax 放的是魔数 0x36D76289，用来确认真的是
 * Multiboot2 引导的，Lab1/Lab2 都没检查过这个魔数，Lab3 也不检查——
 * 教学取向，假设 GRUB 行为可信，不做防御性校验）。从 _start 到
 * _start64 之间的 32 位代码（BSS 清零、页表填充、CR4/CR3/EFER/CR0/
 * lgdt/ljmp 这一串）从头到尾都没有写过 %ebx，所以它原样存活到了
 * _start64。真正需要新增的只是 _start64 里 call kernel_main 之前那
 * 一条 `mov %ebx, %edi`，把这个指针交给 kernel_main() 的第一个参数
 * （System V AMD64 调用约定：%rdi 是第一个整数参数寄存器）——具体为什么
 * 不能直接用 %rbx/%rdi（64 位寄存器名），要用 %ebx/%edi（32 位子集），
 * 见 _start64 里那条指令上面的详细注释。
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

    /* %ebx 从 _start 一路存活到这里——中间的 BSS 清零、页表填充、
     * CR4/CR3/EFER/CR0 那一串、包括刚才的 lgdt/ljmp（far jump 只换
     * CS:EIP，不碰通用寄存器）都没有写过它，GRUB 在 _start 那一刻放进
     * 去的 Multiboot2 info 物理地址原样还在。
     *
     * 这里不能直接 mov %rbx, %rdi：%rbx 的高 32 位是不确定的——GRUB
     * 是按 Multiboot2 规范的 32 位保护模式约定设置 %ebx 的，规范只保证
     * 低 32 位，从没保证过把 %ebx 到 %rbx 之间的高 32 位清成 0（也没有
     * 义务这么做，规范写的时候 x86_64/长模式还不存在）。用 movl 写
     * 32 位目标寄存器 %edi，CPU 在长模式下会自动把 %rdi 的高 32 位清零
     * （x86_64 通用规则：目标是 32 位寄存器名时，无论来源是什么，写入后
     * 上面 32 位总是清零，不是"看情况延续旧值"），这样才能保证 %rdi 里
     * 装的是一个干净的 64 位地址，而不是"低 32 位对、高 32 位随缘"的
     * 半成品值——如果高位不是 0，kernel_main 后面把它当指针解引用就是
     * 直接读飞。 */
    mov %ebx, %edi
    call kernel_main

1:  hlt
    jmp 1b
.size _start64, . - _start64
