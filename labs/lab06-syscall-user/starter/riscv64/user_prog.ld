/* Lab6 riscv64：用户程序自己的链接脚本，和内核 linker.ld 完全独立，
 * 跟 x86_64 侧（../x86_64/user_prog.ld）同一个理由，见那份文件顶部
 * 注释。
 *
 * USER_PROG_VADDR 选 0x400000（4MiB），跟 x86_64 侧用同一个数值——
 * 这个地址在两边都只是"随便一个低地址、不跟内核自映射窗口冲突"的
 * 选择，两个架构的内核虚拟基址完全不同（x86_64 是
 * 0xFFFFFFFF80000000，riscv64 是 0xFFFFFFC000000000），0x400000 在
 * 两边都不落在各自内核的高地址窗口里，选同一个数值只是为了方便对照
 * 阅读两份 kernel_main.c，不是必须相等的架构约束。 */
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
