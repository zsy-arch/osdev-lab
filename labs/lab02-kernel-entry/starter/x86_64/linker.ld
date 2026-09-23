/* Lab2 starter (x86_64) 链接脚本。
 *
 * 相比 labs/lab01-boot-hello/starter/x86_64/linker.ld，多出来的是
 * __bss_start/__bss_end（给 boot.S 的清零循环用）和 __stack_bottom/
 * __stack_top（把栈从 boot.S 里的 .skip 挪到这里定义）。这几个符号
 * 本身没有 TODO——本 Lab 的重点是"在 boot.S 里怎么用它们"，不是考你
 * 链接脚本语法，具体原因见 README.md「核心概念」一节。
 *
 * . = ALIGN(4096); 出现在 .bss 前面不是随手加的格式化空行：如果没有
 * 这一行，链接器可能会把 .bss 和前一个段（.data，权限 RW）合并进同一个
 * PT_LOAD 段——这个特定场景不会出问题，但如果哪天 .text 和 .bss 之间
 * 缺了对齐边界，合并出的段权限是所有被合并段权限的按位或，.text 要
 * R+E、.bss 要 W，一旦被合并到一起就会得到 RWE（可写又可执行）。这正是
 * NX/W^X 这类保护机制存在的原因，写内核链接脚本时应该养成习惯：任何
 * 权限需求不同的段之间都显式对齐分页边界，不依赖链接器"恰好没合并"。
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
