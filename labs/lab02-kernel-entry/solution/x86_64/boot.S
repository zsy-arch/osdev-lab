/* Lab2 x86_64 solution：在 Lab1 的长模式切换基础上，加两步 Lab2 的教学内容——
 * 显式清零 BSS、用链接脚本给的 __stack_top 符号设栈，而不是自己在 .bss
 * 里就地开栈空间（对比 labs/lab01-boot-hello/solution/x86_64/boot.S 就能
 * 看出这两处差异，其它长模式切换步骤完全不变，因为那是 Lab1 的教学内容，
 * Lab2 不重复讲）。
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

/* 注意：这里不再有 stack_bottom/stack_top ——栈的空间由链接脚本的
 * __stack_bottom/__stack_top 定义，见 linker.ld。boot.S 只管"用"这个
 * 符号，不管"栈到底摆在内存的什么位置、多大"，这正是本 Lab 想演示的
 * 职责划分：内存布局是链接脚本的事，指令逻辑是汇编代码的事。 */

.section .text.boot
.code32

.global _start
.type _start, @function
_start:
    /* 用链接脚本定义的符号做初始栈指针。GRUB 交给我们的栈状态未定义，
     * 这一步和 Lab1 完全一样，只是符号名字换成了链接脚本给的
     * __stack_top，而不是本文件里自己 .skip 出来的 stack_top。 */
    mov $__stack_top, %esp

    /* --- Lab2 新增：手动清零 BSS --- */
    /* __bss_start/__bss_end 是链接脚本定义的符号（本身没有大小，只是
     * 地址标记），链接后就是两个具体的常量地址，这里当成 32 位立即数用。
     * 逐字节清零：教学取向，不追求用 rep stosb（字符串指令按 4/8 字节块
     * 清零会更快）之类的优化写法——一眼看懂"从哪到哪、清成什么"比性能
     * 更重要，本课程后面也不会再回头优化这段代码。 */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx          /* ecx = 要清零的字节数 = __bss_end - __bss_start */
clear_bss:
    cmp $0, %ecx
    je bss_cleared
    movb $0, (%edi)
    inc %edi
    dec %ecx
    jmp clear_bss
bss_cleared:

    /* 从这里开始，pml4/pdpt/pd 三块 BSS 空间保证是全 0——下面的页表
     * 填充逻辑（PML4[0]/PDPT[0]/PD 的 512 项）会覆盖它需要用到的字节，
     * 但页表以外、同样落在 BSS 里的其它变量（本 Lab 目前没有，Lab3
     * 引入物理内存分配器的静态数据结构之后就会有）现在有了明确保证：
     * 内核看到的初始值就是 0，不依赖 GRUB/QEMU 加载器的隐含行为。 */

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

    /* 64 位模式下重新加载一次栈指针——%esp 在 32 位阶段已经设过一次，
     * 长模式下寄存器宽度变成 64 位，%rsp 的高 32 位在切换过程中是
     * 未定义的，显式重设一次避免依赖这个未定义状态。 */
    mov $__stack_top, %rsp
    call kernel_main

1:  hlt
    jmp 1b
.size _start64, . - _start64
