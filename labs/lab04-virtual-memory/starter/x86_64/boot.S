/* Lab4 starter (x86_64)：在 Lab2/3 的基础上,把内核从"链接地址=加载地址=
 * 低地址"改造成高半区内核（higher-half kernel,内核主体链接在
 * 0xFFFFFFFF80000000,实际加载在物理 0x100000)。
 *
 * 阶段 A（不变,原样带过来,不是本 Lab 的 TODO)：_start 到 _start64 之前
 * ——32 位保护模式下清 BSS、搭一份临时页表（pml4/pdpt/pd,2MiB 大页身份
 * 映射低 1GiB)、开 PAE/长模式/分页,ljmp 进 64 位。这部分和 Lab2/3 完全
 * 一样,已经帮你写好。
 *
 * 本 Lab 真正要写的是阶段 B 的"插入高地址自映射项"和阶段 C 的"跳到
 * 高地址执行"——这两处标了 TODO,完整原理见 README.md「核心概念」和
 * 「常见坑与排查」两节，这里的注释只保留必要的提示，不重复贴一遍。
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

.section .bss.boot, "aw", @nobits
.align 4096
/* 阶段 A/B 用的临时页表。放在 .bss.boot（VMA=LMA=低地址,和 .text.boot
 * 同一个理由)而不是普通 .bss——这些结构全靠 32 位/跳转前的代码直接用
 * `mov $sym, %eax` 引用,目标地址必须真的很小,放进高 VMA 的 .bss 会在
 * 链接时报 relocation truncated to fit。
 *
 * boot_pdpt_high 是 TODO（阶段 B）要用到的：它会被 PML4[511] 指向,
 * entry[510] 会指向*同一份* boot_pd——不是新建一份 PD 再填一遍 2MiB
 * 大页,是直接复用阶段 A 已经填好的那份,这样"低 1GiB 身份映射"和
 * "高地址前 1GiB 自映射"物理上是同一批 PTE、同一批物理页。 */
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096
boot_pd:
    .skip 4096

.align 16
boot_stack_bottom:
    .skip 128
boot_stack_top:

.section .text.boot
.code32

.global _start
.type _start, @function
_start:
    mov $boot_stack_top, %esp

    mov $__bss_start_phys, %edi
    mov $__bss_end_phys, %ecx
    sub %edi, %ecx
clear_bss:
    cmp $0, %ecx
    je bss_cleared
    movb $0, (%edi)
    inc %edi
    dec %ecx
    jmp clear_bss
bss_cleared:

    /* 阶段 A：和 Lab2/3 完全一样,建低 1GiB 身份映射（2MiB 大页)。 */
    mov $boot_pdpt_low, %eax
    or $0x3, %eax
    mov %eax, (boot_pml4)

    mov $boot_pd, %eax
    or $0x3, %eax
    mov %eax, (boot_pdpt_low)

    xor %ecx, %ecx
fill_pd:
    mov $0x200000, %eax
    mul %ecx
    or $0x83, %eax
    mov $boot_pd, %edx
    mov %eax, (%edx, %ecx, 8)
    inc %ecx
    cmp $512, %ecx
    jl fill_pd

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    mov $boot_pml4, %eax
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

    mov $boot_stack_top, %rsp

    /* %ebx 从 _start 一路存活到这里,装的是 GRUB 给的 Multiboot2 info
     * 物理地址,做法和 Lab3 一样。先挪到 %r15（阶段 C 跳过去之后再从
     * %r15 挪进 %rdi 传给 kernel_boot()）。 */
    mov %ebx, %r15d

    /* TODO 1（阶段 B）：往阶段 A 的临时页表里插一项，让 PML4[511]
     * （虚拟地址 0xFFFFFFFF80000000 起）指向 boot_pdpt_high，
     * boot_pdpt_high 的某一项指向*同一份* boot_pd——低 1GiB 身份映射
     * 和高地址前 1GiB 自映射，此刻要共享完全相同的物理页表项。
     *
     * 提示：
     *   1a. PML4[511] 指向 boot_pdpt_high（照抄阶段 A 里
     *       "mov $boot_pdpt_low, %eax / or $0x3, %eax /
     *       mov %eax, (boot_pml4)" 这三行的写法，目标换成
     *       boot_pml4 + 511*8）。
     *   1b. boot_pdpt_high 的哪一项指向 boot_pd？——不是 entry[0]。
     *       0xFFFFFFFF80000000 的 PDPT 索引（bit 30-38）要自己算：
     *       0xFFFFFFFF80000000 >> 30 & 0x1FF = ?
     *       算完之后，用这个索引乘 8 作为 boot_pdpt_high 内的偏移，
     *       写入 boot_pd 的物理地址 | 0x3。
     *       README「常见坑与排查」一节点名了这里最容易犯的错：抄阶段 A
     *       用 entry[0]，会导致真正需要的那一项还是空的，内核映像所在
     *       的高地址直接查表缺页——用 QEMU monitor 读 boot_pdpt_high
     *       物理内存，核对算出来的 index 对应的 8 字节是不是写对了。
     *
     * TODO 2：写完 PML4[511] 之后，用一次 "mov %cr3, %rax /
     * mov %rax, %cr3"（重新加载同一个值）隐式刷 TLB——不刷的话，接下来
     * TODO 3 的 jmp 目标可能还在用查表缓存里"这一项不存在"的旧结果，
     * 直接缺页。
     */


    /* TODO 3（阶段 C）：跳到高地址执行。
     *
     * 提示：
     *   movabs $boot_stage2_high, %rax
     *   jmp *%rax
     *
     * 必须用 movabs 装绝对地址，不能用 RIP 相对的跳转指令（比如裸的
     * `jmp boot_stage2_high`）——此刻 PC 还在低地址，RIP 相对算出来的
     * 目标会是错的低地址附近某个值。boot_stage2_high 的定义见下面，
     * 已经写好，不需要改。
     */

.size _start64, . - _start64

/* 阶段 C 的落地点：这段代码不在 .text.boot（VMA=LMA=低地址),在普通
 * .text（VMA=高地址）——执行到这里时 PC 已经真正是高地址了（TODO 3 的
 * jmp 保证了这点),高地址标号在这时候引用自己完全一致,这也是为什么
 * __stack_top 只能从这里开始用，不能在 TODO 3 之前用。这部分不是本 Lab
 * 的 TODO，已经写好。 */
.section .text
.code64
.global boot_stage2_high
.type boot_stage2_high, @function
boot_stage2_high:
    mov $__stack_top, %rsp
    mov %r15d, %edi
    call kernel_boot

1:  hlt
    jmp 1b
.size boot_stage2_high, . - boot_stage2_high
