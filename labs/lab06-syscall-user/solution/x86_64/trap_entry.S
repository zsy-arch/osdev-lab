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
    lea __stack_top(%rip), %rsp

    /* rcx（用户 RIP）/r11（用户 RFLAGS）是 SYSRET 唯一会读的两个
     * 寄存器，syscall_dispatch 是一个普通 C 函数，System V AMD64
     * 调用约定里 rcx 是 callee-clobbered（第四个整数参数寄存器）——
     * 如果不在调用前保存，call 指令本身（不需要 rcx）大概率不会碰它，
     * 但 syscall_dispatch 函数体内部完全可能把 rcx 用作临时寄存器，
     * 保存下来是唯一不依赖"这个函数恰好没用到 rcx"这种脆弱假设的
     * 做法。r11 同理是 callee-clobbered（调用约定里的易失寄存器）。 */
    push %rcx
    push %r11

    /* System V AMD64 调用约定：第一参数 rdi，第二 rsi，第三 rdx——
     * syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1) 需要
     * num=rax（SYSCALL 号约定的调用号寄存器）挪到 rdi，原来的 rdi/rsi
     * （用户程序的两个参数）依次挪到 rsi/rdx。三个 mov 的顺序不能颠倒
     * ——如果先覆盖 rdi，原来 rdi 的值就丢了，所以先搬 rsi 到 rdx，
     * 再搬 rdi 到 rsi，最后才把 rax 搬进 rdi（后搬先用，经典的"从后往
     * 前挪"顺序，避免中间值被自己覆盖）。 */
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
