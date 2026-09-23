/* Lab3 riscv64 solution：这份文件和 Lab2 一字不差——riscv64 这边不需要
 * 任何新的汇编代码就能把 DTB 指针传给 kernel_main()。
 *
 * 对比 x86_64 那份（labs/lab03-physical-memory/solution/x86_64/boot.S）
 * 需要新增一条 mov 才能把 Multiboot2 指针挪进调用约定的参数寄存器：
 * riscv64 从 OpenSBI 交接控制权那一刻起，a0 就已经是 hartid、a1 就已经
 * 是 DTB 物理地址（Lab2 的注释就写了这句话），而 RISC-V 调用约定
 * （calling convention）的前两个整数参数寄存器恰好就是 a0/a1——只要
 * kernel_main() 的 C 签名声明成 kernel_main(uint64_t hartid,
 * uint64_t dtb_paddr)，call kernel_main 这一条指令不用变，编译器生成的
 * 函数体第一句话读到的 a0/a1 就是对的值，零额外汇编成本。这是 riscv64
 * "现代设计，接口标准化"相对 x86 历史包袱的又一个具体例子（Lab1 README
 * 的对照表提过这一类差异，这里是它在 Lab3 场景下的实例）。
 */
.section .text.boot
.global _start
.type _start, @function
_start:
    /* a0 = hartid, a1 = DTB 物理地址——从这里到 call kernel_main 之间
     * 只有 BSS 清零循环，用的是 t0/t1，不碰 a0/a1，原样存活到 C 函数。 */

    lla t0, __bss_start
    lla t1, __bss_end
clear_bss:
    bge t0, t1, bss_cleared
    sb zero, 0(t0)
    addi t0, t0, 1
    j clear_bss
bss_cleared:

    lla sp, __stack_top
    call kernel_main

1:  wfi
    j 1b
.size _start, . - _start
