/* Lab6 x86_64：page_fault_stub/timer_stub 原样保留自 Lab5（SYSCALL 是
 * 独立于 IDT 的第三条陷入路径，不影响前两条，见 trap.c 顶部注释）。
 * 新增的是 syscall_entry——SYSCALL 指令触发时 CPU 直接跳到 IA32_LSTAR
 * 里存的这个地址，不经过 IDT，也不像 IDT 中断门那样自动压栈保存现场：
 * 硬件只做了这几件事（AMD64 手册 SYSCALL 一节明确写出来的，不是本课程
 * 猜测）——
 *   RCX  = 用户态 RIP（SYSRET 用它恢复）
 *   R11  = 用户态 RFLAGS（SYSRET 用它恢复）
 *   RFLAGS = RFLAGS AND NOT(IA32_FMASK)（trap.c 里配置成只清 IF）
 *   CS/SS = 按 IA32_STAR[47:32] 算出的内核段选择子
 *   RIP  = IA32_LSTAR
 * %rsp 完全不变（还是用户栈指针）——这是 SYSCALL 跟 IDT 陷入路径最大的
 * 区别，必须手动换栈，见下面的注释。
 */
.code64

.extern syscall_dispatch
.extern syscall_saved_user_rsp
.extern syscall_kernel_rsp
.extern page_fault_handler
.extern timer_interrupt_handler
.global page_fault_stub
.global timer_stub
.global syscall_entry
.global enter_user_mode

page_fault_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov 120(%rsp), %rdi
    call page_fault_handler

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

    add $8, %rsp
    iretq

timer_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    call timer_interrupt_handler

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

    iretq

/* syscall_entry：进来的时候 %rsp 还是用户栈，%rcx=用户RIP，%r11=用户
 * RFLAGS，%rax=系统调用号，%rdi/%rsi=参数（用户程序按这个约定填好，见
 * user_prog.S）——这四个约定寄存器（rax 号+rdi/rsi/rdx/r10/r8/r9 六个
 * 参数位，本 Lab 只用到前两个）都不能在换栈之前被破坏，换栈本身（mov
 * %rsp 到临时变量、再换成内核栈）不涉及这些寄存器，天然安全。
 *
 * 换栈用一个全局变量（syscall_saved_user_rsp）暂存用户 RSP，不是压栈
 * ——这一刻 %rsp 还是用户栈指针，如果先 push 保存内核栈上的东西，会把
 * 数据压到用户栈里（用户栈的内容对内核不可信，且本 Lab 的用户程序栈
 * 空间本来就很小，教学取向不依赖"用户栈还有余量能借用几个字节"这个
 * 假设）；反过来，全局变量在切栈动作发生之前就能被写入，不依赖任何
 * 栈的存在。 */
syscall_entry:
    mov %rsp, syscall_saved_user_rsp(%rip)
    mov syscall_kernel_rsp(%rip), %rsp

    /* rcx（用户 RIP）/r11（用户 RFLAGS）是 SYSRET 唯一会读的两个
     * 寄存器，syscall_dispatch 是一个普通 C 函数，System V AMD64
     * 调用约定里 rcx 是 callee-clobbered（第四个整数参数寄存器）——
     * 如果不在调用前保存，call 指令本身（不需要 rcx）大概率不会碰它，
     * 但 syscall_dispatch 函数体内部完全可能把 rcx 用作临时寄存器，
     * 保存下来是唯一不依赖"这个函数恰好没用到 rcx"这种脆弱假设的
     * 做法。r11 同理是 callee-clobbered（调用约定里的易失寄存器）。 */
    push %rcx
    push %r11

    /* Lab7 新增第 4/5 个参数：user_rip_slot（这次 SYSCALL 恢复现场用的
     * rcx，此刻已经 push 在栈上，见上面两行）的地址，以及 user_rsp_slot
     * （syscall_saved_user_rsp 这个全局变量本身)的地址——sys_fork/
     * sys_exec 需要读（有时还需要写)这两个值，理由见 proc.h 里 sys_
     * fork/sys_exec 声明处的大注释：SYSCALL 路径完全不经过 proc.h 的
     * struct trapframe（那是 IDT 路径——timer_stub/page_fault_stub——
     * 专用的现场格式，两条陷入路径在本 Lab 里是分开的，见 trap.c 顶部
     * 模块注释"SYSCALL 是独立于 IDT 的第三条陷入路径"）。
     *
     * 两个都传地址，不传值：sys_exec() 需要能*修改*"这次 syscall 返回
     * 后会恢复到哪个 RIP/RSP"，如果只传值,sys_exec() 改了也没用（改的
     * 是它自己函数调用栈上的一份拷贝),必须能通过指针写回这两个位置
     * 本身——下面 pop %rcx / mov syscall_saved_user_rsp(%rip), %rsp
     * 才会读到 sys_exec() 写进去的新值。sys_fork() 不需要写它们（子
     * 进程走的是完全独立的另一份 trapframe，父进程自己这次 syscall
     * 正常返回,两个值都不用改),但统一成同一种参数类型更简单,不需要
     * 给 sys_fork/sys_exec 定义两种不同的签名。
     *
     * lea 8(%rsp), %r8：此刻栈顶是 r11（push %r11 最后压的),往下 8
     * 字节是 rcx——算的是"push %rcx 那次写入的内存位置"，不依赖后面
     * call 指令自己还会不会再压栈（这条 lea 在 call 之前执行,%rsp 此刻
     * 就是 push %r11 之后的值,几何关系是固定的,不需要考虑 call 压
     * 返回地址之后 %rsp 还会变——把地址算好之后传给 callee,callee 内部
     * %rsp 再怎么变化都不影响这个已经算好的绝对地址)。
     * lea syscall_saved_user_rsp(%rip), %r9：同理,算的是这个全局变量
     * 本身的地址,不是它当前存的值。 */
    lea 8(%rsp), %r8
    lea syscall_saved_user_rsp(%rip), %r9

    /* System V AMD64 调用约定：第一参数 rdi，第二 rsi，第三 rdx，第四
     * rcx，第五 r8，第六 r9。
     *
     * Lab8 的 syscall_dispatch 签名是
     *   (num, a0, a1, a2, user_rip_slot, user_rsp_slot)
     * 六个参数刚好占满六个寄存器参数位：
     *   rdi=num  rsi=a0  rdx=a1  rcx=a2  r8=rip_slot  r9=rsp_slot
     *
     * 上面两条 lea 已经把 rip_slot/rsp_slot 直接算进了 r8/r9——正好就是
     * 它们该在的位置，不需要任何搬移。剩下的工作只是把用户传进来的
     * rax/rdi/rsi/rdx 往后挪一位，给 num 腾出 rdi。
     *
     * 搬移顺序必须"从高参数位往低参数位"：每一条的目标寄存器都是下一条
     * 的源寄存器，反着来会在读到之前就把源覆盖掉。这是寄存器重排里最
     * 经典的一类错误（等价于交换两个变量忘了用临时变量），而且症状不是
     * 崩溃而是"参数错位"——sys_read 会拿到 (buf, len, 垃圾) 这种看起来
     * 像随机数的参数，很难一眼看出是搬移顺序的问题。
     *
     * ---- Lab7 在这里踩过的一个真实 bug，留档 ----
     *
     * Lab7 的 syscall_dispatch 只有五个参数（没有 a2），当时的写法是先
     * `mov %r8, %rcx`（把 rip_slot 塞进第四参数位 rcx），再 `mov %r9, %r8`
     * （把 rsp_slot 挪进第五参数位 r8）。第二条一旦漏掉，r8 会保持
     * "user_rip_slot 的地址"这个刚搬进 rcx 的旧值不变（没人覆写它，
     * 此刻 r8==rcx，两个寄存器指向*同一个*栈上槽位），于是
     * syscall_dispatch() 读到的 user_rsp_slot
     * 参数实际上是 user_rip_slot 的地址，两者在 sys_exec() 内部变成
     * 同一个写入目标——`*user_rip_slot = USER_PROG_VADDR;` 先写对，
     * 紧接着 `*user_rsp_slot = user_stack_top;` 因为指向同一处内存,
     * 把刚写进去的 USER_PROG_VADDR 原地覆盖成了 user_stack_top,而
     * syscall_saved_user_rsp 这个全局变量本身（真正应该被 sys_exec()
     * 写入新栈顶的地方）完全没被触碰,仍然是这次 syscall 陷入时的旧
     * 用户栈指针。返回路径 `pop %rcx`（读到的是被覆盖过的
     * user_stack_top,不是预期的 USER_PROG_VADDR)接着
     * `mov syscall_saved_user_rsp(%rip), %rsp`（读到的是没被更新的旧
     * rsp)最终 sysretq——现场变成 RIP=user_stack_top（比如
     * 0x402000,即 USER_STACK_VADDR 本身,栈顶再往上一个字节都没映射
     * 过的守护地址),RSP=一个仍然合法但已经过期的旧值,CPU 从
     * RIP=0x402000 处取指直接 #PF（not-present,read,user——指令取指
     * 本身在硬件层面就是一次"读"),现象是 exec 之后子进程立刻缺页崩溃,
     * 而 fork()（sys_fork() 不需要写这两个槽位,从来不会暴露这个问题)
     * 和普通 sys_write/sys_wait（同样不写这两个槽位)全程正常,只有
     * "调用了 exec 的那个进程,exec 生效的那一刻"才会踩中——跟参数
     * 传递 bug 完全对应。
     *
     * Lab8 把 a2 加进来之后，rip_slot/rsp_slot 各自往后挪了一位、正好
     * 落在 lea 算出来的 r8/r9 上，那两条搬移指令自然消失了，这个 bug
     * 也就没有了发生的余地。这是一个值得记下来的规律：需要手工搬移的
     * 寄存器越少，这类错误的空间就越小——能通过"把值一次算到最终位置"
     * 消掉的搬移，就别留着。 */
    mov %rdx, %rcx
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov %rax, %rdi
    call syscall_dispatch
    /* syscall_dispatch 的返回值在 rax（C 调用约定），恰好也是 SYSRET
     * 传回用户态返回值时约定用的寄存器（本课程沿用 Linux syscall ABI
     * 的"rax 传返回值"惯例，见 trap.c syscall_dispatch 声明处注释），
     * 不需要额外搬移。 */

    pop %r11
    pop %rcx

    mov syscall_saved_user_rsp(%rip), %rsp
    sysretq

/* enter_user_mode(entry, stack)：第一次从 ring0 跳进 ring3，不能用
 * sysretq——sysretq 只是"回到刚才那次 SYSCALL 调用点"的镀像，它读取
 * 的 rcx/r11 必须是*之前一次 SYSCALL 硬件自动填入*的用户 RIP/RFLAGS，
 * 这里完全没有发生过 SYSCALL，没有这两个值的合法来源，硬凑一个值
 * 塞进 rcx/r11 然后 sysretq 在语义上是错的（即使可能凑巧不崩，也是在
 * 滥用一条指令去做它不是为此设计的事）。真正对应"凭空构造一次特权级
 * 切换"的机制是 iretq——这正是 IDT 中断/异常路径回到低特权级时用的
 * 同一条指令，只是这次由软件手工搭出 iretq 期望的栈帧，而不是硬件
 * 在异常发生时自动压栈。
 *
 * iretq 从栈顶按顺序弹出五个 64 位值：RIP、CS、RFLAGS、RSP、SS——跟
 * page_fault_stub/timer_stub 里硬件自动压的栈帧形状完全一样（这不是
 * 巧合，这就是 IDT 路径本来的样子，这里只是手工搭一份等价的），意味着
 * push 的顺序必须反过来（SS 最先 push，因为它在栈帧里地址最高，最后
 * 被弹出）。 */
enter_user_mode:
    /* System V AMD64 调用约定：entry=rdi，stack=rsi——先保存到别的
     * 寄存器再开始 push，因为 push 会改变 rsp，但我们还没决定好最终
     * 要用哪个栈（这个函数从内核栈调用，执行期间应该始终留在内核栈上，
     * 直到 iretq 那一刻由硬件原子地切到栈帧里指定的用户 RSP）。 */
    mov %rdi, %rax /* entry */
    mov %rsi, %rdx /* stack */

    push $0x20 | 3 /* SS：用户数据段选择子，RPL=3（trap.c 里的
                    * GDT_SEL_USER_SS_RPL3，这里直接写字面量是因为
                    * .S 文件不方便 #include trap.c 的宏定义——两边
                    * 的数值必须保持同步，跟 user_prog.S 里手写
                    * SYS_WRITE/SYS_EXIT 字面量是同一类"跨文件手工
                    * 契约"，README 会一并指出）。 */
    push %rdx      /* RSP：用户栈顶（USER_STACK_VADDR，kernel_main.c
                    * 传进来的第二个参数）。 */
    push $0x202    /* RFLAGS：IF=1（bit 9）+ bit 1（Intel 手册规定
                    * 永远置 1 的保留位）——用户程序需要能被定时器
                    * 中断打断（不像 SYSCALL 路径故意通过 FMASK 清
                    * IF，用户态代码是可以被中断的常规执行环境）。 */
    push $(0x28 | 3) /* CS：用户代码段选择子，RPL=3（GDT_SEL_USER_CS_
                      * RPL3）。 */
    push %rax      /* RIP：用户程序入口（USER_PROG_VADDR）。 */

    iretq
