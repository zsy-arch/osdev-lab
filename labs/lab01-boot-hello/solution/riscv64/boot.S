/* Lab1 riscv64 solution: OpenSBI 交接之后的 S 模式入口。
 *
 * OpenSBI 跳到这里时的约定（RISC-V SBI 规范里"下一阶段引导程序"的调用
 * 约定）：CPU 已经在 S 模式（Supervisor Mode），MMU 关闭（Sv39 分页尚未
 * 启用——这也是为什么 riscv64 这边 Lab1 完全不需要写页表，页表要到 Lab4
 * 才作为教学内容展开，这是和 x86_64 长模式"必须先有页表才能往下走"的
 * 刻意对照，见 docs/arch-compare.md），寄存器 a0 = hartid（当前硬件线程
 * 编号，多核场景下用来区分是哪个核在跑，Lab1/Lab10 之前只有一个 hart 在跑
 * 所以暂时不用），a1 = 指向 Device Tree Blob（DTB）的物理地址（描述这台
 * QEMU virt 机器的硬件布局，Lab3 会解析它拿可用内存范围，Lab1 不用）。
 *
 * 相比 x86_64 一长串"开 PAE -> 填页表 -> 设 CR3 -> 写 EFER -> 开分页 ->
 * 换 GDT -> 远跳转"才能进入能跑 C 代码的状态，riscv64 这里只需要给个栈
 * 指针就可以直接调用 C 函数——这不是本课程简化了 riscv64，是两种架构在
 * "固件/引导器该替内核做多少事"这件事上的真实设计差异。
 */
/* .text.boot 是特意起的名字，不是 .text——链接脚本靠这个名字保证 _start
 * 一定被摆在链接结果的最前面（0x80200000）。原因见 linker.ld 里的注释：
 * OpenSBI 跳过来的地址是固定值，不会去看 ELF entry point,谁的目标文件在
 * 链接命令行上排第几都不影响最终布局,唯一可靠的办法是用专门的段名。 */
.section .text.boot
.global _start
.type _start, @function
_start:
    /* a0/a1 暂时不用，但特意不覆盖它们（没有拿 a0/a1 做任何运算），
     * 如果后面的 Lab 需要它们，调用约定上它们还完好地留在原处。 */
    la sp, stack_top
    call kernel_main

    /* kernel_main 正常不会返回；意外返回时用 wfi（Wait For Interrupt）
     * 死循环让 hart 停下来，而不是往后越界执行到未知内存。wfi 和
     * x86_64 的 hlt 是同一个思路：让 CPU 进入低功耗等待状态，等中断
     * 唤醒——Lab1 阶段还没配置中断，所以效果上等同于永久停机。 */
1:  wfi
    j 1b
.size _start, . - _start

.section .bss
.align 16   /* RISC-V 调用约定要求函数调用边界上 sp 是 16 字节对齐的 */
.skip 16384    /* 16KiB 内核启动栈，Lab1 阶段够用，Lab2 会重新规划栈布局 */
stack_top:
