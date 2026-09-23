/* Lab2 starter (riscv64)：OpenSBI 交给我们控制权之后，先清 BSS、设栈，
 * 再跳 C 代码。相比 Lab1，多出来的就是 BSS 清零这一步——riscv64 这边
 * "到 C 代码能跑之前要做的事"本来就比 x86_64 少得多，但 BSS 清零是
 * 两边都必须要有的，不因为架构简单就能省略。
 *
 * a0 = hartid（当前核心编号）、a1 = 设备树（DTB）指针，本 Lab 仍然
 * 不用，Lab3 解析 DTB 时会用到 a1。
 */
.section .text.boot
.global _start
.type _start, @function
_start:
    /* TODO 1：清零 __bss_start 到 __bss_end 这段内存（逐字节即可）。
     *
     * 为什么要自己清零：ELF 的 .bss 段（SHT_NOBITS）只是"预留空间"，
     * ELF 规范本身并不保证加载器会把它清零——QEMU 的 ELF 加载器这么做
     * 只是实现上的惯例，不是必须遵守的契约。本 Lab 的 kernel_main.c
     * 会读一个未显式初始化的静态变量来验证这段清零逻辑是否真的生效。
     *
     * lla（load local address）伪指令在 riscv64 下会展开成 auipc +
     * addi 这类相对当前 PC 的直接寻址序列（因为 -mcmodel=medany 生成的
     * 是位置无关的地址计算），不需要关心底层展开细节，用起来和"把符号
     * 地址装进寄存器"完全一样。这里特意用 lla 不用看起来更常见的 la：
     * la 在不少 riscv64-linux-gnu 工具链下默认按"支持 PIC/动态链接"
     * 展开成经 GOT 间接寻址的序列，而这份 freestanding 内核
     * （-nostdlib -static）根本没有 GOT，符号地址算出来的间接寻址一旦
     * 超出编码范围就会在链接期报 relocation truncated to fit:
     * R_RISCV_GOT_HI20。lla 保证永远是直接形式，没有这个隐患。
     *
     * 伪代码（t0 走地址，t1 是结束地址）：
     *   t0 = __bss_start
     *   t1 = __bss_end
     *   while (t0 < t1) {
     *       *t0 = 0
     *       t0 += 1
     *   }
     *
     * 提示：
     *   lla t0, __bss_start
     *   lla t1, __bss_end
     *   clear_bss:
     *   bge t0, t1, bss_cleared
     *   sb zero, 0(t0)
     *   addi t0, t0, 1
     *   j clear_bss
     *   bss_cleared:
     */


    /* TODO 2：把栈指针设到链接脚本定义的 __stack_top，然后 call kernel_main。
     *
     * riscv 的函数调用约定要求栈在调用边界保持 16 字节对齐（不是 4 字节），
     * linker.ld 里 __stack_top 已经用 . = ALIGN(16); 保证了这一点。
     *
     * 提示：
     *   lla sp, __stack_top
     *   call kernel_main
     */


1:  wfi
    j 1b
.size _start, . - _start
