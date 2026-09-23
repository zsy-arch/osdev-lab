/* Lab4 starter (x86_64) 链接脚本：从 Lab1-3 的"链接地址=加载地址"（flat,
 * 一条 `. = 0x100000` 走到底）第一次分裂成两条地址——这是本 Lab 引入
 * 高半区内核（higher-half kernel）之后不可避免的连锁改动。
 *
 * GRUB 只认物理地址：它按 Multiboot2/ELF program header 里的 p_paddr
 * 把文件内容原样拷到物理内存,从头到尾不知道、也不关心"虚拟地址"这个
 * 概念（分页硬件此刻还没开）。所以 boot.S 里从 _start 到"跳进高地址"
 * 之前的那段代码,必须能在物理低地址 0x100000 被直接执行——这段代码的
 * 链接地址（虚拟地址,VMA）必须和它的加载地址（物理地址,LMA）相等。
 *
 * 用 AT() 显式指定 LMA 解决这个矛盾：VMA（不加括号的地址）走高地址,
 * LMA（AT() 里的地址）走低地址,两者相差固定的 KERNEL_VIRT_BASE 常量。
 * .text.boot 是个例外：它必须 VMA=LMA（都是低地址）,因为这段代码执行时
 * 分页还没开,CPU 取指用的是"当前有效映射"下的地址,而这一刻的映射就是
 * "低地址=低地址"。
 *
 * 本文件大部分是固定的脚手架（.bss.boot 隔离、__bss_start_phys/
 * __kernel_phys_end 这类"高 VMA 符号在跳转前不能直接用，需要在链接期
 * 先算好物理地址版本"的写法），已经帮你写好并逐段解释了原理——真正要
 * 你做决定的只有下面标 TODO 的两处：选内核虚拟基址、把从 .text 开始的
 * 后续 section 切到高 VMA。见 README.md「核心概念」一节的完整背景。
 */
ENTRY(_start)

/* TODO 1：定义 KERNEL_VIRT_BASE 和 KERNEL_LOAD_ADDR 两个常量。
 *
 * KERNEL_LOAD_ADDR 沿用 Lab1-3 的物理加载地址：0x100000。
 *
 * KERNEL_VIRT_BASE 要满足 x86_64 canonical address 规则（bit 47 及以上
 * 必须全 0 或全 1），并且落在 PML4 的最高一项（index 511）——参考 Linux
 * -mcmodel=kernel 的习惯用法，选 0xFFFFFFFF80000000（正好是
 * -2GiB 偏移量，即 0 - 0x80000000，用 64 位无符号数表示）。
 *
 *   KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;
 *   KERNEL_LOAD_ADDR = 0x100000;
 */


SECTIONS
{
    /* .text.boot 单独一节,VMA=LMA=低地址,不套用下面的高地址偏移。 */
    . = KERNEL_LOAD_ADDR;
    __boot_load_addr = .;

    .multiboot : {
        *(.multiboot)
    }

    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* .bss.boot：boot.S 阶段 A/B 自己用的临时页表（boot_pml4 系列）和
     * 临时栈（boot_stack_*），VMA=LMA=低地址——这些结构只在分页打开之前
     * 被 32 位/尚未跳转的代码用 `mov $sym, %eax` 这类只能编码 32 位
     * 无符号立即数的指令直接引用，放进高 VMA 的 .bss 会报
     * relocation truncated to fit。已经写好，不是 TODO。 */
    .bss.boot : {
        KEEP(*(.bss.boot))
    }

    /* TODO 2：从这里开始 VMA 要跳到高地址，后续每个 section 的 LMA
     * 要用 AT() 显式指定，紧跟在 .text.boot 后面（保持物理内存连续）。
     *
     * 提示：
     *   . += KERNEL_VIRT_BASE;
     *
     *   .text : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
     *       *(.text)
     *       *(.text.*)
     *   }
     *
     *   .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
     *       *(.rodata)
     *       *(.rodata.*)
     *   }
     *
     *   .data : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
     *       *(.data)
     *       *(.data.*)
     *   }
     *
     * AT(ADDR(.text) - KERNEL_VIRT_BASE) 这个写法保证 LMA 是连续的
     * （不会在物理内存里凿出空洞），同时 VMA 是"高地址基址 + 到目前为止
     * 的累计大小"。
     */


    . = ALIGN(4096);
    __bss_start = .;
    /* 真正的内核栈（__stack_bottom/__stack_top）和 __kernel_end 故意
     * 放在 .bss 这个 output section *花括号内部*，不是花括号后面松散的
     * `.` 推进——GNU ld 对裸 `.` 赋值语句只会当成"前一个 section 的
     * 延续"去算 VMA/LMA，但不会计入任何 section 的大小，反映到 ELF 的
     * PT_LOAD program header 里，p_memsz 就不会覆盖这段范围，等于说
     * ELF 本身没有描述"这段物理内存也被内核占用着"——物理内存分配器
     * （kalloc）后续排除内核占用区间时，需要一个准确的边界。已经写好，
     * 不是 TODO。 */
    .bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        *(.bss)
        *(.bss.*)
        *(COMMON)

        . = ALIGN(16);
        __stack_bottom = .;
        . += 16384;
        __stack_top = .;

        . = ALIGN(4096);
        __kernel_end = .;
    }
    __bss_end = .;

    /* __bss_start_phys/__bss_end_phys：__bss_start/__bss_end 本身的
     * VMA 是高地址，但 boot.S 的 BSS 清零循环在跳转之前执行（分页还
     * 没开，只能用物理地址），不能直接 `mov $__bss_start, %edi`——这里
     * 在链接脚本里定义新符号，把减法运算挪到链接器做。已经写好，
     * 不是 TODO。 */
    __bss_start_phys = __bss_start - KERNEL_VIRT_BASE;
    __bss_end_phys = __bss_end - KERNEL_VIRT_BASE;

    /* __kernel_phys_end：boot.S 建临时身份映射表时，需要知道"内核在
     * 物理内存里到底占了多大范围"——用 LOADADDR() 拿 .bss 的 LMA 加上
     * .bss 的高地址大小，换算出对应的物理结束地址。已经写好，不是 TODO。 */
    __kernel_phys_end = LOADADDR(.bss) + (__bss_end - ADDR(.bss));

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
