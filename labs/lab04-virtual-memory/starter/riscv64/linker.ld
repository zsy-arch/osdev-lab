/* Lab4 starter (riscv64) 链接脚本：和 x86_64 那份（同目录树的
 * ../x86_64/linker.ld）同一个道理——高半区内核需要 VMA（链接地址）与
 * LMA（加载地址）分裂,详细原理见那份文件顶部的长注释,这里只写 riscv64
 * 特有的地方。
 *
 * OpenSBI 的 fw_jump 固件（QEMU virt 平台默认用的那种）跳转目标是一个
 * *固定物理地址* 0x80200000,不是读 ELF 的 e_entry 字段决定的——也就是
 * "下一阶段代码必须真的存在于这个物理地址",和 x86_64 那边"GRUB 只认
 * ELF program header 的 p_paddr"是同一类约束（来源：OpenSBI fw_jump
 * 文档 fw_jump.md 明确写了"assumes a fixed address of the next
 * booting stage entry"）。所以 .text.boot 必须 VMA=LMA=0x80200000,
 * 不能套用后面的高地址偏移。
 *
 * 本文件大部分是固定的脚手架（.bss.boot 隔离、__bss_start_phys/
 * __kernel_phys_end 这类"高 VMA 符号在跳转前不能直接用，需要在链接期
 * 先算好物理地址版本"的写法，和 x86_64 版本同一套理由），已经帮你写好
 * 并逐段解释了原理——真正要你做决定的只有下面标 TODO 的两处：选内核
 * 虚拟基址、把从 .text 开始的后续 section 切到高 VMA。见 README.md
 * 「核心概念」一节的完整背景。
 */
ENTRY(_start)

/* TODO 1：定义 KERNEL_VIRT_BASE 和 KERNEL_LOAD_ADDR 两个常量。
 *
 * KERNEL_LOAD_ADDR 沿用 Lab1-3 的物理加载地址,同时也是 OpenSBI
 * fw_jump 固定跳转目标：0x80200000。
 *
 * KERNEL_VIRT_BASE 要满足 Sv39 canonical address 规则——39 位虚拟
 * 地址,bit 38 及以上必须全 0 或全 1（不是 x86_64 的 bit 47，Sv39 比
 * x86_64 少 9 位虚拟地址位宽）。参考 xv6-riscv 实际使用的 KERNBASE
 * 高半区映射基址,选 0xFFFFFFC000000000。
 *
 *   KERNEL_VIRT_BASE = 0xFFFFFFC000000000;
 *   KERNEL_LOAD_ADDR = 0x80200000;
 */


SECTIONS
{
    /* .text.boot 单独一节,VMA=LMA=低地址,不套用下面的高地址偏移。 */
    . = KERNEL_LOAD_ADDR;
    __boot_load_addr = .;

    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* .bss.boot：boot.S 阶段 A 自己填的临时页表（boot_l2/boot_l1_low/
     * boot_l1_high/boot_leaf/boot_l1_uart/boot_uart_leaf）和临时栈
     * （boot_stack_*），VMA=LMA=低地址——这些结构全靠阶段 A 的 `la`
     * 直接引用，-mcmodel=medany 下 la 展开成相对当前 PC 的
     * auipc+addi，放进高 VMA 的 .bss 会导致偏移超出编码范围（和
     * x86_64 版本"32 位立即数编码不了高地址"是同一类问题，只是
     * riscv64 这边报错方式不同）。已经写好，不是 TODO。 */
    .bss.boot : {
        KEEP(*(.bss.boot))
    }

    /* TODO 2：从这里开始 VMA 要跳到高地址，后续每个 section 的 LMA
     * 要用 AT() 显式指定，紧跟在 .text.boot 后面（保持物理内存连续）。
     *
     * 提示（和 x86_64 版本的写法完全一样，只是常量名的值不同）：
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
     * VMA 是高地址，但 boot.S 的 BSS 清零循环在阶段 A 执行（分页还
     * 没开，只能用物理地址），不能直接 `la t0, __bss_start`——这里
     * 在链接脚本里定义新符号，把减法运算挪到链接器做。已经写好，
     * 不是 TODO。 */
    __bss_start_phys = __bss_start - KERNEL_VIRT_BASE;
    __bss_end_phys = __bss_end - KERNEL_VIRT_BASE;

    /* __kernel_phys_end：boot.S 建临时身份映射表时，需要知道"内核在
     * 物理内存里到底占了多大范围"——用 LOADADDR() 拿 .bss 的 LMA 加上
     * .bss 的高地址大小，换算出对应的物理结束地址。已经写好，不是 TODO。 */
    __kernel_phys_end = LOADADDR(.bss) + (__bss_end - ADDR(.bss));

    /DISCARD/ : {
        *(.riscv.attributes)
        *(.comment)
        *(.eh_frame)
    }
}
