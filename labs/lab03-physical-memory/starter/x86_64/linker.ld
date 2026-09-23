/* 与 solution 完全相同——本 Lab 的新内容（内存探测、链表分配器）全部
 * 发生在运行期，不改变链接期的内存布局，链接脚本不需要任何改动。
 * 见 labs/lab03-physical-memory/solution/x86_64/linker.ld 的注释。
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
