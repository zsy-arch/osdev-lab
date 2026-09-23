/* Lab4 x86_64：#PF 的汇编入口 stub，负责跟硬件对接的部分，把控制权
 * 转交给 trap.c 里的 page_fault_handler()（普通 C 函数，不需要关心
 * 下面这些细节）。
 *
 * 为什么不能让 IDT 直接指向一个普通 C 函数：
 *   1. CPU 触发异常时不会保存通用寄存器（只有 RIP/CS/RFLAGS/RSP/SS，
 *      #PF 还多一个 error code，会被硬件自动压栈）——如果 C 函数体内
 *      用到的寄存器刚好和被打断的代码正在用的冲突，返回后那些代码会
 *      读到错误的值。必须在 call 之前手动保存、返回之后手动恢复。
 *   2. 硬件压栈的 error code 在栈上的位置，不符合 System V AMD64
 *      调用约定"参数从 %rdi 开始"的约定——C 函数没有办法直接读到它，
 *      必须由这段 asm 显式从栈上取出来，放进 %rdi。
 *   3. 异常返回不能用普通的 ret：ret 只会弹出一个返回地址，而 CPU
 *      压栈的是 RIP/CS/RFLAGS/RSP/SS 五个字段（外加 error code，但
 *      error code 在 iretq 之前必须先弹掉，它不属于 iretq 期望的
 *      返回帧格式）——必须用专门的 iretq 指令。
 */
.code64

.extern page_fault_handler
.global page_fault_stub

page_fault_stub:
    /* 进入时栈布局（从高地址到低地址，即从栈顶往下）：
     *   [原来的 SS]
     *   [原来的 RSP]
     *   [原来的 RFLAGS]
     *   [原来的 CS]
     *   [原来的 RIP]
     *   [error code]      <- 当前 %rsp 指向这里
     * 前五项是 CPU 自动压的"异常返回帧"，error code 是 #PF 专属的
     * 额外一项（不是所有异常都有，但#PF 属于"有"的那一类）。
     */

    /* 保存所有会被下面 call 打乱的通用寄存器——教学上偷懒的做法：
     * 全部保存，不去分析 page_fault_handler() 实际会用到哪几个,
     * 保证不会漏保存导致返回后寄存器状态错误。 */
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

    /* error code 现在在"15 个 push 之后"的栈偏移处（15*8=120 字节），
     * 取出来放进 %rdi（System V AMD64 调用约定第一个整数参数）。 */
    mov 120(%rsp), %rdi
    call page_fault_handler

    /* page_fault_handler 目前是 panic() 后不返回的（trap.c 里写明了
     * "Lab4 不实现故障恢复"），下面的恢复+iretq 代码在当前设计下永远
     * 不会真正执行到——但仍然写完整、正确的版本，不写"反正用不到就
     * 摆个样子"的残缺版本：万一以后哪个 Lab 改成可恢复的处理策略
     * （比如实现按需分页），这里不需要重写，直接改 trap.c 让
     * page_fault_handler 正常 return 就行。 */
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

    /* 弹掉 error code——它不属于 iretq 期望的返回帧格式，必须在
     * iretq 之前从栈上移除，不能直接 iretq。 */
    add $8, %rsp

    iretq
