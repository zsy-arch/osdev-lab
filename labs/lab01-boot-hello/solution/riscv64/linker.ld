/* Lab1 riscv64 链接脚本。
 *
 * 0x80200000 不是随便选的：QEMU virt 机器把 OpenSBI 固件加载在
 * 0x80000000（M 模式），OpenSBI 完成硬件初始化后，按约定跳到
 * 0x80200000 把控制权交给 S 模式的内核/引导程序——这是 QEMU
 * `-bios default`（即内置 OpenSBI）的固定约定,不是可以随便改的参数。
 *
 * 特意手写这份脚本而不是用 -Ttext= 简写：见 boot.S 顶部注释里记录的
 * .riscv.attributes 段错位问题——用 -Ttext= 会让链接器把这个属性段
 * 折进和 .text 相同的 PT_LOAD 段并排在它前面，实际加载地址会比预期
 * 少 0x1000。这里显式 /DISCARD/ 掉它，从根上避免这个坑。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

    .text : {
        /* _start 必须是链接后落在 0x80200000 的第一条指令：OpenSBI 交接时
         * 是直接跳到这个固定地址执行，不会去读 ELF 头里的 entry point——
         * 那个字段只有 GDB 之类会解析 ELF 的调试器才关心。如果不显式指定
         * boot.o 的 .text 排最前面，链接器按输入文件在命令行/归档里的
         * 顺序摆放各个 .text，多个 .o 混在一起时哪个先哪个后完全不保证
         * 是 boot.o——实测：不加这一行时 console.o 的 console_puts()
         * 被摆在了最前面，OpenSBI 跳过去执行的第一条指令变成了
         * console_puts 内部的栈操作，Hello 消息永远打不出来，且没有
         * 任何报错，因为这些指令本身是合法的、只是不该在这时候执行。 */
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
