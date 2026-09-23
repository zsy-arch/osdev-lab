/* Lab4 starter (riscv64)：异常入口的汇编外壳。
 *
 * 和 x86_64 一样,C 函数不能直接当异常入口用：异常触发的瞬间,通用
 * 寄存器里还是"被打断的那条指令"用到一半的值,必须先保存,再去调用
 * C 函数,C 函数返回后还要恢复,才能用 sret 安全地回到被打断的地方
 * 继续执行。
 *
 * 但具体要保存哪些寄存器,riscv64 和 x86_64 不一样,而且比 x86_64
 * 简单：riscv64 的调用约定把寄存器分成"caller-saved"（调用者负责
 * 保存,ra/t0-t6/a0-a7)和"callee-saved"（被调用者负责保存,s0-s11)
 * 两类——这里只需要保存 caller-saved 的那一组,因为 supervisor_trap_
 * handler() 内部如果用到 s0-s11,C 编译器生成的代码会*自动*帮我们
 * 保存/恢复它们,不需要在这里手动处理。sp（栈指针本身不会变)、gp/tp
 * （全局指针/线程指针,在这个内核里是全局常量)也不需要保存。
 *
 * 一共 16 个寄存器：ra, t0-t6（7 个), a0-a7（8 个),16*8=128 字节。
 */
.section .text
.align 4
.globl supervisor_trap_entry
supervisor_trap_entry:
    /* TODO 1：在栈上开 128 字节空间,把 ra/t0-t6/a0-a7 按固定偏移
     * 存进去。
     *
     * 提示（偏移从 0 到 120,每个寄存器 8 字节,顺序任意但必须和
     * TODO 3 的恢复顺序一一对应):
     *   addi sp, sp, -128
     *   sd   ra,  0(sp)
     *   sd   t0,  8(sp)
     *   sd   t1,  16(sp)
     *   sd   t2,  24(sp)
     *   sd   t3,  32(sp)
     *   sd   t4,  40(sp)
     *   sd   t5,  48(sp)
     *   sd   t6,  56(sp)
     *   sd   a0,  64(sp)
     *   sd   a1,  72(sp)
     *   sd   a2,  80(sp)
     *   sd   a3,  88(sp)
     *   sd   a4,  96(sp)
     *   sd   a5,  104(sp)
     *   sd   a6,  112(sp)
     *   sd   a7,  120(sp)
     */

    /* TODO 2：调用 C 端的异常分类函数。
     *
     * 不需要像 x86_64 那样从栈上取参数传给它——所有异常信息（scause/
     * stval)都在 CSR 里,supervisor_trap_handler() 自己会去读,不需要
     * 我们在这里准备参数。
     *
     * 提示：
     *   call supervisor_trap_handler
     */

    /* TODO 3：按 TODO 1 存入时的偏移,原样读回来（顺序不影响正确性,
     * 但保持和 TODO 1 一致方便对照阅读),再把栈指针加回去。
     *
     * 提示：
     *   ld   ra,  0(sp)
     *   ld   t0,  8(sp)
     *   ld   t1,  16(sp)
     *   ld   t2,  24(sp)
     *   ld   t3,  32(sp)
     *   ld   t4,  40(sp)
     *   ld   t5,  48(sp)
     *   ld   t6,  56(sp)
     *   ld   a0,  64(sp)
     *   ld   a1,  72(sp)
     *   ld   a2,  80(sp)
     *   ld   a3,  88(sp)
     *   ld   a4,  96(sp)
     *   ld   a5,  104(sp)
     *   ld   a6,  112(sp)
     *   ld   a7,  120(sp)
     *   addi sp, sp, 128
     */

    /* TODO 4：sret 返回。
     *
     * sret 会同时做两件事：把 pc 恢复到 sepc 存的地址（被打断的那条
     * 指令),把特权级恢复到 sstatus.SPP 记录的值——这两件事和 x86_64
     * 的 iretq 恢复 CS/RFLAGS 是同一个道理,只是换了一套寄存器名字。
     * 本 Lab 触发的是不可恢复的故障,所以这条 sret 实际不会被走到
     * （前面的 panic() 会先让内核停下),但作为异常处理框架的完整性,
     * 仍然需要写上,为后续 Lab 实现"故障后可以恢复执行"做准备。
     *
     * 提示：
     *   sret
     */
