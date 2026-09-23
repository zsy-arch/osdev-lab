/* Lab6 riscv64：全课程第一段真正跑在 U-mode 的代码。跟 x86_64 那份
 * （../x86_64/user_prog.S）同一个教学取向——纯汇编，不搭 C 运行环境，
 * 见那份文件顶部注释，原因完全一样，这里不重复。
 *
 * 独立编译/链接（自己的 user_prog.ld，链接到固定地址
 * USER_PROG_VADDR，见 kernel_main.c），产物用 user_blob.S 的 .incbin
 * 整段嵌入内核镜像——流水线跟 x86_64 一侧完全对应，见 Makefile 顶部
 * 注释，riscv64/x86_64 走的是同一套通用规则，不需要单独写一份。
 *
 * 已经写好，不是 TODO：下面这套调用约定（本课程自定义，借用 RISC-V
 * SBI 调用约定里"a7=号，a0-a5=参数"这套角色分工——sbi.h 顶部注释已经
 * 介绍过 SBI 那一层用的正是这个约定，这里的系统调用号不是 SBI
 * EID/FID，是本课程自己的 syscall.h 数值，但寄存器角色分工借用同一套）：
 *   a7 = 系统调用号（syscall.h 的 SYS_WRITE/SYS_EXIT）
 *   a0/a1 = 前两个参数
 *   返回值在 a0
 * 跟 x86_64 侧 rax=号、rdi/rsi=参数、结果回 rax 是同一套角色分工在
 * 不同寄存器上的体现，trap.c/trap_entry.S 已经按这个分工写好了内核侧，
 * 用户侧必须对上。 */
.section .text
.global _user_start

_user_start:
    /* TODO 1：调用 sys_write(msg, len)，再调用 sys_exit(0)。
     *
     * sys_write：
     *   - msg 地址是链接期常量（USER_PROG_VADDR 之后的偏移），用 la
     *     取地址（跟 x86_64 侧同一个简化，不需要运行时定位）。
     *   - 长度*不能*用 `li a1, msg_len` 这种写法——如果 .rodata 里像
     *     x86_64 侧那样定义 `msg_len = . - msg`，这个立即数展开在
     *     GNU as（riscv 目标）这里会报 `illegal operands`：msg_len
     *     是链接期表达式，不是纯汇编期常量，`li` 伪指令的立即数展开
     *     （要在 addi 单条/lui+addi 两条之间选）要求操作数在汇编阶段
     *     就能求值成一个具体数字，这里的长度直到链接阶段才能真正
     *     确定——跟 x86_64 侧 `mov $msg_len, %rsi` 能直接工作是两种
     *     汇编器在"立即数表达式几时求值"上的差异，不是抄错。正确
     *     做法：在 .rodata 里放两个标签（msg 和 msg_end，不要
     *     `msg_len = . - msg`），用两条 la 分别取地址、sub 相减，把
     *     "算长度"这件事完全挪到运行时用寄存器做，绕开对汇编期常量
     *     折叠的依赖。
     *   - a7 设成 1（SYS_WRITE，必须跟 ../syscall.h 里的 SYS_WRITE
     *     数值一致——用户程序不能 #include 内核头文件，跟 x86_64 侧
     *     同一个"手写字面量、两边保持同步"的手工契约）。
     *   - 执行 ecall 指令陷入内核。
     *
     * sys_exit：
     *   - a0 设成 0（退出码）。
     *   - a7 设成 2（SYS_EXIT，同上必须跟 syscall.h 一致）。
     *   - 执行 ecall。
     *
     * 提示：
     * la a0, msg
     * la a1, msg_end
     * sub a1, a1, a0
     * li a7, 1
     * ecall
     *
     * li a0, 0
     * li a7, 2
     * ecall
     */

    /* TODO 2：写一个死循环兜底，用 j（不是 wfi）。
     *
     * 正常流程不会走到这里——见 ../x86_64/user_prog.S 对应位置的注释，
     * 用可控的挂起替换不可控的乱跑。
     *
     * 关键坑：不要用 wfi（riscv 版本的"停下来等中断"，看起来像是
     * x86_64 hlt 的直接对应物）。QEMU 里实测触发：wfi 在 U-mode 执行
     * 直接变成 illegal instruction 陷入内核（QEMU 打的诊断是
     * "system_opcode_insn: Invalid opcode for CSR read/write
     * instruction"，scause=2），根因是 mstatus.TW（Timeout Wait）位
     * ——riscv-privileged 规范原文："When TW=0, the WFI instruction
     * may execute in lower privilege modes... When TW=1, then if WFI
     * is executed in any less-privileged mode... [it traps]"，本 Lab
     * 从未主动设置过 mstatus.TW（只在 sstatus 视图里操作过
     * SPP/SPIE/SUM 这几个跟 S-mode 相关的位，TW 是 mstatus 独有、
     * sstatus 视图里根本看不到的位，没有任何代码路径改过它），QEMU
     * 给出的复位值让它在这里表现为 1，U-mode 的 wfi 因此永远合法地
     * 被拒绝——这跟 x86_64 的 hlt 不一样：x86_64 的 hlt 在 ring<0
     * 执行是*无条件* #GP，没有类似 TW 这样的开关可以让它在用户态合法
     * 执行，所以不能想当然认为 riscv64 的 wfi 就是 x86_64 的 hlt 的
     * 直接对应物，两者在用户态的可用性并不一样。正确写法是跟 x86_64
     * 侧保持一致的忙等循环（j 1b），是唯一一种不依赖任何特权位状态、
     * 在 U-mode 下保证合法的"卡住不再前进"写法。
     *
     * 提示：
     * 1:
     *     j 1b
     */

.section .rodata
msg:
    .ascii "hello from user mode (riscv64, ecall/sret)\n"
msg_end:
