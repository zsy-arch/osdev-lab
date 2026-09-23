/* Lab6 x86_64：全课程第一段真正跑在 ring3/CPL3 的代码。用纯汇编写而不是
 * C，是因为一个能跑在用户态的最小 C 环境（哪怕只是"能编译不需要 libc"）
 * 本身就要解决"怎么不依赖内核符号做重定位""栈从哪来"等一整套问题，那是
 * Lab9（ELF 加载/exec）要教的内容——本 Lab 的教学重点是"特权级切换机制
 * 本身通不通"，不是"怎么给用户程序搭一个可用的 C 运行环境"，纯汇编把
 * 后者完全绕开，只留下前者。
 *
 * 独立编译/链接（自己的 linker.ld，链接到固定地址 USER_PROG_VADDR，见
 * kernel_main.c），产物用 user_blob.S 的 .incbin 整段嵌入内核镜像，
 * kernel_main.c 负责在启动时把这段字节拷贝到映射了 PTE_FLAG_USER 的
 * 物理页——这是本 Lab 避免实现 ELF 解析器的关键，直接省掉。
 *
 * 调用约定（本课程自定义，不是抄 Linux syscall ABI 的数字，只借用它的
 * "rax=号、rdi/rsi=参数、结果回 rax"这个寄存器角色分工——这套角色分工
 * 是 SYSCALL/SYSRET 硬件约定的直接延伸，前面 trap_entry.S 已经按这个
 * 分工写好了内核侧，用户侧必须对上）：
 *   rax = 系统调用号（syscall.h 的 SYS_WRITE/SYS_EXIT）
 *   rdi/rsi = 前两个参数
 *   返回值在 rax
 */
.code64

.section .text
.global _user_start

_user_start:
    /* sys_write(msg, len)：msg 地址是这个文件自己汇编时算出来的链接期
     * 常量（USER_PROG_VADDR 之后的偏移），不需要运行时定位——用户程序
     * 链接到固定地址，跟内核镜像本身"链接到固定地址、不做位置无关"
     * 是同一个简化，Lab9 引入动态加载之后才需要变。 */
    lea msg(%rip), %rdi
    mov $msg_len, %rsi
    mov $1, %rax /* SYS_WRITE，跟 syscall.h 的 SYS_WRITE 数值必须一致
                  * ——用户程序不能 #include 内核头文件（两边是完全独立
                  * 的编译单元，用户程序甚至不是用同一个 Makefile 规则
                  * 编译的），这里手写字面量 1，是本 Lab 唯一一处"两份
                  * 独立代码里的数字必须保持同步"的手工契约，README
                  * 会专门指出这一点。 */
    syscall

    /* sys_exit(0)：验证 sys_write 返回后用户态代码确实继续往下执行
     * （而不是 sysretq 出错但因为某种原因看起来"正常"），再主动退出。 */
    mov $0, %rdi
    mov $2, %rax /* SYS_EXIT，同上，必须跟 syscall.h 的 SYS_EXIT 一致。 */
    syscall

    /* 正常流程 sys_exit 不会返回（sys_exit 是 void，syscall_dispatch
     * 里对 SYS_EXIT 分支返回 0 给 rax，但用户程序在这里完全不检查——
     * exit 之后"还剩下什么要做"这个问题本身就没有意义）。这里放一个
     * 死循环兜底，不是期望会走到，而是避免"如果真的走到了，会执行到
     * 这段代码后面未定义的字节"这种更难诊断的失败模式——用可控的挂起
     * 替换不可控的乱跑。 */
1:
    jmp 1b

.section .rodata
msg:
    .ascii "hello from user mode (x86_64, SYSCALL/SYSRET)\n"
msg_len = . - msg
