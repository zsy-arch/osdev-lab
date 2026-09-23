/* Lab6 riscv64：在 Lab5"单一 stvec 入口,不切栈"的基础上,第一次让
 * trap 可能从 U-mode 发起（ecall）——这打破了 Lab5 注释里写的前提
 * （"本课程目前只有内核态代码,sp 本来就已经是合法的内核栈"）,必须
 * 补一个栈切换机制,跟 x86_64 版本（../x86_64/trap_entry.S）
 * syscall_entry 的 syscall_saved_user_rsp 是同一个问题的同一种解法,
 * 只是触发方式不同——x86_64 的 SYSCALL 完全不经过 IDT,天然需要独立的
 * syscall_entry 入口；riscv64 的 ecall 和 page fault/定时器共享同一个
 * stvec 入口（Direct 模式的本质——所有 trap 只有一个跳转目标）,不能
 * 像 x86_64 那样"写一个新入口只处理 ecall",必须在*唯一的*
 * supervisor_trap_entry 里,用一条分支区分"这次 trap 从哪个特权级来的",
 * 分别决定要不要切栈。
 *
 * 怎么判断来源特权级、且*在还没有任何栈可用之前*就能做到：sstatus
 * 的 SPP 位（bit 8，riscv-privileged 规范"sstatus"一节——硬件规定的
 * 行为，不是本课程自定义：trap 发生时，硬件自动把 SPP 设成 0（来自
 * U-mode）或 1（来自 S-mode，本课程目前只有 page fault/定时器这两条
 * 路径，从 Lab4/5 起从未来自 U-mode，sret 返回时还会读它决定回哪个
 * 特权级、然后清零——这里只用它的"读"这一面，sret 那一步的语义不需要
 * 本文件操心）——csrr 是纯读 CSR 的指令，不依赖栈，可以在决定"要不要
 * 切栈"之前安全执行。
 *
 * SPP=0（来自 U-mode，本 Lab 新增的 ecall 路径）：sp 此刻是*用户栈*
 * 指针，不能在上面 addi sp,sp,-128——跟 x86_64 侧 syscall_entry 注释
 * 里"用户栈的内容对内核不可信，且本 Lab 用户栈空间本来就很小"完全
 * 一样的顾虑。做法也完全对应：先把当前（用户）sp 存进一个全局变量
 * （trap_saved_user_sp，对应 x86_64 的 syscall_saved_user_rsp，同样
 * 用全局变量不用压栈——这一刻还没有可信的栈可以压），再换上
 * __stack_top（跟 x86_64 复用 Lab1 就有的同一个内核栈是同一个简化，
 * 理由见 trap.c/kernel_main.c 对应注释）。
 *
 * SPP=1（来自 S-mode，page fault/定时器，Lab4/5 原有路径）：sp 已经
 * 是合法内核栈，跟 Lab5 完全一样，不做任何切换——下面两个分支在
 * "保存寄存器->call->恢复寄存器"这一段用的是完全相同的一份代码
 * （no fallthrough duplication，两条路径汇合到同一段 body），保证
 * S-mode 来源时的指令序列跟 Lab5 逐字节等价，不会因为新增 U-mode
 * 分支而意外改变这条已经验证过的路径。
 *
 * 出口对称：t1 保存了入口那次 SPP 判断结果，跨越 supervisor_trap_
 * handler 调用——t1 属于 caller-saved（riscv 调用约定），普通 C 函数
 * 完全可能踩它，所以 t1 本身也要跟 t0-t6 一样存进栈帧，call 返回后
 * 从栈帧里读回来，不能指望它在 call 前后保持不变。只有这个值非零
 * （来源 U-mode）才把 sp 换回 trap_saved_user_sp；否则 sp 全程没变过，
 * 不需要恢复。 */
.section .text
.global supervisor_trap_entry
.align 4

#define SSTATUS_SPP (1 << 8)

supervisor_trap_entry:
    /* 读 SPP，此刻还完全不能碰栈——t0/t1 是唯一在"决定要不要切栈"这
     * 一步之前就需要用到的寄存器，两条分支汇合之后会把它们跟其它
     * t0-t6 一视同仁地存进栈帧，不会有任何寄存器被跳过保存。 */
    csrr t1, sstatus
    andi t1, t1, SSTATUS_SPP
    bnez t1, from_supervisor

from_user:
    /* SPP=0：sp 是用户栈指针，先存进全局变量（不压栈，理由见上面
     * 模块注释），再换上内核栈——顺序不能颠倒：如果先换栈，原来的
     * 用户 sp 值就丢了，没有第二个地方能找回它。 */
    la t0, trap_saved_user_sp
    sd sp, 0(t0)
    la sp, __stack_top

from_supervisor:
    addi sp, sp, -128
    sd ra,  0(sp)
    sd t0,  8(sp)
    sd t1,  16(sp)
    sd t2,  24(sp)
    sd t3,  32(sp)
    sd t4,  40(sp)
    sd t5,  48(sp)
    sd t6,  56(sp)
    sd a0,  64(sp)
    sd a1,  72(sp)
    sd a2,  80(sp)
    sd a3,  88(sp)
    sd a4,  96(sp)
    sd a5,  104(sp)
    sd a6,  112(sp)
    sd a7,  120(sp)

    /* supervisor_trap_handler(uint64_t *frame)：跟 Lab4/5 的零参数
     * 签名不同（见 trap.c ecall_handler 上方注释里"折中方案"那段的
     * 完整解释)——这里把当前 sp（栈帧基址,刚保存完的 16 个寄存器就
     * 从这里开始)搬进 a0,C 函数侧按 frame[N] 下标访问,不依赖任何
     * 具体寄存器名,只依赖"sd 的顺序"这一份契约,跟 trap.c 里的下标
     * 注释一一对应。mv 而不是 sd/直接传：a0 本身也已经被上面 `sd a0,
     * 64(sp)` 存进了栈帧,这里的 mv 只是把 sp 的值*读*到 a0 寄存器里
     * 当函数参数传走,不会覆盖栈帧里刚保存的 a0 副本（栈帧和寄存器是
     * 两个独立的存储位置),call 返回之后下面 `ld a0, 64(sp)` 会把
     * a0 寄存器还原成用户程序真正的 a0（如果是 ecall,这个槽位可能
     * 已经被 ecall_handler 改写成返回值,取决于 trap.c 那边的行为,
     * 这里只负责搬运,不关心内容)。 */
    mv a0, sp
    call supervisor_trap_handler

    ld ra,  0(sp)
    ld t0,  8(sp)
    ld t1,  16(sp)
    ld t2,  24(sp)
    ld t3,  32(sp)
    ld t4,  40(sp)
    ld t5,  48(sp)
    ld t6,  56(sp)
    ld a0,  64(sp)
    ld a1,  72(sp)
    ld a2,  80(sp)
    ld a3,  88(sp)
    ld a4,  96(sp)
    ld a5,  104(sp)
    ld a6,  112(sp)
    ld a7,  120(sp)
    addi sp, sp, 128

    /* t1 是刚从栈帧恢复的值（上面 `ld t1, 16(sp)`），跟入口保存进去的
     * 是同一个 SPP 判断结果——如果 supervisor_trap_handler 内部改了
     * sstatus（本 Lab 不会，但不依赖这个假设，直接用保存下来的副本
     * 更稳妥），出口分支判断依据仍然是"这次 trap 刚进来的时候来自
     * 哪个特权级"，不是"处理完之后 sstatus 变成了什么"。 */
    bnez t1, trap_return

    la t0, trap_saved_user_sp
    ld sp, 0(t0)

trap_return:
    sret

/* enter_user_mode(entry, stack)：第一次从 S-mode 跳进 U-mode。跟
 * x86_64 版本（../x86_64/trap_entry.S 的 enter_user_mode)形式上像
 * （"手工构造一次特权级下降",不是复用 ecall/sret 那条"回到刚才那次
 * ecall 调用点"的路径),但背后的原因不同,值得对比着读：
 *
 * x86_64 不能用 sysretq 做第一次切换,是因为 sysretq 读的 rcx/r11
 * 必须来自*之前一次 SYSCALL 硬件自动填入*的值,这里从未发生过
 * SYSCALL,没有合法来源（syscall_entry 注释已经展开过)。riscv64 的
 * sret 完全不是这么回事——sret 读的 sepc/sstatus.SPP/sstatus.SPIE
 * 全部是*软件可写*的 CSR（riscv-privileged 规范定义的行为,不依赖
 * "之前发生过一次 trap"这个前提),没有"必须先有一次 trap 才能
 * sret"的硬件限制。这里能够直接手写 sepc/sstatus 然后 sret,不需要
 * 像 x86_64 那样退而求其次去借用 IDT 路径的 iretq——riscv64 在
 * "凑一次特权级切换"这件事上,第一次和第 N 次用的是*同一条*
 * 指令（sret),不存在 x86_64 那种"两条不同指令分别对应第一次/第 N
 * 次"的区分。这是本 Lab README 会点出的"riscv64 sret vs x86_64
 * sysretq"关键差异之一。
 *
 * sret 不会自动设置 sp（跟 iretq 不一样——iretq 从栈帧里弹出的五个
 * 值包含 RSP,是硬件行为的一部分;sret 只处理 pc/特权级/中断使能这
 * 三件事,riscv-privileged 规范里 sret 的操作性描述完全没有提到任何
 * 通用寄存器),必须在执行 sret 之前,用普通的 mv 把 sp 设成用户栈顶
 * ——这跟 trap_entry.S 出口分支需要手动 ld 恢复 sp 是同一个"sret 不
 * 管通用寄存器"这条规则的另一个体现。 */
.global enter_user_mode
enter_user_mode:
    /* riscv 调用约定：entry=a0,stack=a1——先把 sstatus/sepc 设好,
     * 最后才动 sp（顺序上没有 x86_64 那种"push 会改变 rsp,必须先
     * 把参数存到别的寄存器"的顾虑,因为这里全程只用 csrw/mv,不涉及
     * 压栈),但仍然把"设 sp"放在最后一步,理由是：一旦 sp 变成用户
     * 栈顶,当前内核栈上的局部状态（这个函数本身没有任何局部变量,
     * 但保持"尽量晚才切走当前有效栈"是更谨慎的顺序,不依赖"这个
     * 函数刚好不需要栈"这个事实继续裸奔)不应该再被依赖。 */
    csrw sepc, a0

    /* sstatus：需要管三个位——
     *   SPP（bit 8)清零：sret 读这一位决定回哪个特权级,0=U-mode。
     *     riscv-privileged 规范"sstatus"一节明确写了这条语义,跟
     *     trap_entry.S 里读 SPP 判断"trap 从哪来"是同一个字段的
     *     两个不同使用场景（trap 发生时硬件写它,这里 sret 之前
     *     软件写它)。
     *   SPIE（bit 5)置 1：sret 时 sstatus.SIE 会被设成 SPIE 的值
     *     （riscv-privileged 规范"sstatus"一节),即"回到 U-mode
     *     之后,全局中断使能是什么状态"由 SPIE 决定,不是 SPP。
     *     这里置 1 是因为用户程序需要能被定时器中断打断（跟
     *     x86_64 版本 enter_user_mode 里 RFLAGS.IF=1 的用户态
     *     可中断性是同一个教学取向,只是 riscv64 这边通过 SPIE
     *     这个"影子位"而不是直接写 SIE 本身来实现)。
     *   SUM（bit 18,"permit Supervisor User Memory access")置 1：
     *     riscv64 版本 SMAP 的等价机制——S-mode 默认不能读/写带 U
     *     位的页,哪怕当前 satp 已经把这页映射好、S-mode 自己也有
     *     权限访问其它非 U 页,这是一条独立的、只针对"U 位页"的
     *     禁止,复位值是 0（禁止)。x86_64 那边没有这层限制的对应
     *     物是因为本课程从没启用 CR4.SMAP,SMAP 在 x86_64 上是
     *     opt-in 的,不开就没有这个问题;riscv64 这边 SUM=0 是
     *     *默认状态*,不是"课程选择不开",是 QEMU/OpenSBI 都不会
     *     替内核设置这一位,内核必须自己显式打开。实测触发：没加
     *     这一位时,trap.c 的 sys_write() 从 S-mode 直接解引用
     *     用户传进来的指针（frame[8]/a0,即 msg 的用户虚拟地址)
     *     触发 load page fault（scause=13,addr=用户 msg 的地址),
     *     即便 pagetable.c 的 PTE 上 U 位、R 位都已经正确设置——
     *     故障原因不是页表项本身不对,是 sstatus.SUM 这个额外的
     *     "S-mode 要不要认这个 U 页"开关没打开,PMP/页表权限检查
     *     和 SUM 检查是两层独立的门,页表那层过了不代表 SUM 那层
     *     也过了。riscv-privileged 规范原文："permit Supervisor
     *     User Memory access"——名字已经把语义说得很直接。
     * 读-改-写而不是直接 csrw 一个常量：sstatus 里还有别的位
     * （比如 riscv64 的 SXL/UXL,虽然本课程不主动依赖它们的具体值,
     * 但不应该在没有理由的情况下把它们清零),只改需要改的三个位,
     * 不覆盖其它状态——跟 kalloc.c 那次"读-改-写不覆盖已有状态"
     * 是同一个工程习惯,不是本文件独有的规则。 */
    csrr t0, sstatus
    li t1, (1 << 8) /* SPP */
    not t1, t1
    and t0, t0, t1  /* SPP = 0 */
    li t1, (1 << 5) /* SPIE */
    or t0, t0, t1   /* SPIE = 1 */
    li t1, (1 << 18) /* SUM */
    or t0, t0, t1    /* SUM = 1 */
    csrw sstatus, t0

    mv sp, a1
    sret
