/* Lab4 x86_64：在 Lab2/3 的基础上,把内核从"链接地址=加载地址=低地址"
 * 改造成高半区内核（higher-half kernel,内核主体链接在
 * 0xFFFFFFFF80000000,实际加载在物理 0x100000)。
 *
 * 这份文件比 Lab2/3 的 boot.S 多了两个新阶段,原有阶段基本不变：
 *
 *   阶段 A（不变,原样带过来)：_start 到 _start64 之前——32 位保护
 *     模式下清 BSS、搭一份临时页表（pml4/pdpt/pd,2MiB 大页身份映射
 *     低 1GiB)、开 PAE/长模式/分页,ljmp 进 64 位。
 *
 *   阶段 B（原有,小改)：_start64——切 段寄存器,这次先用一个专用的
 *     低地址临时栈（boot_stack,不是 __stack_top,原因见下面 _start64
 *     的注释),暂存 %ebx（Multiboot2 info 指针,做法和 Lab3 一样),
 *     然后【新增】往阶段 A 的临时页表里再插一项,让最高的 PML4 槽位
 *     （index 511,对应虚拟地址 0xFFFFFFFF80000000 起)也指向同一份
 *     pd——这样低 1GiB 身份映射和高地址内核自映射此刻*同时*生效,
 *     指向完全相同的物理页。
 *
 *   阶段 C（全新)：跳到高地址执行。用 movabs load 一个高地址标号的
 *     绝对地址,jmp 过去——这一步之后,PC 第一次落在链接器算的"真实"
 *     链接地址上,RIP 相对寻址（编译出来的 C 代码全部靠这个)才第一次
 *     生效。__stack_top 也是从这里开始才切过去。
 *
 *   阶段 D（全新,C 代码)：kernel_boot()（见 kernel_main.c)在高地址
 *     执行,这时候 CR3 还是阶段 B 那份临时表（同时有低身份映射和高
 *     自映射),用它可以安全地做 memmap_discover()（读 Multiboot2 info,
 *     物理地址是低地址,阶段 B 留的低身份映射还在,能读)。读完、
 *     kalloc 池建好之后,kernel_boot() 调 pagetable_create() +
 *     pagetable_map() 建一份*正式*页表（只有高地址自映射,不含低
 *     身份映射),pagetable_activate() 换过去——从这一刻起,低地址
 *     身份映射彻底失效（不是被显式清除某一项,是从来没被写进这份
 *     新表),这就是"跳转后撤掉低地址映射"这个决定的落地方式。
 */
.section .multiboot, "a"
.align 8
multiboot_header:
    .long 0xE85250D6
    .long 0
    .long multiboot_header_end - multiboot_header
    .long -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))
    .align 8
    .word 0
    .word 0
    .long 8
multiboot_header_end:

.section .bss.boot, "aw", @nobits
.align 4096
/* 阶段 A/B 用的临时页表。放在 .bss.boot（linker.ld 里 VMA=LMA=低地址,
 * 和 .text.boot 同一个理由,见那份文件对应位置的注释)而不是普通 .bss——
 * 下面这些结构全靠 32 位/跳转前的代码直接用 `mov $sym, %eax` 引用,
 * 编码宽度决定了目标地址必须真的很小,放进高 VMA 的 .bss 会在链接时
 * 报 relocation truncated to fit（实测确认过,不是猜测)。
 *
 * 故意不叫 pml4_high/pml4_low 这种名字——这
 * 一份表本身就同时扮演两个角色（低身份映射 + 高自映射),叫 boot_*
 * 强调"这是启动脚手架,不是 Lab4 最终产出的那份表"（最终那份由
 * pagetable_create()/pagetable_map() 在 C 代码里建,是两份完全独立
 * 的物理页)。
 *
 * boot_pdpt_high/boot_pd 后面会用到：boot_pdpt_high 是阶段 B 新插的
 * PML4[511] 指向的 PDPT,它的 entry[0] 会指向*同一份* boot_pd——不是
 * 新建一份 PD 再填一遍 2MiB 大页,是直接复用阶段 A 已经填好的那份,
 * 这样"低 1GiB 身份映射"和"高地址前 1GiB 自映射"物理上是同一批
 * PTE、同一批物理页,不会出现两份表各自维护、容易漏改一处的问题。 */
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096
boot_pd:
    .skip 4096

/* 阶段 A/B 专用的低地址临时栈,只用到阶段 C 跳转之前——不能用
 * __stack_top（linker.ld 里定义在高 VMA 的 .bss 段,阶段 B 执行时
 * 分页硬件还只认阶段 A 那份表,查表结果是"低 1GiB 身份映射"+"刚插的
 * 高地址自映射",这时候访问 __stack_top 对应的高地址理论上也能通过
 * 刚插的映射查到,但顺序上更清晰的做法是：阶段 A/B 全程只碰低地址,
 * 阶段 C 跳转那一刻才第一次切换到高地址视角,包括栈——避免在"到底
 * 现在能不能安全访问高地址"这件事上留有歧义)。128 字节足够阶段 B
 * 那几行 C 无关的纯汇编用（没有函数调用,不需要深栈)。 */
.align 16
boot_stack_bottom:
    .skip 128
boot_stack_top:

.section .text.boot
.code32

.global _start
.type _start, @function
_start:
    mov $boot_stack_top, %esp

    /* __bss_start/__bss_end 本身是高 VMA 符号（linker.ld 里定义在
     * `. += KERNEL_VIRT_BASE` 之后)——不能直接 `mov $__bss_start, %edi`,
     * 跟 boot_pml4 那批结构移进 .bss.boot 是同一个问题,但解法不同：
     * __bss_start/__bss_end 不能移出高 VMA（真正的内核 .bss 靠它们
     * 定界,后面 C 代码全靠这两个符号的高地址值算范围),只能用
     * linker.ld 里新定义的 __bss_start_phys/__bss_end_phys（减法运算
     * 挪到链接器做,原因见 linker.ld 里那两个符号定义上面的注释)——
     * 此刻分页还没打开,物理地址就是当前唯一有效的地址空间,清这段
     * 物理内存和清"将来跳转过去之后看到的高地址 .bss"清的是同一批
     * 字节。 */
    mov $__bss_start_phys, %edi
    mov $__bss_end_phys, %ecx
    sub %edi, %ecx
clear_bss:
    cmp $0, %ecx
    je bss_cleared
    movb $0, (%edi)
    inc %edi
    dec %ecx
    jmp clear_bss
bss_cleared:

    /* 阶段 A：和 Lab2/3 完全一样,建低 1GiB 身份映射（2MiB 大页)。
     * boot_pdpt_low 就是 Lab2/3 里那个"pdpt",改了名字但内容/填法
     * 一字不差——PML4[0] 指向它,它的 entry[0] 指向 boot_pd。 */
    mov $boot_pdpt_low, %eax
    or $0x3, %eax
    mov %eax, (boot_pml4)

    mov $boot_pd, %eax
    or $0x3, %eax
    mov %eax, (boot_pdpt_low)

    xor %ecx, %ecx
fill_pd:
    mov $0x200000, %eax
    mul %ecx
    or $0x83, %eax
    mov $boot_pd, %edx
    mov %eax, (%edx, %ecx, 8)
    inc %ecx
    cmp $512, %ecx
    jl fill_pd

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    mov $boot_pml4, %eax
    mov %eax, %cr3

    /* IA32_EFER（MSR 0xC0000080）：这里一次置两位。
     *   bit 8  LME (Long Mode Enable) = 0x100：进 long mode 的必要条件，
     *          Lab3 起就有。
     *   bit 11 NXE (No-Execute Enable) = 0x800：【Lab8 新增】让页表项的
     *          bit 63（NX，不可执行）真正生效。
     *
     * 为什么 Lab8 才需要 NXE —— 这是一个潜伏了四个 Lab 的真实 bug，
     * README"常见坑"会引用这段：
     *
     * pagetable.c 的 translate_flags() 从 Lab4 起就在"调用者没有传
     * PTE_FLAG_EXECUTABLE"时给页表项置上 bit 63。它旁边的注释曾经写着
     * "需要 EFER.NXE 置位才生效，否则这一位被硬件忽略"——这句话是错的，
     * 也正是 bug 的来源：NXE=0 时 bit 63 不是"被忽略"，而是一个**保留位**，
     * 而"页表项里置了保留位"是一种页错误原因（#PF 错误码的 bit 3，RSVD）。
     * 也就是说 NXE=0 时把 bit 63 置上，那一页不是"变成可执行"，而是
     * **完全不能访问**——读、写、执行全都直接 #PF。
     *
     * 为什么 Lab4~Lab7 全都没暴露：内核自映射用的是
     * PTE_FLAG_WRITABLE | PTE_FLAG_EXECUTABLE（见 kernel_main.c），
     * 带了 EXECUTABLE 所以不置 NX；用户程序页也带 EXECUTABLE。整个系统里
     * 唯一不带 EXECUTABLE 的映射是 map_user_prog() 建的**用户栈页**——
     * 而 Lab6/Lab7 的用户程序从来没有真正访问过自己的栈！它们不 push、
     * 不 pop、没有局部变量（Lab7 的 exit_code_slot 在 .data，属于程序页），
     * syscall 指令也不碰用户栈（返回地址进 rcx，不进栈）。于是这一页被
     * 映射了、但从头到尾没人碰，bug 一直没有机会发作。
     *
     * Lab8 的用户程序第一次把 read() 的缓冲区放在栈上（user_prog.S 解释
     * 了为什么必须放栈上），内核 fs_read() 里的 memcpy 第一次写向这一页，
     * bug 当场现形：#PF，CR2=0x401f80，错误码 0xa。
     *
     * 附带一课，关于怎么读错误码：0xa = RSVD(bit3) | W(bit1)，P(bit0)=0。
     * trap.c 的解码器只看 bit0/1/2，于是把它打成"not-present,write"，
     * 指向"这一页没映射"——而页其实映射得好好的。看到"明明映射了却报
     * not-present"时，一定要回头看 bit 3：它说的是"页表项本身是非法的"，
     * 跟"没有这一项"是两件完全不同的事。
     *
     * 修法选了"打开 NXE"而不是"不置 bit 63"，因为 translate_flags() 的
     * 意图本来就是 NX 语义：打开之后用户栈页真的不可执行，W^X 从一句
     * 注释变成硬件强制的事实，栈上代码执行这类攻击被真正挡住。 */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x900, %eax
    wrmsr

    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    lgdt gdt64_pointer
    ljmp $0x08, $_start64

.size _start, . - _start

.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.code64
.global _start64
.type _start64, @function
_start64:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov $boot_stack_top, %rsp

    /* %ebx 从 _start 一路存活到这里,做法和 Lab3 一样（详见那份文件
     * 里对应位置的注释——为什么是 movl 到 32 位寄存器名而不是
     * movq)。这里先挪到 %r15（call kernel_boot 之前的整段代码都不碰
     * 它,64 位调用约定里 %r15 是 callee-saved,但这里根本没有真正的
     * call/ret 配对,只是借用"这个寄存器不会被接下来几行覆盖"这个
     * 事实,不是在遵守调用约定)，阶段 C 跳过去之后再从 %r15 挪进
     * %rdi 传给 kernel_boot()。 */
    mov %ebx, %r15d

    /* 阶段 B：往阶段 A 的临时表里插一项,让 PML4[511]（虚拟地址
     * 0xFFFFFFFF80000000 起)指向 boot_pdpt_high,boot_pdpt_high 的
     * entry[0] 指向*同一份* boot_pd——低 1GiB 身份映射和高地址前
     * 1GiB 自映射,此刻共享完全相同的物理页表项。 */
    mov $boot_pdpt_high, %eax
    or $0x3, %eax
    mov %eax, (boot_pml4 + 511*8)

    /* 这里必须写 index 510，不是 0——KERNEL_VIRT_BASE=0xFFFFFFFF80000000
     * 本身的 PDPT 索引（bit 30-38）算出来是 510，不是 0：0xFFFFFFFF80000000
     * >> 30 & 0x1ff = 510。写 entry[0] 会导致 boot_pdpt_high 里真正需要
     * 的那一项（510）还是全 0/不存在,内核映像所在的高地址会直接查表
     * 缺页——实测触发过一次真实的 triple fault（PML4[511] 存在、
     * boot_pdpt_high[0] 也存在,但 CPU 翻译 0xFFFFFFFF80105fc7 这类地址
     * 时用的是 boot_pdpt_high[510],不是 [0],所以还是缺页),用 QEMU
     * monitor 读物理内存加算 VA 各级页表索引确认过,不是猜测。 */
    mov $boot_pd, %eax
    or $0x3, %eax
    mov %eax, (boot_pdpt_high + 510*8)

    /* mov %cr3, %cr3 隐式刷 TLB,让刚插的 PML4[511] 项立刻生效——
     * 严格来说这一步"重新加载同一个值"在 x86_64 上等价于刷新非
     * Global 页的 TLB 缓存,和 pagetable_activate() 里的做法（Lab4
     * 页表.c)是同一个机制,这里提前用一次,原因是接下来阶段 C 的
     * movabs+jmp 需要 PML4[511] 这一项已经生效——如果不刷,CPU 可能
     * 还在用查表缓存里"这一项不存在"的旧结果,jmp 目标就会直接缺页。 */
    mov %cr3, %rax
    mov %rax, %cr3

    /* 阶段 C：跳到高地址。movabs 装的是链接器算出来的绝对地址（Lab4
     * 链接脚本把 boot_stage2_high 放在高 VMA 的 .text 里),不是
     * RIP 相对——此刻 PC 还在低地址,任何 RIP 相对的寻址方式算出来
     * 都会是错的低地址附近的某个值,必须用装绝对立即数的 movabs。 */
    movabs $boot_stage2_high, %rax
    jmp *%rax
.size _start64, . - _start64

/* 阶段 C 的落地点：这是本文件里*唯一*一段 VMA 是高地址、但仍然手写
 * 汇编（不是编译出来的 C)的代码——之所以不干脆让上面的 jmp 直接
 * 跳到 kernel_boot()（C 函数),是因为还有两件汇编专属的收尾工作
 * （切换到真正的高地址栈、把暂存在 %r15 的 mb2 指针挪进调用约定的
 * 参数寄存器)要做,混进 C 函数开头会很别扭。
 *
 * 这段代码不在 .text.boot（VMA=LMA=低地址),在普通的 .text（VMA=高
 * 地址,LMA 由链接脚本的 AT() 换算到低地址)——链接器算这段代码内部
 * 任何标号引用、任何跳转目标时,用的都是高地址,这正是我们想要的：
 * 执行到这里时 PC 已经真正是高地址了（阶段 C 的 jmp 保证了这点),
 * 高地址标号在这时候引用自己完全一致。 */
.section .text
.code64
.global boot_stage2_high
.type boot_stage2_high, @function
boot_stage2_high:
    mov $__stack_top, %rsp
    mov %r15d, %edi
    call kernel_boot

1:  hlt
    jmp 1b
.size boot_stage2_high, . - boot_stage2_high
