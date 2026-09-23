/* Lab5 x86_64：两个 IDT 向量各自的汇编入口 stub。page_fault_stub 跟
 * Lab4 完全一样（#PF 是异常，硬件会自动压 error code），新增的
 * timer_stub 对应 PIT 定时器中断（IRQ0，重映射后的向量 32）——外部
 * 中断跟异常在 CPU 眼里除了"有没有硬件 error code"这一点，压栈/iretq
 * 的形状完全相同（都是 RIP/CS/RFLAGS/RSP/SS 五项），所以 timer_stub
 * 比 page_fault_stub 只少了"从栈上取 error code 传参"这一步，其余
 * 结构不变。
 */
.code64

.extern page_fault_handler
.extern timer_interrupt_handler
.global page_fault_stub
.global timer_stub

/* 已经写好，不是 TODO：跟 Lab4 完全一样，Lab5 没有改动 #PF 这条路径。 */
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

/* TODO 1：实现 timer_stub —— IRQ0（PIT）触发时 CPU 跳到这里。
 *
 * 提示：跟 page_fault_stub 的结构一样，先保存全部通用寄存器，
 * 但 IRQ0 没有硬件 error code（这是"中断"，不是"带 error code 的
 * 异常"，8259A/APIC 递送外部中断时 CPU 不会往栈上多压一项），所以：
 *   1) 不需要 page_fault_stub 那个"从栈上取一项传参"的步骤，直接
 *      call timer_interrupt_handler（零参数，对照 pit.c 里的签名）。
 *   2) 恢复完寄存器之后直接 iretq，不需要先 add $8,%rsp 弹 error code
 *      （栈上原本就没有多压这一项）。
 *
 * timer_stub:
 *     push %rax
 *     push %rbx
 *     push %rcx
 *     push %rdx
 *     push %rsi
 *     push %rdi
 *     push %rbp
 *     push %r8
 *     push %r9
 *     push %r10
 *     push %r11
 *     push %r12
 *     push %r13
 *     push %r14
 *     push %r15
 *
 *     call timer_interrupt_handler
 *
 *     pop %r15
 *     pop %r14
 *     pop %r13
 *     pop %r12
 *     pop %r11
 *     pop %r10
 *     pop %r9
 *     pop %r8
 *     pop %rbp
 *     pop %rdi
 *     pop %rsi
 *     pop %rdx
 *     pop %rcx
 *     pop %rbx
 *     pop %rax
 *
 *     iretq
 */
