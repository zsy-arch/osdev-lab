/* Lab2 starter (riscv64) 链接脚本。
 *
 * 相比 labs/lab01-boot-hello/starter/riscv64/linker.ld，多出来的是
 * __bss_start/__bss_end（给 boot.S 的清零循环用）和 __stack_bottom/
 * __stack_top（栈从 boot.S 的 .skip 挪到这里定义）。这几个符号本身
 * 没有 TODO——重点是"在 boot.S 里怎么用它们"，具体原因见 README.md
 * 「核心概念」一节。
 *
 * . = ALIGN(4096); 出现在 .bss 前面不是随手加的格式化空行：这个课程
 * 在设计 riscv64 版本时就是先在这里漏掉这一行,直接暴露了一个真实的
 * 链接器警告——`... has a LOAD segment with RWX permissions`。原因是
 * 没有对齐边界时,链接器可能把 .bss（需要 W）和前一个段合并进同一个
 * PT_LOAD 段,合并后的权限是所有被合并段权限的按位或,一旦这些段里有
 * 哪个需要 E（可执行）,就会得到 RWE。写内核链接脚本时应该养成习惯：
 * 任何权限需求不同的段之间都显式对齐分页边界，不依赖链接器"恰好没
 * 合并"，详见 README.md「踩坑记录」一节。
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
