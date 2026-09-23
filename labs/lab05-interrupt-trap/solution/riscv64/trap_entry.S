/* Lab5 riscv64：跟 Lab4 完全一样,一个字节都没改——这不是偷懒漏做,
 * 是因为 Lab5 新增的时钟中断和 Lab4 的 page fault 共享同一个 stvec
 * 入口（Direct 模式,所有 trap 不分种类都跳到这里,靠 C 函数体内读
 * scause 分发,见 trap.c 里 supervisor_trap_handler() 开头的 if 链）,
 * 汇编层面"保存寄存器->调用 C 函数->恢复寄存器->sret"这套流程对
 * 异常和中断没有任何区别,不需要像 x86_64 那样为新中断源单独写一个
 * timer_stub（x86_64 的 IDT 是按向量号分发到不同入口地址,riscv64
 * 的 Direct 模式只有一个入口,这是两边这里出现"要不要新增 stub"这个
 * 决策点不同的根本原因）。
 *
 * trap 的汇编入口 stub,负责跟硬件对接的部分,把控制权转交给 trap.c
 * 里的 supervisor_trap_handler()（普通 C 函数)。跟 x86_64 版本
 * trap_entry.S 开头注释是同一个分工理由,这里只写 riscv 特有的差异。
 *
 * 跟 x86_64 的 #PF stub 相比,这里没有"从栈上取 error code 放进参数
 * 寄存器"这一步——riscv64 的异常信息（scause/stval/sepc)全部是普通
 * CSR,C 函数体内自己 csrr 就行,不需要汇编层面帮忙搬运,所以
 * supervisor_trap_handler() 是零参数的（对照 trap.c 里的签名)。
 */
.section .text
.global supervisor_trap_entry
.align 4

supervisor_trap_entry:
    /* riscv 进入 trap 时不会自动切换栈指针（跟 x86_64 从用户栈切到
     * TSS 里配置好的内核栈不一样——本课程目前只有内核态代码,sp 本来
     * 就已经是合法的内核栈,不需要额外切换,这里直接在当前栈上开一段
     * 空间保存寄存器)。
     *
     * 只保存"调用者可能还在用、且会被下面 call 打乱"的寄存器：
     * ra（返回地址,call 指令会覆盖它)以及所有 caller-saved 的整数
     * 寄存器（riscv 调用约定：a0-a7/t0-t6 是 caller-saved,call 前
     * 不保证保留;s0-s11 是 callee-saved,C 函数会自己保. 这里教学上
     * 偷懒的做法跟 x86_64 版本一样：不去精确分析
     * supervisor_trap_handler 具体用到哪几个,全部保存,保证不会漏.
     * sp/gp/tp 不用保存：sp 全程没变过(上面那条注释),gp/tp 是全局
     * 不变的,trap 处理过程不会修改它们)。 */
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

    call supervisor_trap_handler

    /* supervisor_trap_handler 目前是 panic() 后不返回的（trap.c 里
     * 写明了"Lab4 不实现故障恢复"),下面的恢复+sret 在当前设计下永远
     * 不会真正执行到——跟 x86_64 版本同一个理由,仍然写完整、正确的
     * 版本,方便以后改成可恢复策略时不需要重写这一段。 */
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

    sret
