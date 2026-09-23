/* Lab4 riscv64：从 satp=0（bare mode,从 Lab1 起一直如此)第一次真正
 * 打开 Sv39 分页,同时把内核变成高半区（链接在
 * 0xFFFFFFC000000000,加载在物理 0x80200000)。
 *
 * 和 x86_64 那份 boot.S（同目录树 ../x86_64/boot.S)的四阶段划分基本
 * 对应,阶段划分的理由在那份文件顶部写得更详细,这里只写 riscv64 的
 * 具体实现,重复的原理不再展开——但有一处*不*对应,先说清楚：
 * memmap_discover() 的调用时机两边不一样（x86_64 在阶段 D 调,
 * riscv64 在阶段 A 调),原因见下面阶段 A 说明的最后一段。
 *
 *   阶段 A（riscv64 全新——x86_64 从长模式切换那一刻就自带临时页表,
 *     riscv64 到现在为止（Lab1-3)完全没有任何页表,阶段 A 要从零开始
 *     搭)：清 BSS,填一份临时 Sv39 页表,同时做到"低 1GiB 身份映射"
 *     和"高地址前 1GiB 自映射"（riscv64 没有 x86_64 那种"先只做低
 *     映射,过一会再插高地址项"的顺序要求——Sv39 分页此刻还没打开,
 *     随便什么顺序填页表都不会被"当前生效的映射"影响,所以这里一次
 *     性把两份都填好,比 x86_64 版本还简单一点)。
 *
 *     填完页表*但在打开分页之前*,这里插入了一次 memmap_discover()
 *     调用（C 函数,用阶段 A/B 共用的低地址临时栈 boot_stack_top）——
 *     这是跟 x86_64 版本不对应的地方,原因是实测出来的一个具体问题：
 *     GRUB 把 Multiboot2 info 结构体贴着内核镜像放在低地址（实测
 *     0x108c10,在阶段 A 的低 1GiB 身份映射范围内,x86_64 阶段 D 才调
 *     完全没问题),但 QEMU 的 riscv64 virt 机器把自动生成的 DTB放在
 *     "RAM 顶部往下 2MiB"（实测 128MiB 内存下是 0x87e00000,不同
 *     内存大小下验证过是同一个规律：base+size-0x200000),远超阶段 A
 *     临时页表覆盖的低 2MiB 身份映射范围——如果留到阶段 D（分页已经
 *     靠这份临时表打开)才调 memmap_discover(),读 DTB 那一刻会立刻
 *     触发缺页异常（此时 Lab4 的 #PF 处理 stub 还没装好,会直接三次
 *     故障重启)。挪到阶段 A、分页完全没打开之前调用,是绕开这个问题
 *     最直接的办法：bare mode 下任何 64 位物理地址都能直接解引用,
 *     不需要为 DTB 专门建映射,不需要假设/硬编码 QEMU 具体把 DTB 放
 *     在哪。代价是 memmap_discover() 现在在"还没有通常意义上的高
 *     地址 __stack_top 栈"这个阶段被调用——但这不是真的代价：C 函数
 *     不关心栈具体在哪,只要栈有效,阶段 A/B 的低地址临时栈完全够用
 *     （memmap_discover() 内部只有循环和函数调用,没有深栈需求)。
 *
 *   阶段 B（riscv64 全新)：csrw satp 打开 Sv39——这是*第一次*开
 *     分页,不是换根。sfence.vma 刷 TLB（原理见 pagetable.c 里
 *     pagetable_activate() 的注释,这里提前手写一次同样的序列,原因
 *     和 x86_64 版本一样：接下来阶段 C 要跳到刚生效的高地址映射,
 *     必须先确保这份映射不会被过期的 TLB 缓存挡住——不过 riscv64
 *     这里其实没有"过期缓存"这个风险,bare mode 下 TLB 本来就没有
 *     缓存任何翻译结果,这条 sfence.vma 严格来说是防御性的,但和
 *     x86_64 版本保持同样的写法/同样的心智模型,方便对照阅读)。
 *
 *   阶段 C（riscv64 全新)：跳到高地址。riscv64 的 jalr/auipc 是 PC
 *     相对的（auipc 拿当前 PC 高 20 位,加上 12 位立即数),不能像
 *     x86_64 的 movabs 那样直接装一个 64 位绝对立即数——riscv64 装
 *     64 位绝对地址的标准做法是 la（load address,汇编器展开成一串
 *     lui/addi 或者走 GOT,由 -mcmodel=medany 决定具体展开方式),但
 *     la 展开出来的序列本身默认是 PC 相对的（medany 的核心特性),
 *     在低 PC 执行时算出来的还是错的低地址。这里改用最原始的方式：
 *     `.dword` 在 .text.boot 里内嵌一个 64 位常量（链接器直接填链接
 *     地址,不经过任何 PC 相对计算),用 ld 指令把它当数据读出来,再
 *     jalr 过去——这是 riscv64 汇编里"我要一个不依赖当前 PC 的绝对
 *     地址"的标准手法。
 *
 *   阶段 D（C 代码,kernel_boot(),见 kernel_main.c)：跟 x86_64 版本
 *     少一步——memmap_discover() 已经在阶段 A 调完了,这里只需要建
 *     正式页表、切换过去、进入 kernel_main() 的演示逻辑。 */
.section .bss.boot, "aw", @nobits
.balign 4096
/* 放在 .bss.boot（linker.ld 里 VMA=LMA=低地址,和 .text.boot 同一个
 * 理由,见那份文件对应位置的注释)而不是普通 .bss——下面这些结构全靠
 * 阶段 A 的 `la` 直接引用,-mcmodel=medany 下 la 展开成相对当前 PC 的
 * auipc+addi,放进高 VMA 的 .bss 会导致偏移超出编码范围（实测确认过,
 * 不是猜测)。
 *
 * 阶段 A 的临时页表,和 x86_64 版本的 boot_pml4 系列同一个角色（启动
 * 脚手架,不是 Lab4 最终产出)。riscv64 Sv39 只需要 3 级,但本 Lab
 * 不用大页（教学取向和 x86_64 版本一致,先讲清楚"3 级都要走一遍",大页
 * 留给挑战任务),所以阶段 A 得手动填满第 3 级（叶子级)512 项——这一点
 * 和 x86_64 版本"直接用 PD 大页,一项覆盖 2MiB,512 项覆盖 1GiB"不同,
 * riscv64 版本的叶子级一项只覆盖 4KiB,512 项只覆盖 2MiB,要填两张
 * 叶子表才能凑够 1GiB 身份映射——这里为了控制启动阶段汇编代码的量,
 * 只做 4MiB（boot_leaf_low 一张表,512*4KiB=2MiB,不够,见下方循环
 * 说明)身份映射,够 kernel_boot() 自己跑起来、读 DTB、调用 kalloc 就
 * 足够了（正式的完整映射由阶段 D 的 C 代码,用 pagetable_map(),按
 * Lab3 memmap_discover() 探测出来的实际可用内存范围来建,不需要阶段
 * A 就贪心地覆盖一大片)。
 *
 * boot_l2 是顶层（对应 x86_64 的 PML4),boot_l1_low/boot_l1_high 是
 * 中间层,boot_leaf 是叶子级——boot_l1_low 和 boot_l1_high 的 entry[0]
 * 都指向*同一份* boot_leaf,和 x86_64 版本"boot_pdpt_low/high 共享
 * 同一份 boot_pd"是同一个思路。 */
boot_l2:
    .skip 4096
boot_l1_low:
    .skip 4096
boot_l1_high:
    .skip 4096
boot_leaf:
    .skip 4096

/* boot_l1_uart/boot_uart_leaf：单独给 UART MMIO（物理 0x10000000)搭的
 * 一条身份映射路径,不能复用 boot_l1_low/boot_leaf——0x10000000 的
 * VPN[2]=0,和内核镜像所在的 VPN[2]=2 是 boot_l2 里两个不同的顶层槽位,
 * 必须分别建中间层。之所以需要这条映射：riscv64 的串口是内存映射 I/O
 * （console_putc.c 里 `*(volatile uint8_t *)0x10000000`),不像 x86_64
 * 版本走 in/out 端口指令——端口 I/O 不经过页表,MMIO 经过页表,一旦
 * satp 生效,任何没被显式映射的物理地址（包括 UART)都不可访问。这里
 * 只映射 UART 所在的这一个 4KiB 页,不是一整个 1GiB/2MiB 窗口,足够
 * console_putc() 在阶段 C 跳到高地址之后、阶段 D 的 kernel_boot() 建好
 * 正式页表并切换之前，这段窗口期内继续工作（这段窗口期确实会调用
 * console_puts_line,见 kernel_main.c 开头的 "Hello OS" 打印),
 * kernel_boot() 自己建的正式页表另外需要一份独立的 UART 映射,不能靠
 * 阶段 A 这份临时表续命——这两处都要修,见 boot_l1_uart 安装点和
 * kernel_main.c 里的注释。 */
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

    /* __bss_start/__bss_end 本身是高 VMA 符号——不能直接
     * `la t0, __bss_start`,原因和上面 .bss.boot 说明一样（PC 相对
     * 偏移超出编码范围)，也不能像 memmap_discover_lo_target 那样简单
     * 套用 `.dword sym - KERNEL_VIRT_BASE` 这个技巧,因为这里需要的是
     * *地址*本身进寄存器,不是一次性经过 jalr 消费掉的跳转目标,直接用
     * la 加载 .dword 里存的常量当然也可以,但链接脚本里定义
     * __bss_start_phys/__bss_end_phys 这两个新符号更直接——把减法运算
     * 挪到链接期,boot.S 里退化成两条普通的 `lla`（不是 la——这份
     * freestanding 内核没有 GOT,用 lla 保证展开成直接的 auipc+addi,
     * 不会被某些 riscv64-linux-gnu 工具链默认展开成经 GOT 间接寻址,
     * 链接期报 relocation truncated to fit: R_RISCV_GOT_HI20),不需要
     * 额外一层 .dword+ld 中转,和 x86_64 版本的解法保持一致（那边同样
     * 在 linker.ld 里定义了 __bss_start_phys/__bss_end_phys)。 */
    lla t0, __bss_start_phys
    lla t1, __bss_end_phys
clear_bss:
    bge t0, t1, bss_cleared
    sb zero, 0(t0)
    addi t0, t0, 1
    j clear_bss
bss_cleared:

    /* a0=hartid,a1=DTB 物理地址,从 OpenSBI 交接控制权那一刻起就是
     * 这样,到目前为止的所有代码都没碰过 a0/a1——挪到 s0/s1（callee-
     * saved,但这里同 x86_64 版本一样,只是借用"接下来这几行不会碰
     * 它们"这个事实,不是真的在遵守调用约定)存到阶段 C 跳过去之后。 */
    mv s0, a0
    mv s1, a1

    /* 阶段 A：填叶子级——每项映射 4KiB,物理地址 = KERNEL_LOAD_ADDR +
     * index * 4096,覆盖 [KERNEL_LOAD_ADDR, KERNEL_LOAD_ADDR + 2MiB)。
     * 标志位 V|R|W|X（0xf)：叶子级需要至少一个 RWX 位非零才会被硬件
     * 当成叶子（见 pagetable.c 模块注释),这里为了让阶段 D 的 C 代码
     * （包括指令本身的取指)都能正常工作,读写执行全开——这是启动脚手架
     * 专属的宽松权限,阶段 D 建的正式页表会按 Lab4 的 pagetable_map()
     * 调用点分别收紧。
     *
     * 基地址是 KERNEL_LOAD_ADDR,不是 0：这份 boot_leaf 被装进
     * boot_l1_low/boot_l1_high 的 entry[1]（见下面那段索引推导的
     * 长注释),对应的虚拟地址窗口是 [0x80200000, 0x80400000),不是
     * [0, 0x200000)——叶子级 PTE 的 PPN 字段存的是*物理*页号,不管
     * 走 identity 还是 self-map 哪条路径翻译过来,落在这份表上的
     * 每一项都必须实打实指向内核真正加载的物理地址 0x80200000 开始
     * 的那 2MiB,不能从 0 开始数（最初就是这样写的,构建能通过,但
     * 实测在 QEMU 里会在 csrw satp 之后立刻卡进无限重复的
     * exec_page_fault——PTE 存在但指向的物理页不对,取指读到的是
     * OpenSBI 固件自己的代码/数据而不是内核镜像,继续执行的下一条
     * 指令落在错误内容里,大概率不是合法指令,或者干脆再次不可执行,
     * 用 -d int,cpu_reset 抓日志确认过,不是猜测)。
     *
     * PTE 格式：Sv39 的 PTE 不是直接存物理地址,而是
     * PTE[53:10]=PPN,即 `(phys_addr >> 12) << 10 | flags`——和下面
     * boot_l1_low/boot_l1_high/boot_l2 填中间层时用的 srli 12/slli 10
     * 是同一套编码,叶子级没有理由不一样。最初这里漏了这两条移位指令,
     * 直接把 `phys_addr | 0xf` 当成 PTE 存进去——链接、构建都不报错
     * （纯算术,汇编器不检查语义),但把裸物理地址错当 PTE 存进去之后,
     * 硬件会把这段位当 PPN 解析,算出来的"物理地址"变成
     * `(phys_addr | 0xf) >> 10 << 12`,和真正想映射的物理页差着十万
     * 八千里（比如 phys_addr=0x80200000 时算出来是 0x200800000,QEMU
     * -machine virt 的 RAM 根本不到那么大),取指自然失败——实测表现
     * 正是 csrw satp 之后立刻在 sfence.vma 这条指令自己所在的页
     * 上取指失败,反复卡死在同一个 epc,用 -d int,cpu_reset 抓日志、
     * 反推 PTE 编码格式确认过,不是猜测。 */
    li t2, 0
fill_leaf:
    li t0, 0x80200000
    li t1, 4096
    mul t1, t1, t2
    add t0, t0, t1
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0xf
    lla t1, boot_leaf
    slli t3, t2, 3
    add t1, t1, t3
    sd t0, 0(t1)
    addi t2, t2, 1
    li t3, 512
    blt t2, t3, fill_leaf

    /* boot_l1_low[1] -> boot_leaf,boot_l1_high[1] -> 同一份
     * boot_leaf——中间层标志位只有 V（0x1),R=W=X=0,理由见
     * pagetable.c 模块注释："中间层必须 RWX 全零,否则被当成叶子"。
     *
     * 这里是 1,不是 0：boot_leaf 是叶子级,一项 4KiB,512 项刚好覆盖
     * 2MiB（fill_leaf 循环注释里已经写了"只做 2MiB,不够用"这个已知
     * 取舍),对应中间层（boot_l1_*)里的*一项*。KERNEL_LOAD_ADDR=
     * 0x80200000 正好落在第 1025 个 2MiB 窗口（0x80200000/0x200000=
     * 1025),1025 & 0x1ff = 1——不是第 0 个窗口。identity 和 self-map
     * 两条路径在这里用的是同一个索引 1,不是凑巧：KERNEL_VIRT_BASE=
     * 0xFFFFFFC000000000 本身是 1GiB 对齐（低 30 位全零),所以"身份地址"
     * 和"高地址自映射地址"只在 VPN[2]（顶层)上不同,VPN[1]/VPN[0]
     * 完全相等,boot_l1_low/boot_l1_high 才能安全地共用同一个索引、
     * 指向同一份 boot_leaf——这一点在 boot_l2 顶层索引 2/258 那段
     * 长注释里详细算过,这里不重复推导。 */
    lla t0, boot_leaf
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x1
    lla t1, boot_l1_low
    li t3, 1*8
    add t1, t1, t3
    sd t0, 0(t1)
    lla t1, boot_l1_high
    li t3, 1*8
    add t1, t1, t3
    sd t0, 0(t1)

    /* boot_l2[2] -> boot_l1_low（低地址身份映射,覆盖 KERNEL_LOAD_ADDR
     * =0x80200000 所在的那 1GiB),boot_l2[258] -> boot_l1_high（高地址
     * 自映射,覆盖 KERNEL_VIRT_BASE+KERNEL_LOAD_ADDR 所在的那 1GiB)。
     *
     * 这两个索引*不是* 0 和 511——最初照抄 x86_64 版本的心智模型写成
     * boot_l2[0]/boot_l2[511],链接、构建都不报错,但实测在 QEMU 下
     * boot 会在 Stage B 的 csrw satp 之后立刻卡进一个无限重复的
     * exec_page_fault（epc 反复落在 0x80200000/0x802000e8,即内核自己
     * 的加载地址),用 `-d int,cpu_reset` 抓日志确认过,不是猜测。
     * 根因是 Sv39 的顶层索引公式和 x86_64 的 PML4 索引公式不是一回事：
     *
     *   - 身份映射需要覆盖的是物理地址 KERNEL_LOAD_ADDR=0x80200000 本身
     *     所在的顶层槽位,算出来是 (0x80200000 >> 30) & 0x1ff = 2,不是
     *     0——0x80200000 落在虚拟地址第二个 1GiB 区间（[0x40000000,
     *     0x80000000)是第 1 个,[0x80000000, 0xc0000000)是第 2 个,
     *     0x80200000 落在后者),不是第 0 个。
     *
     *   - 自映射需要覆盖的是 KERNEL_VIRT_BASE+KERNEL_LOAD_ADDR=
     *     0xFFFFFFC080200000 所在的顶层槽位,算出来是
     *     (0xFFFFFFC080200000 >> 30) & 0x1ff = 258,不是 511——511 是
     *     x86_64 48 位虚拟地址下"符号位扩展到底"对应的 PML4 最后一项,
     *     但 Sv39 是 39 位虚拟地址,canonical 规则只要求 bit 38 及以上
     *     符号扩展,KERNEL_VIRT_BASE=0xFFFFFFC000000000 truncate 到低
     *     39 位是 0x4000000000,右移 30 位取 9 位是 256（0b100000000),
     *     不是 511（0b111111111)——两种架构的"高地址对应最后一项"这个
     *     直觉在 x86_64 上恰好成立,在 riscv64 Sv39 上不成立,不能类比,
     *     必须重新按公式算。 */
    lla t0, boot_l1_low
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x1
    lla t1, boot_l2
    li t3, 2*8
    add t1, t1, t3
    sd t0, 0(t1)

    lla t0, boot_l1_high
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x1
    lla t1, boot_l2
    li t3, 258*8
    add t1, t1, t3
    sd t0, 0(t1)

    /* UART（物理 0x10000000)身份映射,只填一项叶子——boot_uart_leaf[0]
     * 覆盖 [0x10000000, 0x10001000),标志位 V|R|W（0x7),不给 X：MMIO
     * 寄存器不是代码,没有理由可执行,和下面 kernel_main.c 里
     * pagetable_map(..., PTE_FLAG_WRITABLE) 那次（同样不给 EXECUTABLE)
     * 保持一致。为什么只能是身份映射、不需要（也不能)像内核镜像那样
     * 再建一份高地址自映射：console_putc.c 里硬编码的是物理地址
     * 0x10000000 本身,不是"物理地址+KERNEL_VIRT_BASE"，程序里从来
     * 没有以高地址形式访问过 UART,不需要占用 boot_l2 顶层槽位 258 那
     * 条链路上的任何位置。
     *
     * 标志位算错过一次：Sv39 的 PTE 位布局是 D(7)/A(6)/G(5)/U(4)/X(3)/
     * W(2)/R(1)/V(0)——bit2 才是 W,bit1 是 R,不是"V|R|W 顺理成章挨着的
     * 前三位 0x3"。最初写成 0x3（=V|R,没有 W),链接、构建都不报错,实测
     * 在 QEMU 下 console_putc 第一次往 UART 寄存器写字节（THR,`sb`指令)
     * 就卡进一次 store_page_fault（epc 落在 console_putc 里的
     * `sb a4,0(a5)`,tval=0x10000000),用 `-d int,cpu_reset` 抓日志确认
     * 过：紧邻的上一条 LSR 状态位轮询（`lbu`读,同一页)完全正常,恰好印证
     * 只缺 W、不缺整条映射——如果是"完全没映射"这种更严重的错误,读也会
     * 一起失败,不会是读成功、写才失败这种精确到单一权限位的现象。 */
    la t0, 0x10000000
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x7
    lla t1, boot_uart_leaf
    sd t0, 0(t1)

    /* boot_l1_uart[128] -> boot_uart_leaf：0x10000000 的 VPN[1]=128
     * （(0x10000000>>21)&0x1ff),不是 0——UART 物理地址本身不是 2MiB
     * 对齐的整数倍窗口的第 0 个,算法和上面 boot_l2 顶层索引推导是
     * 同一套公式,这里不重复推导过程,只给结论。 */
    lla t0, boot_uart_leaf
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x1
    lla t1, boot_l1_uart
    li t3, 128*8
    add t1, t1, t3
    sd t0, 0(t1)

    /* boot_l2[0] -> boot_l1_uart：0x10000000 的 VPN[2]=0,恰好是
     * boot_l2 里真正的第 0 项——和内核镜像身份映射用的 boot_l2[2]是
     * 两个不同的顶层槽位,互不冲突,可以放心分别安装。 */
    lla t0, boot_l1_uart
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x1
    lla t1, boot_l2
    sd t0, 0(t1)

    /* 在打开分页之前调用 memmap_discover(dtb_paddr)——原因见文件顶部
     * 阶段 A 说明。memmap_discover 是 memmap.c 里的普通 C 函数,链接
     * 脚本把它放进 .text（VMA=高地址,LMA=低地址,和 boot_stage2_high
     * 反过来——boot_stage2_high 是 VMA 高、当前 PC 也真的高;这里是
     * VMA 高、但当前 PC 还是低),不能用 call/jal：-mcmodel=medany 下
     * call 由链接器松弛成 auipc+jalr,是相对*当前 PC*算的相对地址,
     * 而当前 PC（低)和 memmap_discover 的链接地址（高)之间差着整个
     * KERNEL_VIRT_BASE,既没法用±2GiB 的 auipc+jalr 编码表示,算出来
     * 也是错的目标。
     *
     * 但 memmap_discover 的*实际指令字节*在这一刻已经真实存在于低
     * 物理地址（LMA = VMA - KERNEL_VIRT_BASE,链接脚本 AT() 决定的
     * 加载地址,和运行时地址是同一件事,不需要谁去"搬")——所以只要
     * 算出这个低地址,jalr 过去就是合法调用,和 lo_target 那次的
     * "内嵌链接期常量、不经 PC 相对计算"是同一手法,只是这次要减
     * KERNEL_VIRT_BASE（riscv64 版链接脚本定义的绝对符号,汇编器/
     * 链接器能直接算),而且用 jalr ra 而不是 jr,因为这次要返回。
     *
     * a0 = dtb_paddr（此刻还在 s1,阶段 A 一开始从 a1 存过来的),调用
     * 完 memmap_discover 之后 s0/s1 仍然完好（callee-saved,C 函数
     * 不会碰),阶段 C 跳过去之后 boot_stage2_high 照常从 s0/s1 取
     * hartid/dtb_paddr 传给 kernel_boot,不受影响。 */
    mv a0, s1
    lla t0, memmap_discover_lo_target
    ld t1, 0(t0)
    jalr ra, 0(t1)

    /* 阶段 B：装 satp 并打开分页。MODE=8（Sv39,bit 63-60),PPN 是
     * boot_l2 的物理地址右移 12 位。 */
    lla t0, boot_l2
    srli t0, t0, 12
    li t1, 8
    slli t1, t1, 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma zero, zero

    /* 阶段 C：跳到高地址。.dword 内嵌的 64 位常量是链接器算出来的
     * boot_stage2_high 的*链接地址*（高 VMA),不经过任何 PC 相对
     * 计算——ld 指令把这 8 字节当数据读出来（此刻 PC 还是低地址,但
     * ld 用的是 lo_target 这个标号自己的地址做基准,lo_target 在
     * .text.boot 里,VMA=LMA=低地址,所以 auipc+ld 这两条指令算出来的
     * "lo_target 的地址"是对的,读出来的 8 字节内容才是我们真正要用的
     * 高地址绝对值),读出来的值直接扔进 jalr 的目标寄存器。 */
    lla t0, lo_target
    ld t1, 0(t0)
    jr t1

.balign 8
lo_target:
    .dword boot_stage2_high
memmap_discover_lo_target:
    .dword memmap_discover - KERNEL_VIRT_BASE
.size _start, . - _start

/* boot_stage2_high 落在普通 .text（VMA=高地址,LMA 由链接脚本的 AT()
 * 换算)——和 x86_64 版本同一个理由：这是本文件里唯一一段 VMA 是高
 * 地址但仍手写汇编的代码,执行到这里时 PC 已经真正是高地址了（阶段 C
 * 的 jr 保证了这点),所以放普通 .text 而不是 .text.boot 是安全且
 * 正确的——.text.boot 的定位（VMA=LMA=低地址)反而不适合它。 */
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
