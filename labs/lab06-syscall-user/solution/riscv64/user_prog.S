/* Lab6 riscv64：全课程第一段真正跑在 U-mode 的代码。跟 x86_64 那份
 * （../x86_64/user_prog.S）同一个教学取向——纯汇编，不搭 C 运行环境，
 * 见那份文件顶部注释，原因完全一样，这里不重复。
 *
 * 独立编译/链接（自己的 user_prog.ld，链接到固定地址
 * USER_PROG_VADDR，见 kernel_main.c），产物用 user_blob.S 的 .incbin
 * 整段嵌入内核镜像——流水线跟 x86_64 一侧完全对应，见 Makefile 顶部
 * 注释，riscv64/x86_64 走的是同一套通用规则，不需要单独写一份。
 *
 * 调用约定（本课程自定义，借用 RISC-V SBI 调用约定里"a7=号，
 * a0-a5=参数"这套角色分工——sbi.h 顶部注释已经介绍过 SBI 那一层用的
 * 正是这个约定，这里的系统调用号不是 SBI EID/FID，是本课程自己的
 * syscall.h 数值，但寄存器角色分工借用同一套，理由是"同一个架构下，
 * a7 传'这次 ecall 想干什么'的角色是一致的心智模型，SBI 用它传
 * extension ID，我们的系统调用用它传 syscall 号，同一个寄存器，同一个
 * 角色，不同的数值表——不需要为系统调用另发明一套寄存器分工"）：
 *   a7 = 系统调用号（syscall.h 的 SYS_WRITE/SYS_EXIT）
 *   a0/a1 = 前两个参数
 *   返回值在 a0
 * 跟 x86_64 侧 rax=号、rdi/rsi=参数、结果回 rax 是同一套角色分工在
 * 不同寄存器上的体现,trap.c/trap_entry.S 已经按这个分工写好了内核侧,
 * 用户侧必须对上。 */
.section .text
.global _user_start

_user_start:
    /* sys_write(msg, len)：msg 地址是链接期常量（USER_PROG_VADDR 之后
     * 的偏移），跟 x86_64 侧同一个简化，不需要运行时定位。
     *
     * len 用 "la 两个标签、地址相减" 算出来，不是 `li a1, msg_len`
     * ——后者实测在 as 这里报 `illegal operands`：msg_len 是
     * `.rodata` 段里 `. - msg` 算出来的一个链接期表达式，不是纯汇编期
     * 常量，`li` 伪指令的立即数展开（要在 addi 单条/lui+addi 两条之间
     * 选)要求操作数在汇编阶段就能求值成一个具体数字，这里的 msg_len
     * 直到链接阶段才能真正确定——跟 x86_64 侧 `mov $msg_len, %rsi`
     * 能直接工作是两种汇编器在"立即数表达式几时求值"上的差异，不是
     * 抄错。改用两个 la 分别取地址、sub 相减，把"算长度"这件事完全
     * 挪到运行时用寄存器做，绕开了对汇编期常量折叠的依赖。 */
    lla a0, msg
    lla a1, msg_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE，跟 syscall.h 的 SYS_WRITE 数值必须一致——
              * 用户程序不能 #include 内核头文件，跟 x86_64 侧同一个
              * "手写字面量、两边保持同步"的手工契约，见那份文件对应
              * 位置的注释，README 会一并指出。 */
    ecall

    /* sys_exit(0)：验证 sys_write 返回后用户态代码确实继续往下执行，
     * 再主动退出，跟 x86_64 侧同一个验证目的。 */
    li a0, 0
    li a7, 2 /* SYS_EXIT，同上，必须跟 syscall.h 的 SYS_EXIT 一致。 */
    ecall

    /* 正常流程不会走到这里，见 x86_64 侧 user_prog.S 对应位置的完整
     * 注释——用可控的挂起替换不可控的乱跑。
     *
     * 这里原来写的是 wfi（riscv 版本的"停下来等中断"，对应 x86_64 的
     * hlt），QEMU 里实测触发：wfi 在 U-mode 执行直接变成 illegal
     * instruction 陷入内核（QEMU 打的诊断是 "system_opcode_insn:
     * Invalid opcode for CSR read/write instruction"，scause=2），
     * 根因是 mstatus.TW（Timeout Wait）位——riscv-privileged 规范
     * 原文："When TW=0, the WFI instruction may execute in lower
     * privilege modes... When TW=1, then if WFI is executed in any
     * less-privileged mode... [it traps]"，本 Lab 从未主动设置过
     * mstatus.TW（只在 sstatus 视图里操作过 SPP/SPIE/SUM 这几个跟
     * S-mode 相关的位，TW 是 mstatus 独有、sstatus 视图里根本看不到
     * 的位，没有任何代码路径改过它），QEMU 给出的复位值让它在这里
     * 表现为 1，U-mode 的 wfi 因此永远合法地被拒绝——这跟 x86_64 的
     * hlt 不一样：x86_64 的 hlt 在 ring<0 执行是*无条件* #GP，没有
     * 类似 TW 这样的开关可以让它在用户态合法执行，所以 x86_64 版
     * user_prog.S 从一开始就用忙等 jmp 1b，不是 wfi 的对应物 hlt。
     * riscv64 这边补一次教训：换成同样的忙等循环（j 1b），跟 x86_64
     * 侧保持一致，也是唯一一种不依赖任何特权位状态、在 U-mode 下
     * 保证合法的"卡住不再前进"写法。 */
1:
    j 1b

.section .rodata
msg:
    .ascii "hello from user mode (riscv64, ecall/sret)\n"
msg_end:
