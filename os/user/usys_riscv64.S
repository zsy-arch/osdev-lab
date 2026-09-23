/* Lab9：riscv64 用户态系统调用桩。
 *
 * 跟 usys_x86_64.S 一一对应的另一半：10 个函数，每个三条指令，把调用号
 * 放进 a7，执行 ecall，返回。
 *
 * 跟 x86_64 的对照
 * ----------------
 * 两边的桩长得几乎一样，但"为什么可以这么短"的理由不同，值得逐条比：
 *
 *   参数位置：RISC-V 的 C 调用约定把前 8 个整型参数放在 a0~a7。我们内核
 *   的约定是 a7 放调用号、a0/a1/a2 放三个参数（见 trap.c 的
 *   syscall_dispatch 调用处）。前三个参数位置跟 C 约定完全重合，所以跟
 *   x86_64 一样，桩函数不用搬参数。
 *
 *   但错位的地方不同：x86_64 的第 4 个参数撞上 rcx（被 syscall 指令占用），
 *   riscv64 是第 8 个参数撞上 a7（被调用号占用）。我们最多 3 个参数，两边
 *   都没碰到。真实 Linux/riscv 也是用 a7 放调用号、a0~a5 放 6 个参数,
 *   刚好卡在 a7 之前。
 *
 *   返回地址：这是本质区别。x86_64 的 call 把返回地址 push 到栈上，所以
 *   进内核和出内核都不影响它；riscv64 的 jal/call 把返回地址写进 ra
 *   寄存器，而内核陷入后立刻就要用 ra（call supervisor_trap_handler 会
 *   覆盖它）。
 *
 *   所以这里的 `ret`（= jr ra）能正确工作，完全依赖内核陷入帧把 ra 保存
 *   并恢复了。翻 trap_entry.S 能看到 ra 存在帧偏移 0：
 *
 *       sd ra, 0(sp)      // 进入时
 *       ...
 *       ld ra, 0(sp)      // 退出时
 *
 *   这不是可选的优化，是"用户程序能用普通函数调用发起系统调用"的前提。
 *   如果内核忘了保存 ra，症状是：ecall 本身工作正常、内核也正确返回了,
 *   但用户程序的 ret 跳到内核 trap handler 里某个残留地址上，立刻缺页。
 *   而且因为 ecall 之前一切正常，第一反应总是怀疑系统调用实现错了，很难
 *   想到是"返回路径上少存了一个寄存器"。
 *
 *   x86_64 那边没有这个依赖，这是本 Lab 里"同一件事两个架构的必要条件
 *   不同"最干净的一个例子：把返回地址放栈上还是放寄存器里，这个看起来
 *   纯风格的 ISA 选择，决定了陷入帧必须保存什么。
 *
 * 为什么 t0/t1 不在担心范围内
 * --------------------------
 * ecall 指令本身不破坏任何通用寄存器（对比 x86_64 的 syscall 会写 rcx
 * 和 r11）。所以理论上用户态所有寄存器跨 ecall 都应该保持不变。
 *
 * 实践上取决于内核的陷入帧存了哪些。我们内核保存 ra, t0~t6, a0~a7 ——
 * 也就是全部 caller-saved 寄存器（临时寄存器 t* 和参数寄存器 a*），加上
 * ra。callee-saved 的 s0~s11 和 sp 不在帧里，因为内核 C 代码本身会遵守
 * 调用约定保存它们（这跟 Lab7/Lab8 手写的用户程序把跨 ecall 的值放 s0/s1
 * 是同一个道理）。
 *
 * 换句话说：ecall 在用户程序看来表现得像一次普通的 C 函数调用——caller-
 * saved 寄存器可能变、callee-saved 一定不变。而这正是编译器在 call 一个
 * 外部函数时的假设。所以 GCC 编译出的用户程序调用这些桩函数，天然是安全的。
 *
 * 注意这句话里藏着一个对内核的要求：帧必须覆盖*全部* caller-saved 寄存器。
 * 如果漏了一个（比如 t3），bug 只在"用户程序恰好把活跃值放在 t3 里，且
 * 恰好跨了一次 ecall"时出现。手写汇编的用户程序几乎不会用到 t3，所以
 * Lab7/Lab8 不会暴露；Lab9 开始用 GCC 编译用户程序，寄存器分配由编译器
 * 说了算，t3 随时可能被用上。这是本 Lab 必须把陷入帧检查一遍的原因。
 */
#include "../syscall.h"

.section .text

/* 跟 x86_64 版同构的宏。li 是伪指令，对小立即数展开成一条 addi。 */
#define SYSCALL_STUB(name, num) \
    .globl name;                \
    .type name, @function;      \
name:                           \
    li a7, num;                 \
    ecall;                      \
    ret;                        \
    .size name, . - name

SYSCALL_STUB(write, SYS_WRITE)
SYSCALL_STUB(exit,  SYS_EXIT)
SYSCALL_STUB(fork,  SYS_FORK)
SYSCALL_STUB(exec,  SYS_EXEC)

/* wait 不能用上面的宏——跟 x86_64 版同一个真实 bug，这里的修法跟那边
 * 逐字对应，完整现象、排查过程见 usys_x86_64.S 同一处的大注释，不重复
 * 展开。一句话版本：user.h 把它声明成 int wait(void)，调用点不会往 a0
 * 里塞任何值,SYSCALL_STUB 的桩也从不碰 a0,于是内核 sys_wait() 那句
 * "exit_code_out != NULL 才写"的判断，读到的是上一次某个系统调用留在
 * a0 里的旧内容，而不是调用者真正想传的"我不关心退出码"。 */
.globl wait
.type wait, @function
wait:
    li a0, 0
    li a7, SYS_WAIT
    ecall
    ret
    .size wait, . - wait

SYSCALL_STUB(open,  SYS_OPEN)
SYSCALL_STUB(read,  SYS_READ)
SYSCALL_STUB(close, SYS_CLOSE)
SYSCALL_STUB(pipe,  SYS_PIPE)
SYSCALL_STUB(dup,   SYS_DUP)

/* 返回值在 a0 里：内核的 syscall_dispatch 返回 int64_t（RISC-V 调用约定
 * 放 a0），trap_entry.S 的退出路径把 a0 从帧里恢复给用户态——注意是恢复
 * 成*内核写进帧的那个值*，而不是用户态原来的 a0。trap.c 里那行
 *
 *     frame[8] = (uint64_t)ret;   // a0 槽
 *
 * 就是干这个的。这是陷入帧的一个不太直观的用法：它主要是"保存-恢复"，但
 * a0 那一槽被故意改写，用来传递返回值出去。 */

.section .note.GNU-stack, "", @progbits
