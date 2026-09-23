/* Lab2 riscv64 solution：OpenSBI 交给我们控制权之后，先清 BSS、设栈，
 * 再跳 C 代码。相比 labs/lab01-boot-hello/solution/riscv64/boot.S，
 * 多出来的就是这段清零循环——riscv64 这边"到 C 代码能跑之前要做的事"
 * 本来就比 x86_64 少得多（Lab1 README 的核心概念一节讲过这个对比），
 * Lab2 加的 BSS 清零是两边都必须要有的，不因为架构简单就能省略。
 */
.section .text.boot
.global _start
.type _start, @function
_start:
    /* a0 = hartid, a1 = DTB 物理地址，Lab2 阶段仍然不用，也不覆盖它们
     * （不用它们做任何运算），Lab3 解析 DTB 时会用到 a1。 */

    /* --- 清零 BSS --- */
    /* la 伪指令在 riscv64 下会展开成 auipc + addi 这类相对当前 PC 的
     * 寻址序列（因为 -mcmodel=medany 生成的是位置无关的地址计算），
     * 不是一条真的硬件指令，但用起来和"把符号地址装进寄存器"完全一样，
     * 不需要关心底层展开细节。 */
    la t0, __bss_start
    la t1, __bss_end
clear_bss:
    bge t0, t1, bss_cleared    /* t0 >= t1 说明清完了（等于覆盖 BSS 长度恰好是 0 的情况） */
    sb zero, 0(t0)
    addi t0, t0, 1
    j clear_bss
bss_cleared:

    la sp, __stack_top
    call kernel_main

1:  wfi
    j 1b
.size _start, . - _start
