/* Lab4 riscv64 链接脚本：和 x86_64 那份（同目录树的 ../x86_64/linker.ld）
 * 同一个道理——高半区内核需要 VMA（链接地址）与 LMA（加载地址）分裂,
 * 详细原理见那份文件顶部的长注释,这里不重复,只写 riscv64 特有的地方。
 *
 * OpenSBI 的 fw_jump 固件（QEMU virt 平台默认用的那种）跳转目标是一个
 * *固定物理地址* 0x80200000,不是读 ELF 的 e_entry 字段决定的——也就是
 * "下一阶段代码必须真的存在于这个物理地址",和 x86_64 那边"GRUB 只认
 * ELF program header 的 p_paddr"是同一类约束（来源：QEMU/Zephyr 文档
 * 与社区实测,OpenSBI fw_jump 文档 fw_jump.md 明确写了"assumes a fixed
 * address of the next booting stage entry"）。所以 .text.boot 必须
 * VMA=LMA=0x80200000,不能套用后面的高地址偏移。
 *
 * Sv39 的高半区规则和 x86_64 不同：39 位虚拟地址,bit 38 决定高低半区
 * （等价于 x86_64 48 位虚拟地址下的 bit 47）,任何 bit 38 及以上不全 1
 * 的地址都不是合法的 Sv39 canonical address。KERNEL_VIRT_BASE 选
 * 0xFFFFFFC000000000——这是 xv6-riscv 实际使用的 KERNBASE 高半区映射
 * 基址,落在顶层页表（Sv39 也是 512 项）最后一项 PTE[511],和内核物理
 * 加载地址 0x80200000 之间相差的偏移量是一个巨大但固定的常量。
 */
ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFC000000000;
KERNEL_LOAD_ADDR = 0x80200000;

SECTIONS
{
    . = KERNEL_LOAD_ADDR;
    __boot_load_addr = .;

    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* .bss.boot：阶段 A 自己填的临时页表（boot_l2/boot_l1_low/
     * boot_l1_high/boot_leaf)和临时栈（boot_stack_*),VMA=LMA=低地址,
     * 和 x86_64 版本 .bss.boot 同一个理由——阶段 A 全程用 `la` 直接
     * 引用这些结构,-mcmodel=medany 下 la 展开成相对*当前 PC*（此刻是
     * 低地址)的 auipc+addi,如果这些结构放进下面的高 VMA .bss,算出来
     * 的 PC 相对偏移会超出 auipc+addi 的 ±2GiB 编码范围（和
     * memmap_discover 那次是同一类问题)。 */
    .bss.boot : {
        KEEP(*(.bss.boot))
    }

    . += KERNEL_VIRT_BASE;

    .text : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
        *(.text)
        *(.text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
        *(.data)
        *(.data.*)
        /* .sdata：riscv64 gcc 的"small data"优化——大小不超过
         * -msmall-data-limit（默认 8 字节）的已初始化全局/静态变量,
         * 编译器会把它们塞进 .sdata 而不是 .data，理由是这类变量能用
         * gp（global pointer，riscv 里固定指向 .sdata/.sbss 附近某个
         * 基址）相对寻址一条指令访问到，比 .data 里的对象少一条指令。
         * Lab5 目前没有已初始化的这类全局变量触发到这条路径，但和
         * .sbss（见下面 .bss 里的说明，那边是本 Lab 实测踩到的坑）
         * 是同一个机制的两半，为了不留下"看起来对称但只修了一半"的
         * 隐患，两个都在这里显式收进对应的 output section。 */
        *(.sdata)
        *(.sdata.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    /* 真正的内核栈（__stack_bottom/__stack_top）和 __kernel_end 故意
     * 放在 .bss 这个 output section *花括号内部*，不是花括号后面松散的
     * `.` 推进——GNU ld 对 SECTIONS 块里"某个具名 section 关闭之后、
     * 下一个具名 section 开始之前"的裸 `.` 赋值语句，只会把它们当成
     * "前一个 section 的延续"去算 VMA/LMA，但不会把这段范围计入任何
     * section 的大小，最终反映到 ELF 的 PT_LOAD program header 里，
     * p_memsz 只覆盖到 .bss 自己声明的大小为止（用 readelf -l 实测
     * 确认过，和 x86_64 版本同一个问题，不是猜测）。栈和 __kernel_end
     * 之前就是这样放在 .bss 外面的，等于说 OpenSBI 加载完内核之后，
     * ELF 本身完全没有描述"这一段物理内存其实也被内核占用着"——之所以
     * 运行起来看似正常，纯粹是因为 QEMU 会把所有 RAM 清零，而且这段
     * 紧邻内核镶像的物理内存刚好没有被别的东西声明，能用不代表描述是
     * 对的。放进花括号内部之后，这些字节就是 .bss 这个 section 自己的
     * 一部分，p_memsz 自然把它们包含进去，物理内存分配器（kalloc）
     * 后续按 ELF 里描述的范围排除内核占用区间时，才有一个准确、不用
     * 另外硬编码大小的边界可以用。 */
    .bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        *(.bss)
        *(.bss.*)
        /* .sbss：.sdata 对应的"未初始化/零初始化"版本，riscv64 gcc
         * 把 Lab5 新增的 `volatile uint64_t timer_ticks = 0;` 这类
         * 全局变量塞进了这里，而不是 .bss——这是本 Lab 实测踩到的真
         * 坑：链接脚本里如果只写 `*(.bss) *(.bss.*)`，.sbss 不会被
         * 匹配到任何具名 output section，GNU ld 默认把它按输入顺序
         * 追加在最后一个显式安排的 section 之后（这里是紧跟在
         * __kernel_end 后面），结果是 timer_ticks 的存储：(1) 落在
         * boot.S 用 __bss_start_phys/__bss_end_phys 控制的清零区间
         * *外面*，从来没被清成 0；(2) 更严重的是落在 __kernel_phys_end
         * 描述的内核物理范围*外面*，物理内存分配器（memmap_discover /
         * kalloc）当作空闲内存分配了出去，被 kernel_main.c 建页表时
         * kalloc 出来的物理页直接覆盖——观察到的症状是 timer_ticks
         * 在第一次进中断处理函数之前就已经是一个五亿多的巨大数字，
         * 而且每次中断之间的真实间隔（用 time CSR 差值验证过）其实
         * 是对的、约 100000 个 mtime 计数，说明中断本身没有以异常
         * 高的频率触发，纯粹是 timer_ticks 读到的内存已经被别人写坏。 */
        *(.sbss)
        *(.sbss.*)
        *(COMMON)

        . = ALIGN(16);
        __stack_bottom = .;
        . += 16384;
        __stack_top = .;

        . = ALIGN(4096);
        __kernel_end = .;
    }
    __bss_end = .;

    /* __bss_start_phys/__bss_end_phys：boot.S 的 BSS 清零循环在阶段 A
     * 执行（分页还没开,只能用物理地址),不能直接 `la t0, __bss_start`——
     * __bss_start 是高 VMA 符号,-mcmodel=medany 下 la 展开成 PC 相对的
     * auipc+addi,阶段 A 的 PC 是低地址,和 __bss_start 的高 VMA 之间
     * 差着整个 KERNEL_VIRT_BASE,超出 auipc+addi 的编码范围（和
     * memmap_discover 那次是同一类问题,详见 boot.S 顶部注释和阶段 A
     * 那次 jalr 前的说明)。这里同样在链接脚本里定义新符号把减法运算
     * 挪到链接期完成——为什么不能直接在 boot.S 里写
     * `la t0, __bss_start - KERNEL_VIRT_BASE` 这种表达式：其实 riscv64
     * 的 as 对 `.dword sym - CONST` 这种数据方向的表达式能处理（阶段 C
     * 那个 memmap_discover_lo_target 已经证明了),但 `la` 伪指令内部
     * 展开成的 lui/addi 系列指令,操作数是"寄存器+立即数",riscv64 的
     * as 对指令操作数里的复杂符号运算表达式支持没有数据方向那么完整,
     * 统一走"链接脚本里定义新符号"这条路更保险、和 x86_64 版本的解法
     * 也保持一致，不需要为两个架构分别记两种不同的规则。 */
    __bss_start_phys = __bss_start - KERNEL_VIRT_BASE;
    __bss_end_phys = __bss_end - KERNEL_VIRT_BASE;

    /* __kernel_phys_end：现在栈和 __kernel_end 都挪进了 .bss 的花括号
     * 内部（见上面那段长注释),__bss_end - ADDR(.bss) 这个大小天然就把
     * 栈也算进去了,__kernel_phys_end 和 __kernel_end 描述的是同一段
     * 范围在物理/虚拟地址下的两个视角,不会再出现"phys_end 只到 .bss
     * 末尾,但 end 还多算了 16KiB 栈"这种不一致。 */
    __kernel_phys_end = LOADADDR(.bss) + (__bss_end - ADDR(.bss));

    /DISCARD/ : {
        *(.riscv.attributes)
        *(.comment)
        *(.eh_frame)
    }
}
