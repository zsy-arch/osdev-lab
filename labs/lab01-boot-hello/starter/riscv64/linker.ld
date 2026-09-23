/* Lab1 starter (riscv64) 链接脚本。
 *
 * 0x80200000 不是可以自由选择的地址——这是 OpenSBI 在 QEMU virt
 * 机器上的固定跳转约定（OpenSBI 本身加载在 0x80000000，完成硬件
 * 初始化后跳到 0x80000000 + 2MiB 这个约定地址）。改了这个数字，
 * 内核就不会被执行到，没有任何报错提示你，行为完全不可预测。
 *
 * .text.boot 用 KEEP(*(.text.boot)) 排在最前面，理由见 boot.S
 * 顶部的注释。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

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
        *(.riscv.attributes)
        *(.comment)
        *(.eh_frame)
    }
}
