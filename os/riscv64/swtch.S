/* Lab7 riscv64：swtch(old, new) —— 内核执行流之间的上下文切换。跟
 * x86_64 版本（../x86_64/swtch.S）同一个 xv6 经典设计，这里只写 riscv64
 * 寄存器集合上的具体差异，核心原理（"切换执行流只需要保存 callee-saved
 * 寄存器 + 一个返回地址，caller-saved 寄存器由调用约定本身保证不需要
 * swtch() 越权处理"）跟那份文件顶部模块注释完全一致，不重复展开。
 *
 * 签名：void swtch(struct context **old, struct context *new)
 *   a0 = old（二级指针）
 *   a1 = new
 * 跟 x86_64 版本用 rdi/rsi 传这两个参数是同一个"调用约定的头两个整数
 * 参数寄存器"角色，只是 riscv64 用 a0/a1。
 *
 * callee-saved 集合：riscv-abi 规范"Register Convention"表格里 ra 和
 * s0-s11 都标"Saved register"（跟 x86_64 的 rbx/rbp/r12-r15 五个比,
 * riscv64 这里是 13 个——ra 本身也算在内,因为 riscv64 没有 x86_64 call
 * 指令那种"硬件自动把返回地址压栈"的行为,call/jalr 是把返回地址放进
 * ra 寄存器,不是内存,swtch() 要把 ra 也一起 sd/ld，跟 x86_64 版本注释
 * 里"call 已经把返回地址压到栈上了，不需要手动保存"正好是相反的具体
 * 做法——但达到的效果一样：都是把"下次切回这里应该跳到哪"这份信息
 * 保存进 struct context）。
 */
.section .text

.global swtch
swtch:
    /* 保存当前现场：先在栈上开出 13 个 uint64_t（104 字节）的空间，
     * sd 顺序跟 struct context 声明顺序一致（ra/s0-s11），方便两侧
     * 对照阅读——跟 x86_64 版本一样，这里的 sd 顺序不需要严格对应
     * struct context 的字段顺序（只要 sd 和下面 new 现场的 ld 内部
     * 自洽即可），选择一致只是为了可读性。 */
    addi sp, sp, -104
    sd ra,   0(sp)
    sd s0,   8(sp)
    sd s1,  16(sp)
    sd s2,  24(sp)
    sd s3,  32(sp)
    sd s4,  40(sp)
    sd s5,  48(sp)
    sd s6,  56(sp)
    sd s7,  64(sp)
    sd s8,  72(sp)
    sd s9,  80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)

    /* 此刻 sp 就是"当前现场"的起点，把它存进 *old（a0 = &old 变量本身，
     * 对应 struct context * 类型)，供将来 swtch 回这里时读取——跟
     * x86_64 版本 `mov %rsp, (%rdi)` 是同一个动作。 */
    sd sp, (a0)

    /* 换栈：把 sp 换成目标现场的栈顶（a1 = new，这个指针本身的数值就是
     * 目标现场保存时的栈顶地址——proc_alloc_skeleton() 第一次创建这个
     * 现场时也是按同样的布局手工摆好这 13 个字段+一个 trap_return 地址，
     * 见 proc.c，跟 x86_64 版本同一个道理）。 */
    mv sp, a1

    ld ra,   0(sp)
    ld s0,   8(sp)
    ld s1,  16(sp)
    ld s2,  24(sp)
    ld s3,  32(sp)
    ld s4,  40(sp)
    ld s5,  48(sp)
    ld s6,  56(sp)
    ld s7,  64(sp)
    ld s8,  72(sp)
    ld s9,  80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    addi sp, sp, 104

    /* ret（riscv64 里是 jalr x0, 0(ra) 的伪指令，等价于"跳到 ra，不再
     * 保存返回地址"）跳到刚 ld 出来的 ra——这个地址是目标现场当初（上一
     * 次被换出，或者 proc_alloc_skeleton() 初次构造）留在这个位置的
     * "下一步该去哪"，跟 x86_64 版本 ret 从栈顶弹出地址是同一件事，只是
     * riscv64 的"返回地址"活在寄存器 ra 里，不是栈上的隐式一格——这也是
     * 为什么上面必须显式 sd/ld ra（x86_64 的 call/ret 全程都在操作栈上
     * 那一格，swtch() 不用管；riscv64 的 call/ret 全程操作 ra 寄存器，
     * swtch() 必须像对待 s0-s11 一样对待它）。 */
    ret

/* trap_return：proc_alloc_skeleton() 把每个新进程 context.ra 都设成这个
 * 地址，swtch() 第一次切进一个新进程时，上面的 `ld ra, 0(sp)` 读到的
 * 正是这个地址，`ret` 跳过来——"启动一个从未运行过的新进程"和"resume
 * 一个 trap 处理完毕、正常走到这里的旧进程"落进同一段代码,跟 x86_64
 * 版本 trap_return 同一个 xv6 经典手法,proc.h 顶部模块注释已经展开过
 * 这个设计,这里不重复。
 *
 * 进来的时候 sp 已经是"这个进程自己的内核栈"（swtch() 换栈换的就是
 * 这个），且这个栈上紧跟在 context 13 个字段被 ld 完之后的位置，正是
 * proc_alloc_skeleton() 摆好的 struct trapframe——两者紧邻，trap_return
 * 不需要任何寄存器传参就能找到自己的 trapframe，直接理解成"栈顶再往上
 * 一点就是 trapframe 的起始地址"，跟 x86_64 版本同一个布局关系。
 *
 * 下面这段 ld 序列必须跟 struct trapframe（proc.h）的字段顺序逐一对应
 * ——跟 trap_entry.S 里 supervisor_trap_entry 的 sd 顺序也必须一致，
 * 三者描述的是同一种栈帧形状。不能复用 supervisor_trap_entry 内部那段
 * ld（无法复用：那段紧跟着 sret，中间没有导出成一个独立符号，且本 Lab
 * 刻意不改 trap_entry.S 里 S-mode 来源那条路径的既有形状，避免影响
 * Lab4/5/6 已经验证过的 page fault/定时器路径）——这跟 x86_64 版本
 * trap_return 无法复用 timer_stub/page_fault_stub 内部弹栈段是同一个
 * 理由。
 *
 * 最后两个字段（sepc/sstatus）不能用 ld 直接进通用寄存器再指望它们
 * "自动生效"——riscv64 没有 iretq 那种"一条指令把栈上一串字段当成
 * PC/flags/segment 自动加载"的硬件行为（trap_entry.S 顶部模块注释、
 * enter_user_mode 那段注释已经反复说明这一点：sret 不管通用寄存器,也
 * 不会自己去读栈,它只读当前 CSR 的值),必须先 ld 进一个临时通用寄存器，
 * 再 csrw 显式写进 sepc/sstatus 这两个 CSR，最后 sret 才会真正用上
 * 它们——这是 riscv64 相对 x86_64 iretq 在"恢复现场"这一步必须多做的
 * 两步，对应 x86_64 版本注释里"RIP/CS/RFLAGS/RSP/SS 留在栈上,形状正好
 * 是 iretq 期望的样子"这一整句在 riscv64 上不成立,riscv64 没有等价的
 * "栈帧形状换来硬件自动处理"这条捷径。 */
.global trap_return
trap_return:
    /* 恢复顺序刻意跟 struct trapframe 的字段声明顺序不一致：CSR
     * （sepc/sstatus，位于 trapframe 最后两个字段）必须最先恢复，
     * 因为写它们需要借用 t0/t1 当搬运暂存——如果先把 t0/t1 的"真实
     * 值"（属于被打断进程自己的 t0/t1）恢复回去，这两个寄存器就不能
     * 再被 trap_return 自己拿来搬运 CSR 值了。x86_64 版本没有这个
     * 顾虑：iretq 直接从栈上读 RIP/CS/RFLAGS/RSP/SS，不需要经过任何
     * 通用寄存器中转，这是 riscv64 sret 相对 iretq 缺的那层硬件能力
     * （trap_entry.S 顶部注释、上面 trap_return 说明块已经提到）。
     *
     * sp/gp/tp 三个"这条指令一执行就会让当前取址/取栈方式变掉"的
     * 寄存器，同理必须放最后——一旦 ld sp 之后，当前栈上剩余待恢复的
     * 字段就再也找不到了（栈指针已经指向用户栈，不再是内核栈上的
     * trapframe）；gp 在 riscv64 有些环境下参与"全局指针相对寻址"，
     * tp 参与线程本地寻址，这两个本 Lab 未使用，但恢复顺序仍然把它们
     * 放最后，跟 sp 同一类"改变寻址基准"的寄存器一并处理，更安全。 */
    ld t0, 248(sp)   /* trapframe.sepc */
    ld t1, 256(sp)   /* trapframe.sstatus */
    csrw sepc, t0
    csrw sstatus, t1

    ld ra,  0(sp)
    ld t0,  32(sp)
    ld t1,  40(sp)
    ld t2,  48(sp)
    ld t3,  56(sp)
    ld t4,  64(sp)
    ld t5,  72(sp)
    ld t6,  80(sp)
    ld s0,  88(sp)
    ld s1,  96(sp)
    ld s2, 104(sp)
    ld s3, 112(sp)
    ld s4, 120(sp)
    ld s5, 128(sp)
    ld s6, 136(sp)
    ld s7, 144(sp)
    ld s8, 152(sp)
    ld s9, 160(sp)
    ld s10,168(sp)
    ld s11,176(sp)
    ld a0, 184(sp)
    ld a1, 192(sp)
    ld a2, 200(sp)
    ld a3, 208(sp)
    ld a4, 216(sp)
    ld a5, 224(sp)
    ld a6, 232(sp)
    ld a7, 240(sp)

    /* sp/gp/tp 放最后：一旦 sp 被改写，trapframe 剩余部分（此刻已经
     * 没有剩余，但保持"改变寻址基准的寄存器最后处理"这条规则本身比
     * 记住"这次恰好没有剩余"更不容易在以后改动 trapframe 布局时踩坑）
     * 就不能再通过当前 sp 相对偏移访问了，所以 gp/tp 必须先于 sp 从
     * "老 sp 相对偏移"读出来。 */
    ld gp,   16(sp)
    ld tp,   24(sp)
    ld sp,    8(sp)

    /* sret：把当前特权级切到 sstatus.SPP 指定的级别（这个进程是普通
     * 用户进程，proc_alloc_skeleton() 构造 trapframe 时 SPP 位清零，
     * 见 proc.c，所以这里落到 U-mode），PC 跳到 sepc（刚 csrw 写好的
     * 用户态入口或者断点地址）。跟 enter_user_mode 里那条 sret 是同一
     * 条指令、同一个硬件行为——这也是为什么本次改写之后 enter_user_mode
     * 整个函数不再需要：它原来手工做的"设 sepc/清 SPP/设 SPIE/设 SUM/
     * 换 sp/sret"这一整套，现在等价于"提前在 trapframe 里摆好这些值，
     * 然后走到这里"，两条路径最终执行的都是这同一条 sret。 */
    sret
