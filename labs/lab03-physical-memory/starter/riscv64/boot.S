/* 与 solution 完全相同——riscv64 不需要任何新汇编代码就能把 DTB 指针
 * 传给 kernel_main()：OpenSBI 交接时 a0=hartid、a1=DTB 物理地址已经
 * 落在调用约定的前两个整数参数寄存器里，只要 C 函数签名声明成
 * kernel_main(uint64_t hartid, uint64_t dtb_paddr)，这里不需要任何改动。
 * 见 labs/lab03-physical-memory/solution/riscv64/boot.S 的完整注释。
 */
.section .text.boot
.global _start
.type _start, @function
_start:
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
