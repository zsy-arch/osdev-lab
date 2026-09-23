/* Lab3 x86_64 链接脚本：和 Lab2 完全一样，见
 * labs/lab02-kernel-entry/solution/x86_64/linker.ld 的注释——本 Lab 的
 * 新内容（内存探测、链表分配器）全部发生在运行期，不改变链接期的内存
 * 布局，链接脚本没有任何理由跟着变。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .multiboot : {
        *(.multiboot)
    }

    .text : {
        KEEP(*(.text.boot))
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __bss_end = .;

    . = ALIGN(16);
    __stack_bottom = .;
    . += 16384;
    __stack_top = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
