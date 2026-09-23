/* Lab1 starter (x86_64) 链接脚本。
 *
 * 加载地址 0x100000（1MiB）是 x86 传统的"安全"内核加载地址，
 * 避开了实模式时代 640KB 以下的各种保留区域。这个值本身不需要你改，
 * 但要理解它是链接脚本和 boot.S 共同依赖的一个前提：GRUB 会按这个地址
 * 把内核加载到内存，你的代码里任何"当前指令在哪个地址"的假设都建立在
 * 这个数字上。
 *
 * .text.boot 用 KEEP(*(.text.boot)) 排在最前面：确保 _start 出现在
 * 链接后的最前面。虽然 Multiboot2/GRUB 是按 ELF 的 e_entry 字段跳转
 * （不像 riscv64 那样硬编码地址），理论上不严格依赖这个顺序，但保持
 * 这个模式没有成本，也让两个架构的链接脚本结构保持一致。
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
