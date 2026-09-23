/* Lab4 x86_64 链接脚本：从 Lab1-3 的"链接地址=加载地址"（flat, 一条
 * `. = 0x100000` 走到底）第一次分裂成两条地址——这是本 Lab 引入高半区
 * 内核（higher-half kernel）之后不可避免的连锁改动，不是随手加的复杂度。
 *
 * GRUB 只认物理地址：它按 Multiboot2/ELF program header 里的 p_paddr
 * 把文件内容原样拷到物理内存,从头到尾不知道、也不关心"虚拟地址"这个
 * 概念（分页硬件此刻还没开）。所以 boot.S 里从 _start 到"跳进高地址"
 * 之前的那段代码,必须能在物理低地址 0x100000 被直接执行——这段代码的
 * 链接地址（虚拟地址,VMA）必须和它的加载地址（物理地址,LMA）相等,
 * 否则 CPU 现在这条 PC 指向的地址和代码里写死的 label 地址就对不上,
 * 跑起来就是错的。
 *
 * 但内核主体（一旦跳到高地址执行）要用高地址 0xFFFFFFFF80000000 寻址
 * 自己的全局变量、字符串常量、跳转目标——这是 Lab4 选定的内核虚拟基址
 * （PML4 第 511 项,x86_64 canonical address 高半区规则天然对齐,和
 * Linux 的 -mcmodel=kernel 用法完全一致,-2GiB 偏移量算出来正好是
 * 0xFFFFFFFF80000000 - 0x100000）。
 *
 * 用 AT() 显式指定 LMA 解决这个矛盾：VMA（不加括号的地址,链接器算
 * 符号地址、生成跳转目标用的）走高地址,LMA（AT() 里的地址,GRUB 实际
 * 拷贝文件内容去的物理地址）走低地址,两者相差固定的 KERNEL_VIRT_BASE
 * 常量。objcopy/ld 生成的 ELF 里,每个 section 的 program header 会
 * 分别记录 p_vaddr（=VMA）和 p_paddr（=LMA）,GRUB 只读 p_paddr。
 *
 * .text.boot（_start 到跳转前的那段汇编,見 boot.S）是个例外：它必须
 * VMA=LMA（都是低地址）,因为这段代码执行时分页还没开、CR3 里还是
 * boot.S 自己填的临时身份映射表,CPU 取指用的是"当前有效映射"下的地址,
 * 而这一刻的映射就是"低地址=低地址"——把这段代码的 VMA 也标成高地址,
 * 链接器会用高地址算它内部的跳转 label,但 CPU 实际是在低地址执行,
 * 两者不一致,第一条跳转指令就会跳飞。
 */
ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;
KERNEL_LOAD_ADDR = 0x100000;

SECTIONS
{
    /* .text.boot 单独一节,VMA=LMA=低地址,不套用下面的高地址偏移。
     * 只放 boot.S 里 KEEP(*(.text.boot)) 这一段——_start 到 ljmp 之前
     * 的 32 位代码,以及 _start64 刚跳过来、还没来得及切到高地址栈/
     * 还在用低地址访问全局变量的那一小段(见 boot.S 里的详细注释)。
     */
    . = KERNEL_LOAD_ADDR;
    __boot_load_addr = .;

    .multiboot : {
        *(.multiboot)
    }

    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* .bss.boot：boot.S 阶段 A/B 自己用的临时页表（boot_pml4 系列）和
     * 临时栈（boot_stack_*），VMA=LMA=低地址，和 .text.boot 同一个理由——
     * 这些结构只在分页打开之前被 32 位/尚未跳转的代码用 `mov $sym, %eax`
     * 这类只能编码 32 位无符号立即数的指令直接引用，如果放进下面高 VMA
     * 的 .bss，链接器会报 "relocation truncated to fit"（实测确认过：
     * 高地址不落在无符号 32 位范围内，这条指令的操作数宽度决定了目标
     * 地址必须真的很小，不是"数值上凑巧等价"就能绕过去的，这跟下面
     * __bss_start/__bss_end 用 - KERNEL_VIRT_BASE 算物理地址是两种不同
     * 的解法：那两个符号本身还需要保留在高 VMA（真正的内核 .bss 靠它们
     * 定界），这里的 boot_* 结构则是纯粹的启动脚手架，从来不需要在跳转
     * 之后被访问，干脆让它们的 VMA 本来就是低地址，不需要算术）。 */
    .bss.boot : {
        KEEP(*(.bss.boot))
    }

    /* 从这里开始 VMA 跳到高地址,LMA 继续紧跟在 .text.boot 后面——
     * AT(ADDR(.text) - KERNEL_VIRT_BASE + KERNEL_LOAD_ADDR) 这个写法
     * 保证 LMA 是连续的（不会在物理内存里凿出空洞),同时 VMA 是
     * "高地址基址 + 到目前为止的累计大小"。后面每个 section 都复用
     * 同一个偏移量常量,不需要每次手算。
     */
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
     * 确认过，不是猜测）。栈和 __kernel_end 之前就是这样放在 .bss
     * 外面的，等于说 GRUB/固件加载完内核之后，ELF 本身完全没有描述
     * "这一段物理内存其实也被内核占用着"——之所以运行起来看似正常，
     * 纯粹是因为 QEMU 会把所有 RAM 清零，而且这段紧邻内核镶像的物理
     * 内存刚好没有被别的东西声明，能用不代表描述是对的。放进花括号
     * 内部之后，这些字节就是 .bss 这个 section 自己的一部分，
     * p_memsz 自然把它们包含进去，物理内存分配器（kalloc）后续按
     * ELF 里描述的范围排除内核占用区间时，才有一个准确、不用另外
     * 硬编码大小的边界可以用。 */
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
     * 没开，只能用物理地址），不能直接 `mov $__bss_start, %edi`——
     * 试过在 boot.S 里写 `__bss_start - KERNEL_VIRT_BASE` 直接当立即数，
     * GNU as 报 "can't resolve"：这类跨符号的减法表达式，as 在处理
     * 指令立即数时会尝试自己求值（不像 .quad/.dword 那样能退化成一条
     * 链接期重定位，那是 riscv64 版本 `.dword memmap_discover -
     * KERNEL_VIRT_BASE` 能工作、但同样的写法搬到 x86_64 这里的
     * `mov $(sym - sym), %reg` 就不行的原因——两种架构的汇编器对"跨
     * 符号减法"的重定位支持粒度不同，实测确认过，不是猜测），而
     * KERNEL_VIRT_BASE 这个符号 as 处理 .S 文件时根本看不到（只在
     * 链接脚本里定义），所以连尝试求值都做不到。这里换成在链接脚本里
     * 直接定义一个*新符号*，把减法运算挪到链接器做（链接器处理
     * SECTIONS 块里的赋值语句时,两边都是它自己刚定义的符号,运算
     * 完全在链接期完成,as 那边只会看到一个普通的、有确定值的符号），
     * boot.S 里直接 `mov $__bss_start_phys, %edi` 引用即可。 */
    __bss_start_phys = __bss_start - KERNEL_VIRT_BASE;
    __bss_end_phys = __bss_end - KERNEL_VIRT_BASE;

    /* __kernel_phys_end：上面这些符号都是高地址（VMA）,但 boot.S 里
     * 建临时身份映射表时,需要知道"内核在物理内存里到底占了多大范围"
     * 来决定身份映射要覆盖到哪——这里用 LOADADDR() 拿 .bss 的 LMA 加上
     * .bss 的高地址大小,换算出对应的物理结束地址。现在栈和
     * __kernel_end 都挪进了 .bss 的花括号内部（见上面那段长注释),
     * __bss_end - ADDR(.bss) 这个大小天然就把栈也算进去了,
     * __kernel_phys_end 和 __kernel_end 描述的是同一段范围在物理/
     * 虚拟地址下的两个视角,不会再出现"phys_end 只到 .bss 末尾,
     * 但 end 还多算了 16KiB 栈"这种不一致。写成一个链接期常量,
     * boot.S 里直接 `mov $__kernel_phys_end, %eax` 用,不需要在汇编里
     * 手动做地址翻译。 */
    __kernel_phys_end = LOADADDR(.bss) + (__bss_end - ADDR(.bss));

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
