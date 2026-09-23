/* Lab6 x86_64：用户程序自己的链接脚本，和内核 linker.ld 完全独立
 * （不同的 ENTRY，不同的地址空间设定，不共享任何符号）——这是一个
 * "从零开始"的最小链接脚本，不是内核脚本的变体。
 *
 * USER_PROG_VADDR 选 0x400000（4MiB）：低地址（不是内核的高半区
 * 0xFFFFFFFF80000000 附近），符合"用户地址空间在低地址，内核在高
 * 地址"这个真实内核的惯例（Linux/大多数 64 位内核都是这么分的），
 * 也刻意避开 0 附近（避免"NULL 解引用刚好也命中一个映射页"这种
 * 会让 bug 更难发现的巧合）以及内核自己身份映射用到的
 * [KERNEL_LOAD_ADDR, KERNEL_LOAD_ADDR+0x200000) 范围（Lab5
 * kernel_main.c 的 self_map_end 算式）——这两个地址范围本 Lab
 * 不会重叠，kernel_main.c 的用户页表只会显式映射这一页，不会跟内核
 * 自己的映射产生冲突。
 *
 * 这个链接脚本产出的 ELF 从不会被 GRUB/QEMU 直接加载执行（不是内核
 * 镶像）——它只是 objcopy -O binary 的输入，取的是最终字节内容，
 * ELF 头/program header 本身在流程里完全不重要，只有 .text/.rodata
 * 两个 section 合并后的原始字节会被 user_blob.S 的 .incbin 吸进内核
 * 镶像。 */
ENTRY(_user_start)

USER_PROG_VADDR = 0x400000;

SECTIONS
{
    . = USER_PROG_VADDR;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
