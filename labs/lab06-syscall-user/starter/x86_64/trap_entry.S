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
.extern __stack_top
.extern page_fault_handler
.extern timer_interrupt_handler
.global page_fault_stub
.global timer_stub
.global syscall_entry
.global enter_user_mode

/* 已经写好，不是 TODO：page_fault_stub/timer_stub 跟 Lab5 完全一样，
 * IDT 中断门触发时硬件已经自动压好 iretq 期望的栈帧（RIP/CS/RFLAGS
 * [/RSP/SS，跨特权级时]），这里只需要保存/恢复通用寄存器。 */
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

/* TODO 1：实现 syscall_entry。
 *
 * 进来的时候 %rsp 还是用户栈，%rcx=用户RIP，%r11=用户RFLAGS，
 * %rax=系统调用号，%rdi/%rsi=参数（用户程序按这个约定填好，见
 * user_prog.S）——这些约定寄存器在换栈之前都不能被破坏。
 *
 * 步骤：
 *   1. 换栈：用一个全局变量（syscall_saved_user_rsp）暂存用户 RSP，
 *      不是压栈——这一刻 %rsp 还是用户栈指针，如果先 push 保存东西，
 *      会把数据压到用户栈里（用户栈内容对内核不可信，且空间很小）；
 *      全局变量在切栈动作发生之前就能被写入，不依赖任何栈的存在。
 *      换到 __stack_top（内核栈顶）。
 *   2. 保存 rcx（用户 RIP）/r11（用户 RFLAGS）——这是 SYSRET 唯一会
 *      读的两个寄存器，syscall_dispatch 是一个普通 C 函数，System V
 *      AMD64 调用约定里 rcx 是 callee-clobbered（第四个整数参数
 *      寄存器），syscall_dispatch 函数体内部完全可能把 rcx 用作临时
 *      寄存器，保存下来是唯一不依赖"这个函数恰好没用到 rcx"这种脆弱
 *      假设的做法。r11 同理是 callee-clobbered。
 *   3. 参数搬移：System V AMD64 调用约定第一参数 rdi，第二 rsi，第三
 *      rdx——syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1)
 *      需要 num=rax 挪到 rdi，原来的 rdi/rsi（用户程序的两个参数）
 *      依次挪到 rsi/rdx。三个 mov 的顺序不能颠倒——如果先覆盖 rdi，
 *      原来 rdi 的值就丢了，要先搬 rsi 到 rdx，再搬 rdi 到 rsi，最后
 *      才把 rax 搬进 rdi（后搬先用，从后往前挪，避免中间值被自己
 *      覆盖）。
 *   4. call syscall_dispatch——返回值在 rax，恰好也是 SYSRET 传回
 *      用户态返回值时约定用的寄存器，不需要额外搬移。
 *   5. 恢复 r11/rcx（顺序跟保存时相反）。
 *   6. 恢复用户栈（从 syscall_saved_user_rsp 读回 rsp）。
 *   7. sysretq。
 *
 * 提示：
 * syscall_entry:
 *     mov %rsp, syscall_saved_user_rsp(%rip)
 *     lea __stack_top(%rip), %rsp
 *
 *     push %rcx
 *     push %r11
 *
 *     mov %rsi, %rdx
 *     mov %rdi, %rsi
 *     mov %rax, %rdi
 *     call syscall_dispatch
 *
 *     pop %r11
 *     pop %rcx
 *
 *     mov syscall_saved_user_rsp(%rip), %rsp
 *     sysretq
 */

/* TODO 2：实现 enter_user_mode(entry, stack)。
 *
 * 第一次从 ring0 跳进 ring3，不能用 sysretq——sysretq 只是"回到刚才
 * 那次 SYSCALL 调用点"的镀像，它读取的 rcx/r11 必须是*之前一次
 * SYSCALL 硬件自动填入*的用户 RIP/RFLAGS，这里完全没有发生过
 * SYSCALL，没有这两个值的合法来源。真正对应"凭空构造一次特权级切换"
 * 的机制是 iretq——这正是 IDT 中断/异常路径回到低特权级时用的同一条
 * 指令，只是这次由软件手工搭出 iretq 期望的栈帧，而不是硬件在异常
 * 发生时自动压栈。
 *
 * iretq 从栈顶按顺序弹出五个 64 位值：RIP、CS、RFLAGS、RSP、SS——跟
 * page_fault_stub/timer_stub 里硬件自动压的栈帧形状完全一样，意味着
 * push 的顺序必须反过来（SS 最先 push，因为它在栈帧里地址最高，最后
 * 被弹出）。
 *
 * System V AMD64 调用约定：entry=rdi，stack=rsi——先保存到别的寄存器
 * 再开始 push，因为 push 会改变 rsp，但这个函数从内核栈调用，执行
 * 期间应该始终留在内核栈上，直到 iretq 那一刻由硬件原子地切到栈帧里
 * 指定的用户 RSP。
 *
 * 需要 push 的五个值（从高地址到低地址，也就是 push 的顺序）：
 *   SS：用户数据段选择子，RPL=3（0x20 | 3——trap.c 里的
 *       GDT_SEL_USER_SS_RPL3，.S 文件不方便 #include trap.c 的宏
 *       定义，这里直接写字面量，两边数值必须保持同步）。
 *   RSP：用户栈顶（stack 参数）。
 *   RFLAGS：0x202——IF=1（bit 9）+ bit 1（Intel 手册规定永远置 1 的
 *       保留位）。用户程序需要能被定时器中断打断。
 *   CS：用户代码段选择子，RPL=3（0x28 | 3，GDT_SEL_USER_CS_RPL3）。
 *   RIP：用户程序入口（entry 参数）。
 *
 * 提示：
 * enter_user_mode:
 *     mov %rdi, %rax // entry
 *     mov %rsi, %rdx // stack
 *
 *     push $0x20 | 3
 *     push %rdx
 *     push $0x202
 *     push $(0x28 | 3)
 *     push %rax
 *
 *     iretq
 */
