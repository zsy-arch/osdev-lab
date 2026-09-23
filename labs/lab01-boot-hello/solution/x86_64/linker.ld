/* Lab1 x86_64 链接脚本。
 *
 * .multiboot 必须落在文件最前面几个字节内——GRUB 按 Multiboot2 规范只在
 * ELF 文件开头 32768 字节内扫描 header，放太后面 GRUB 会报 "no multiboot
 * header found"。加载基址选 1MiB（0x100000）是 x86 的历史惯例：0-1MiB 这段
 * 物理地址上有实模式 IVT、BIOS 数据区、VGA 显存等一堆历史遗留占用，GRUB/多数
 * bootloader 都默认把内核加载到 1MiB 之后，避开这些区域。
 *
 * /DISCARD/ 里的 .comment/.eh_frame：.comment 是编译器版本字符串，.eh_frame
 * 是 C++ 异常处理用的栈展开表——本课程不用异常，也不希望这些无关段影响
 * ELF 的 PT_LOAD 段布局（多一个不必要的段会让 readelf -l 检查变得啰嗦）。
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

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
