/* Lab9：x86_64 用户态系统调用桩。
 *
 * user.h 里声明的 10 个系统调用包装函数，在这里用汇编实现。每一个都只有
 * 三条指令：把调用号放进 rax，执行 syscall，返回。
 *
 * 为什么是汇编而不是 C
 * --------------------
 * 因为 C 里没有"执行 syscall 指令"这个表达式。能写的只有内联汇编，而内联
 * 汇编要正确地告诉编译器"rcx 和 r11 被这条指令破坏了"、"返回值在 rax
 * 里"，写出来的约束串（clobber list）比这三行汇编本身长。真实的 libc
 * 确实用内联汇编（glibc 的 sysdep.h 里那一大堆宏），因为它要为 6 个参数
 * 数量 × 多个架构生成代码，宏比手写划算；我们只有 10 个固定的函数，手写
 * 更清楚。
 *
 * 为什么不需要保存/恢复任何寄存器
 * ------------------------------
 * 这是本文件最值得确认的一点，它依赖三个事实的叠加：
 *
 *   1. 参数已经在正确的位置上了。System V AMD64 C 调用约定把前 6 个整型
 *      参数放在 rdi, rsi, rdx, rcx, r8, r9；而我们内核的 syscall 约定
 *      （见 trap_entry.S 的 syscall_entry）取的是 rdi/rsi/rdx 这前三个。
 *      两者前三个完全重合，所以 write(fd, buf, n) 编译出的 call 已经把
 *      fd/buf/n 放在了内核要读的地方——桩函数一个参数都不用搬。
 *
 *      这不是巧合，是 Linux 刻意的设计：让系统调用参数寄存器尽量贴合 C
 *      调用约定，包装函数才能退化成"设置 rax + syscall"。唯一的错位是
 *      第 4 个参数：C 用 rcx，而 syscall 指令会把返回地址写进 rcx，所以
 *      Linux 的第 4 个系统调用参数改用 r10。我们的系统调用最多 3 个参数
 *      （见 syscall_dispatch 的签名），刚好没碰到这个错位——但知道它存在
 *      很重要，将来加第 4 个参数时这里就要多一条 mov %rcx, %r10。
 *
 *   2. syscall 指令只破坏 rcx 和 r11（分别存放返回后的 rip 和 rflags）,
 *      而这两个在 C 调用约定里都是 caller-saved（调用者保存）。也就是说
 *      调用 write() 的那段 C 代码本来就不能假设 rcx/r11 在 write() 返回
 *      后还有效——编译器早就按这个前提生成代码了。所以破坏它们是合法的,
 *      不需要我们保存。
 *
 *      反过来看：如果 syscall 破坏的是 rbx 或 r12（callee-saved），这里
 *      就必须 push/pop 了。
 *
 *   3. 返回地址在栈上，不在寄存器里。桩函数是被 call 进来的，返回地址被
 *      push 到了用户栈；syscall 往 rcx 里写的"返回后的 rip"是内核 sysretq
 *      用的，跟我们的 ret 无关。所以 syscall 之后直接 ret 就回到了调用者。
 *
 *      这一点在 riscv64 上完全不同：那边函数返回地址在 ra 寄存器里，而
 *      ecall 进内核后内核要用 ra 干活，所以 ra 必须被内核的陷入帧保存下
 *      来。对照 usys_riscv64.S 里的说明。
 *
 * 返回值
 * ------
 * 内核的 syscall_dispatch 返回 int64_t，按 C 调用约定放在 rax 里；sysretq
 * 不改 rax，所以用户态 ret 之后 rax 里就是返回值，正好也是 C 调用约定放
 * 返回值的地方。又一次"因为贴合所以什么都不用做"。
 *
 * 注意 user.h 里这些函数声明的返回类型是 int（32 位）而不是 int64_t。这
 * 是刻意的：fd、字节数、pid 这些值都不需要 64 位，而 int 读起来更像
 * 熟悉的 POSIX 接口。编译器会只看 eax（rax 的低 32 位），高 32 位被忽略
 * ——内核返回的负数错误码（-1 = 0xFFFFFFFFFFFFFFFF）截断成 32 位仍然是
 * -1，符号是对的。
 */

/* 每个桩的样板。用宏而不是复制 10 遍，是因为 10 份三行代码里任何一处
 * 打错（比如调用号写重了）都极难发现——症状是"调用 open 却执行了 close"。
 * 宏让调用号只出现一次，而且跟 syscall.h 里的名字对得上，肉眼就能核对。
 *
 * 这里不能 #include "../syscall.h" 之后用 SYS_WRITE 这样的宏吗？可以，而且
 * 更好——汇编器（走的是 gcc 驱动，会先跑预处理器，所以文件名用 .S 大写）
 * 认得 #include 和 #define。下面就是这么做的。 */
#include "../../syscall.h"

.section .text

/* name = 函数名，num = 系统调用号 */
#define SYSCALL_STUB(name, num) \
    .globl name;                \
    .type name, @function;      \
name:                           \
    movq $num, %rax;            \
    syscall;                    \
    ret;                        \
    .size name, . - name

SYSCALL_STUB(write, SYS_WRITE)
SYSCALL_STUB(exit,  SYS_EXIT)
SYSCALL_STUB(fork,  SYS_FORK)
SYSCALL_STUB(exec,  SYS_EXEC)

/* wait 不能用上面的宏——这里踩过一个真实 bug，留档。
 *
 * user.h 把它声明成 int wait(void)：零参数。但 SYSCALL_STUB 展开出来的
 * 桩从不碰 %rdi，只是 movq $num,%rax; syscall; ret。内核这边
 * sys_wait(int64_t *exit_code_out) 读的是第一个系统调用参数（trap.c
 * syscall_dispatch 里 a0，映射自用户态的 %rdi），并且只判断它是不是
 * NULL：exit_code_out != NULL 才会写 *exit_code_out。
 *
 * 问题就在这句"判断是不是 NULL"上——它的前提是调用者要跳过这个功能时
 * 会传 NULL（也就是 0）。可是 C 原型是 void，调用点根本不会往 %rdi 里
 * 塞任何值,%rdi 里留的是*上一次*那个寄存器被写过之后剩下的旧内容。
 *
 * 真实症状：sh.c 的管道分支背靠背调用两次 wait()（等管道两端的子进程,
 * 见 sh.c "两个孩子，wait 两次" 那段注释）。两次调用之间只有
 * close(fds[0])/close(fds[1])，而 close 的桩同样不碰 %rdi——于是 %rdi
 * 从更早的某次调用（很可能是前面 close(fd) 调用时 fd 参数留下的值,
 * 或者更早的调用链条留下的某个内核态地址，两次实测抓到的都是形如
 * 0xffffffff801xxxxx 这样的内核虚拟地址）原样传进第二次 wait()，内核
 * 那句 *exit_code_out = p->exit_code 就变成了往一个跟"退出码"毫无关系
 * 的地址上写 8 个字节。命中的地址恰好落在已映射、可写的内核数据页时,
 * 后果是静默数据损坏；命中未映射页时是这里实测到的内核态 page fault
 * （error_code=2, not-present+write+kernel），GDB 断在 page_fault_stub
 * 读硬件压的 IRET 帧拿到故障 RIP，info symbol 定位到 sys_wait 内部那句
 * mov %rdx,(%rax) ——同一类"调用者手上的寄存器/指针在函数返回后已经
 * 不可信，但契约双方都没把这一点当真"的 bug，这次出在系统调用 ABI
 * 边界上而不是地址空间生命周期上。
 *
 * 修法：既然 C 原型承诺零参数，桩就必须真的传一个内核认得的"我没有
 * 参数"值——也就是显式把 %rdi 清零，而不是依赖调用点或者上一次系统
 * 调用碰巧留下 0。这比在内核侧加指针校验更贴合本文件一路强调的设计
 * 取舍（sys_write/sys_read/sys_wait 全都不做用户指针校验，见
 * syscall_dispatch 里的说明）：契约是"NULL 表示不要"，桩的职责是让
 * NULL 真正被传递，不是让内核多做一层它明确选择不做的校验。 */
.globl wait
.type wait, @function
wait:
    xor %edi, %edi
    movq $SYS_WAIT, %rax
    syscall
    ret
    .size wait, . - wait

SYSCALL_STUB(open,  SYS_OPEN)
SYSCALL_STUB(read,  SYS_READ)
SYSCALL_STUB(close, SYS_CLOSE)
SYSCALL_STUB(pipe,  SYS_PIPE)
SYSCALL_STUB(dup,   SYS_DUP)

/* exit 用 SYSCALL_STUB 生成会带一条永远执行不到的 ret ——因为 sys_exit
 * 不返回。留着它没有坏处（2 个字节的死代码），而且比为 exit 单独写一份
 * 特例更省心。
 *
 * 但 user.h 里给 exit 加了 __attribute__((noreturn))，这是给*调用方*的
 * 承诺，跟这里的死 ret 不矛盾：编译器据此知道 exit() 之后的代码不可达,
 * 于是 main 末尾不用生成返回序列，写在 exit() 之后的语句会收到告警。
 * 假如内核实现有 bug 让 sys_exit 真的返回了，这条 ret 会让它悄悄地退回
 * 到调用点，而调用点的代码已经被编译器按"不可达"优化掉了——行为不可
 * 预测。这类"用 noreturn 承诺了但实现没兑现"的 bug 很隐蔽，是本 Lab
 * 常见坑一节里会提到的一种。 */

/* 关于 .note.GNU-stack：新版 binutils 对没有显式声明栈属性的 .o 会警告
 * "missing .note.GNU-stack section implies executable stack"。内核那边的
 * .S 文件在 toolchain.mk 里靠 -Wl,-z,noexecstack 之类的方式处理，用户
 * 程序这边直接在源码里声明，更省事也更明确：这个目标文件不需要可执行栈。 */
.section .note.GNU-stack, "", @progbits
