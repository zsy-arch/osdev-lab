/* Lab1 starter (riscv64)：内核入口。
 *
 * OpenSBI 完成 M 模式初始化后，直接跳到固定物理地址 0x80200000
 * （见 linker.ld），这时 CPU 已经在 S 模式，是完整的 64 位执行环境：
 * 不需要像 x86_64 那样切换任何模式，直接设好栈指针就能安全调用 C 函数。
 *
 * 传入的寄存器约定（本 Lab 暂不使用，Lab3/Lab10 会用到）：
 *   a0 = hartid（当前核心编号）
 *   a1 = 设备树（DTB）指针
 *
 * .text.boot 是专属段名，配合 linker.ld 里的 KEEP(*(.text.boot))，
 * 保证 _start 被强制排在链接结果的最前面，落在 0x80200000。这不是
 * 可选的细节：OpenSBI 硬编码跳转到这个地址，完全不读 ELF 的入口
 * 字段，如果 _start 因为多目标文件链接顺序不确定而没有排在最前面，
 * CPU 会从一条随机指令开始"执行"，没有任何报错提示你。
 */
.section .text.boot
.global _start
.type _start, @function
_start:
    /* TODO：把栈指针设到 stack_top（下面 .bss 里定义的符号），
     * 然后 call kernel_main。
     *
     * riscv 的函数调用约定要求栈在调用边界保持 16 字节对齐
     * （不是 4 字节），下面 .bss 里 stack_top 已经用 .align 16
     * 保证了这一点，你只需要把地址装进 sp。
     *
     * 提示：
     *   lla sp, stack_top
     *   call kernel_main
     *
     * 用 lla（load local address）不用 la：两者在不少环境下展开结果
     * 一样，但 la 在某些 riscv64-linux-gnu 工具链默认按"位置无关代码"
     * 展开成经 GOT 间接寻址的序列，这份 freestanding、没有 GOT 的内核
     * 撑不起这个假设，符号稍微离得远一点就会在链接期报
     * relocation truncated to fit: R_RISCV_GOT_HI20。lla 保证始终是
     * 直接的 auipc+addi，没有这个隐患。 */

1:  wfi
    j 1b
.size _start, . - _start

.section .bss
.balign 16
.skip 16384
stack_top:
