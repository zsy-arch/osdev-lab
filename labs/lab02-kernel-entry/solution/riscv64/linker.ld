/* Lab2 riscv64 链接脚本：和 x86_64 版本教的是同一件事——暴露
 * __bss_start/__bss_end 给 boot.S 手动清零，栈的边界由链接脚本决定
 * （__stack_top），不再像 Lab1 那样在 boot.S 里就地 .skip 一块。
 * 0x80200000 加载地址、KEEP(*(.text.boot))、丢弃 .riscv.attributes
 * 这几处沿用 Lab1 的理由不变，见
 * labs/lab01-boot-hello/solution/riscv64/linker.ld 的注释，这里不重复。
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

    /* ALIGN(4096) 之前没写：不加它时，链接器发现 .bss 紧跟在 .text/.rodata
     * 后面、地址连续，会把它们合并进同一个 PT_LOAD 段——一旦合并，这个
     * 段的权限位是所有被合并节权限的按位或，.text 要 R+E，.bss 要 W，
     * 合出来就是 RWE（可写可执行）。用 `readelf -l` 能直接看到这一列，
     * 实测触发过，不是纸面推理。一个可写又可执行的段是真实的安全隐患
     * （这正是 NX/W^X 这类防护机制要堵的那类问题的雏形），也不符合本
     * Lab"用链接脚本精确控制内存布局"的目标——布局精确不该只体现在
     * 符号地址对不对，也该体现在每个段的权限恰好是它需要的那些。
     * 加上页对齐，强制 .bss 落进独立的一页，链接器就会给它单独的
     * PT_LOAD 段，权限干净地是 RW，不沾 .text 的可执行位。 */
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
        *(.riscv.attributes)
        *(.comment)
        *(.eh_frame)
    }
}
