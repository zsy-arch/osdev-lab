/* Lab7 x86_64：swtch(old, new) —— 内核执行流之间的上下文切换。
 *
 * 这是本 Lab 全新引入的机制，Lab1-6 从未需要过它：之前每个 Lab 全程
 * 只有*一条*内核执行流（"内核主循环 + 偶尔陷入的用户程序"，trap.c 的
 * 注释反复强调过这一点），"切换到另一条内核执行流"这个操作没有出现
 * 过的理由。本 Lab 引入多进程之后，调度器需要在"进程 A 的内核栈"和
 * "进程 B 的内核栈"之间跳转——这正是 swtch() 要做的事,也是"上下文
 * 切换"这个词在操作系统课里的标准含义。
 *
 * 签名：void swtch(struct context **old, struct context *new)
 *   rdi = old（一个二级指针——*old 是"调用者这次要把现场存到哪里"，
 *         这个位置的内容会被 swtch() 写入，调用者稍后 swtch 回来时
 *         会从同一个位置读出）
 *   rsi = new（调用者想要切换过去的目标现场）
 *
 * 核心原理（xv6 教学内核的经典手法，本课程直接采用同一个设计）：
 * "切换执行流"不需要保存/恢复全部寄存器，只需要保存 callee-saved
 * 寄存器（System V AMD64 调用约定规定 rbx/rbp/r12-r15 是 callee 必须
 * 保留的，caller-saved 的 rax/rcx/rdx/rsi/rdi/r8-r11 在函数调用边界上
 * 本来就不保证存活，不需要 swtch() 越权处理）+ 一个"返回地址"。这个
 * "返回地址"就是 call 指令本身已经隐式压到栈上的那个值——swtch() 不
 * 需要手动保存 rip，因为它自己也是被 call 进来的，rip 已经在栈上；
 * 真正需要做的是：把*当前*栈上那个隐式返回地址挪到 struct context
 * 里保存（作为"下次切回这里时应该跳到哪"的记录），再把 rsp 换成
 * *目标*现场事先准备好的栈，最后 ret——ret 会从新栈顶弹出一个地址当
 * 作跳转目标，而这个地址正是目标进程上次被换出时保存的 context.rip
 * （或者是 proc_alloc() 初始化时手工填的 trap_return，见 proc.c）。
 *
 * 这就是为什么 swtch() 看起来"进去一次，出来在另一个地方"——它的
 * ret 跳到的不是它自己的调用点，是*目标*现场当初存的调用点，这正是
 * "切换执行流"这个概念在汇编层面最朴素的样子：栈本身就是执行流的
 * 身份，换栈就是换执行流。
 */
.code64

.global swtch
swtch:
    /* 保存当前现场：先把要保存的寄存器压栈（顺序无所谓，只要恢复时
     * 反过来弹即可），再把这段栈顶地址存进 *rdi（即 *old）。这里的
     * push 顺序和 struct context 声明的字段顺序不需要严格对应——
     * struct context 的字段顺序是给"下面 pop 目标现场时怎么读"用的，
     * 保存这一侧只要保证"push 什么、之后精确地 pop 什么"内部自洽即可，
     * 但为了两侧读起来对称，这里选择跟 struct context 声明顺序一致
     * （rbx/rbp/r12/r13/r14/r15）。 */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /* 此刻栈顶（%rsp）就是"当前现场"的起点——往上（更高地址）依次是
     * r15/r14/r13/r12/rbp/rbx，再往上是 call swtch 时压入的返回地址，
     * 正好对应 struct context 的字段布局（rbx..r15 后面紧跟 rip）。
     * 把这个地址存进 *old，供将来 swtch 回这里时读取。 */
    mov %rsp, (%rdi)

    /* 换栈：把 rsp 换成目标现场的栈顶（rsi = new，即 struct context *，
     * 这个指针本身的数值就是"目标现场保存时的栈顶地址"——proc_alloc()
     * 第一次创建这个现场时，也是按同样的布局手工在 kstack 上摆好这几个
     * 字段+一个 trap_return 地址,让"第一次被 swtch 进来"和"之后每次
     * 被 swtch 进来"走的是完全相同的恢复路径,不需要为"第一次"写特殊
     * 情况)。 */
    mov %rsi, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    /* ret 从新栈顶弹出一个地址并跳过去——这个地址是目标现场当初
     * （无论是上一次被换出，还是 proc_alloc() 初次构造）留在这个位置
     * 的"下一步该去哪"，swtch() 自己完全不知道、也不需要知道这个地址
     * 具体是什么,这正是这个函数能够同时服务"切到另一个已经跑过的进程"
     * 和"第一次启动一个新进程"两种场景的原因——两种场景在 swtch()
     * 看来是同一件事：栈顶有一个地址,跳过去。 */
    ret

/* trap_return：proc_alloc() 把每个新进程 context.rip 都设成这个地址，
 * swtch() 第一次切进一个新进程时，ret 弹出的正是这个地址，于是"启动
 * 一个从未运行过的新进程"和"resume 一个 trap 处理完毕、正常走到这里
 * 的旧进程"落进同一段代码——这正是本文件顶部模块注释、proc.h 顶部
 * 模块注释都强调的 xv6 经典手法："resume a process" 统一等价于"从
 * trapframe 恢复现场再 iretq"，不需要为"这是第一次运行还是第 N 次
 * 恢复运行"写任何分支。
 *
 * 进来的时候 %rsp 已经是"这个进程自己的内核栈"（swtch() 换栈换的就是
 * 这个），且这个栈上紧跟在 context 6 个 callee-saved 寄存器之下（也就
 * 是 swtch() 里那 6 次 pop 执行完之后）的位置，正是 proc_alloc() 摆好
 * 的 struct trapframe——proc.c 里 proc_alloc() 会保证 kstack 的布局是
 * [低地址...trapframe...context...高地址/栈顶]，两者紧邻，trap_return
 * 不需要任何寄存器传参就能找到自己的 trapframe：它就在当前 %rsp 正上方
 * （直接理解成"栈顶再往上一点就是 trapframe 的起始地址"）。
 *
 * 下面这段 pop 序列必须跟 trap_entry.S 里 page_fault_stub/timer_stub
 * 的 pop 序列逐字节相同——三者描述的是*同一种*栈帧形状（push 顺序
 * 是 r15 最先、rax 最后，pop 自然反过来，rax 最先、r15 最后），
 * trap_return 只是在"调度器恢复一个进程"这个新增场景下把同一套弹栈
 * 逻辑重新写一遍，而不是复用 timer_stub 内部那段（无法复用：
 * timer_stub 的弹栈段紧跟着 iretq，中间没有导出成一个独立符号，且
 * 本 Lab 刻意不改 Lab6 遗留的 trap_entry.S 弹栈段来避免影响已经验证
 * 过的陷入路径）。 */
.global trap_return
trap_return:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    /* 剩下的五个字段（RIP/CS/RFLAGS/RSP/SS）留在栈上，形状正好是
     * iretq 期望的样子（跟 enter_user_mode 手工搭的那份栈帧、以及
     * page_fault_stub/timer_stub 硬件自动压的那份栈帧，三者字段顺序
     * 完全一致——这正是本 Lab 反复强调的"trap 现场只有一种形状"）。 */
    iretq
