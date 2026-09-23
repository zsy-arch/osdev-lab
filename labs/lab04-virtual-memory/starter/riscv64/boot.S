/* Lab4 starter (riscv64)：从 satp=0（bare mode,从 Lab1 起一直如此)第一次
 * 真正打开 Sv39 分页,同时把内核变成高半区（链接在
 * 0xFFFFFFC000000000,加载在物理 0x80200000)。
 *
 * 和 x86_64 那份 boot.S 的四阶段划分基本对应,但 riscv64 这边阶段 A/B/C
 * 全是全新内容（x86_64 从长模式切换那一刻就自带临时页表,riscv64 到
 * Lab1-3 为止完全没有任何页表)。本文件已经写好了寄存器保存、循环骨架
 * 这类重复性代码，真正要你填的是几个具体的索引/地址计算，标了 TODO——
 * 完整背景见 README.md「核心概念」「分步实现步骤」两节。
 *
 * 一处提前说明：memmap_discover() 的调用时机和 x86_64 版本不一样——
 * QEMU 的 riscv64 DTB 被放在"RAM 顶部往下 2MiB"的地方，远超本文件
 * 临时页表覆盖的范围，必须趁 satp=0（bare mode，任何 64 位地址都能
 * 直接当物理地址解引用）的窗口读掉，所以这里在阶段 A、打开分页之前
 * 就调用了它——这部分已经写好，不是 TODO。
 */
.section .bss.boot, "aw", @nobits
.balign 4096
/* 阶段 A 的临时页表。riscv64 Sv39 只需要 3 级,但本 Lab 不用大页,阶段 A
 * 得手动填满第 3 级（叶子级)512 项——一张叶子表 512*4KiB=2MiB,只做
 * 2MiB 身份映射,够 kernel_boot() 自己跑起来、读 DTB、调用 kalloc 就
 * 足够了。
 *
 * boot_l2 是顶层（对应 x86_64 的 PML4),boot_l1_low/boot_l1_high 是
 * 中间层,boot_leaf 是叶子级——boot_l1_low 和 boot_l1_high 会共用同一份
 * boot_leaf（TODO 里会安装这条链路）。 */
boot_l2:
    .skip 4096
boot_l1_low:
    .skip 4096
boot_l1_high:
    .skip 4096
boot_leaf:
    .skip 4096

/* boot_l1_uart/boot_uart_leaf：单独给 UART MMIO（物理 0x10000000)搭的
 * 一条身份映射路径——riscv64 的串口是内存映射 I/O,不像 x86_64 版本走
 * in/out 端口指令,一旦 satp 生效,任何没被显式映射的物理地址（包括
 * UART)都不可访问。 */
boot_l1_uart:
    .skip 4096
boot_uart_leaf:
    .skip 4096

.balign 16
boot_stack_bottom:
    .skip 128
boot_stack_top:

.section .text.boot
.global _start
.type _start, @function
_start:
    lla t0, boot_stack_top
    mv sp, t0

    lla t0, __bss_start_phys
    lla t1, __bss_end_phys
clear_bss:
    bge t0, t1, bss_cleared
    sb zero, 0(t0)
    addi t0, t0, 1
    j clear_bss
bss_cleared:

    /* a0=hartid,a1=DTB 物理地址,从 OpenSBI 交接控制权那一刻起就是
     * 这样——挪到 s0/s1，阶段 C 跳过去之后再用。 */
    mv s0, a0
    mv s1, a1

    /* TODO 1：填叶子级 boot_leaf 的 512 项，每项映射 4KiB，覆盖
     * [KERNEL_LOAD_ADDR, KERNEL_LOAD_ADDR + 2MiB)（KERNEL_LOAD_ADDR=
     * 0x80200000）。标志位 V|R|W|X（0xf）——叶子级需要至少一个 RWX 位
     * 非零才会被硬件当成叶子，这里为了让阶段 D 的 C 代码都能正常工作，
     * 读写执行全开。
     *
     * 关键点（README「常见坑与排查」点名过的两处）：
     *   - 基地址必须是 KERNEL_LOAD_ADDR=0x80200000，不能从 0 开始数——
     *     这份 boot_leaf 对应的虚拟地址窗口是 [0x80200000, 0x80400000)，
     *     不是 [0, 0x200000)，叶子级 PTE 的 PPN 字段存的是*物理*页号，
     *     必须实打实指向内核真正加载的物理地址。
     *   - PTE 编码不是把物理地址直接 | 0xf 存进去——Sv39 的 PTE 格式是
     *     PPN 存在 bit 10 起，物理地址要先 >>12（拿到页帧号）再 <<10
     *     才是正确的 PPN 字段位置，即 `(phys_addr >> 12) << 10 | flags`。
     *     漏了这两条移位指令，硬件会把裸物理地址当 PPN 解析，算出来的
     *     "物理地址"和真正想映射的物理页差着十万八千里。
     *
     * 循环骨架（t2 是循环变量，从 0 到 511）：
     *   li t2, 0
     * fill_leaf:
     *   li t0, 0x80200000
     *   li t1, 4096
     *   mul t1, t1, t2
     *   add t0, t0, t1        # t0 = 0x80200000 + t2*4096
     *   srli t0, t0, 12
     *   slli t0, t0, 10
     *   ori t0, t0, 0xf       # t0 = 编码好的 PTE
     *   lla t1, boot_leaf
     *   slli t3, t2, 3
     *   add t1, t1, t3        # t1 = &boot_leaf[t2]
     *   sd t0, 0(t1)
     *   addi t2, t2, 1
     *   li t3, 512
     *   blt t2, t3, fill_leaf
     */


    /* TODO 2：把 boot_leaf 安装进 boot_l1_low 和 boot_l1_high 的同一个
     * 索引——中间层标志位只有 V（0x1），R=W=X=0（中间层必须 RWX 全零，
     * 否则会被当成叶子）。
     *
     * 关键点：这个索引是 1，不是 0——boot_leaf 一项 4KiB，512 项覆盖
     * 2MiB，对应中间层里的*一项*，KERNEL_LOAD_ADDR=0x80200000 正好落在
     * 第 1025 个 2MiB 窗口（0x80200000/0x200000=1025），1025 & 0x1ff=1。
     *
     * 提示（对 boot_l1_low 和 boot_l1_high 各做一次，索引都是 1*8）：
     *   lla t0, boot_leaf
     *   srli t0, t0, 12
     *   slli t0, t0, 10
     *   ori t0, t0, 0x1
     *   lla t1, boot_l1_low      # 第二次换成 boot_l1_high
     *   li t3, 1*8
     *   add t1, t1, t3
     *   sd t0, 0(t1)
     */


    /* TODO 3：把 boot_l1_low 安装进 boot_l2 顶层槽位 2（低地址身份映射），
     * 把 boot_l1_high 安装进 boot_l2 顶层槽位 258（高地址自映射）。
     *
     * 这两个索引*不是* 0 和 511（x86_64 的直觉在这里不成立）：
     *   - 身份映射覆盖的是物理地址 0x80200000 本身所在的顶层槽位：
     *     (0x80200000 >> 30) & 0x1ff = 2，不是 0——0x80200000 落在
     *     虚拟地址第二个 1GiB 区间（[0x80000000, 0xc0000000)）。
     *   - 自映射覆盖的是 KERNEL_VIRT_BASE+KERNEL_LOAD_ADDR=
     *     0xFFFFFFC080200000 所在的顶层槽位：算出来是 258，不是 511——
     *     511 是 x86_64 48 位地址"符号扩展到底"对应的 PML4 最后一项，
     *     但 Sv39 只有 39 位虚拟地址，canonical 规则只要求 bit 38 及
     *     以上符号扩展，KERNEL_VIRT_BASE truncate 到低 39 位再右移
     *     30 位取 9 位是 256（0b100000000），加上 0x80200000 落在
     *     第 2 个 1GiB 区间贡献的 2，正好是 258。
     *
     * 提示（对 boot_l1_low→boot_l2[2]、boot_l1_high→boot_l2[258] 各做
     * 一次，标志位只有 V=0x1）：
     *   lla t0, boot_l1_low
     *   srli t0, t0, 12
     *   slli t0, t0, 10
     *   ori t0, t0, 0x1
     *   lla t1, boot_l2
     *   li t3, 2*8               # 第二次换成 258*8
     *   add t1, t1, t3
     *   sd t0, 0(t1)
     */


    /* TODO 4：UART（物理 0x10000000）身份映射——只填一项叶子
     * boot_uart_leaf[0]，覆盖 [0x10000000, 0x10001000)。
     *
     * 关键点（README「常见坑与排查」点名过）：标志位是 V|R|W（0x7），
     * 不给 X（MMIO 寄存器不是代码）——Sv39 的 PTE 位布局是 bit2=W、
     * bit1=R、bit0=V，不是"V|R|W 顺理成章挨着的前三位 0x3"，0x3 只是
     * V|R，漏了 W，会导致读 UART 状态寄存器正常但写数据寄存器直接
     * store page fault。
     *
     * 提示：
     *   la t0, 0x10000000
     *   srli t0, t0, 12
     *   slli t0, t0, 10
     *   ori t0, t0, 0x7
     *   lla t1, boot_uart_leaf
     *   sd t0, 0(t1)
     *
     * 然后把 boot_uart_leaf 安装进 boot_l1_uart[128]（0x10000000 的
     * VPN[1]=(0x10000000>>21)&0x1ff=128，不是 0），标志位只有 V=0x1：
     *   lla t0, boot_uart_leaf
     *   srli t0, t0, 12
     *   slli t0, t0, 10
     *   ori t0, t0, 0x1
     *   lla t1, boot_l1_uart
     *   li t3, 128*8
     *   add t1, t1, t3
     *   sd t0, 0(t1)
     *
     * 最后把 boot_l1_uart 安装进 boot_l2[0]（0x10000000 的 VPN[2]=0，
     * 恰好是 boot_l2 的第 0 项，和内核镜像身份映射用的 boot_l2[2]是
     * 两个不同的顶层槽位，互不冲突），标志位只有 V=0x1：
     *   lla t0, boot_l1_uart
     *   srli t0, t0, 12
     *   slli t0, t0, 10
     *   ori t0, t0, 0x1
     *   lla t1, boot_l2
     *   sd t0, 0(t1)
     */


    /* 在打开分页之前调用 memmap_discover(dtb_paddr)——已经写好，
     * 不是 TODO。这里用 .dword 内嵌链接期常量的手法绕开 -mcmodel=medany
     * 的 PC 相对寻址限制（此刻 PC 是低地址，memmap_discover 链接在高
     * 地址，普通 call 编码不了这么远的跳转）。 */
    mv a0, s1
    lla t0, memmap_discover_lo_target
    ld t1, 0(t0)
    jalr ra, 0(t1)

    /* 阶段 B：装 satp 并打开分页。MODE=8（Sv39,bit 63-60),PPN 是
     * boot_l2 的物理地址右移 12 位。已经写好，不是 TODO。 */
    lla t0, boot_l2
    srli t0, t0, 12
    li t1, 8
    slli t1, t1, 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma zero, zero

    /* 阶段 C：跳到高地址。已经写好，不是 TODO。 */
    lla t0, lo_target
    ld t1, 0(t0)
    jr t1

.balign 8
lo_target:
    .dword boot_stage2_high
memmap_discover_lo_target:
    .dword memmap_discover - KERNEL_VIRT_BASE
.size _start, . - _start

.section .text
.global boot_stage2_high
.type boot_stage2_high, @function
boot_stage2_high:
    lla t0, __stack_top
    mv sp, t0
    mv a0, s0
    mv a1, s1
    call kernel_boot

1:  wfi
    j 1b
.size boot_stage2_high, . - boot_stage2_high
