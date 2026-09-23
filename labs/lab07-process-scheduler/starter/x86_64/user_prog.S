/* Lab7 x86_64：在 Lab6 的 write+exit 基础上，加两段独立的演示——fork+
 * wait+exit（子进程）和 exec（覆盖自己的地址空间）。用纯汇编写的理由跟
 * Lab6 保持一致，见下面保留的原有说明。
 *
 * 调用约定（本课程自定义，不是抄 Linux syscall ABI 的数字，只借用它的
 * "rax=号、rdi/rsi=参数、结果回 rax"这个寄存器角色分工——这套角色分工
 * 是 SYSCALL/SYSRET 硬件约定的直接延伸，前面 trap_entry.S 已经按这个
 * 分工写好了内核侧，用户侧必须对上）：
 *   rax = 系统调用号（syscall.h 的 SYS_WRITE/SYS_EXIT/SYS_FORK/SYS_EXEC/
 *         SYS_WAIT）
 *   rdi/rsi = 前两个参数
 *   返回值在 rax
 *
 * 本文件为什么不是一个"从头到尾自动跑通 fork+exec+wait"的单一线性脚本：
 * 这个内核只有一份内嵌的用户程序二进制（user_blob.S 的 .incbin），
 * sys_exec() 的语义是"把调用者自己的地址空间重新映射成这份二进制的一份
 * *全新拷贝*，然后把 rip/rsp 重置回 _user_start/栈顶"（sys_exec() 的
 * 大注释、proc.c 顶部模块注释都已经说明：本 Lab 的 exec 只演示"机制
 * 存在"，不支持传参/换成不同程序）——换句话说，任何调用了 exec() 的
 * 进程，"下一世"执行的都是*同一份*_user_start，包括 fork。如果这份
 * 脚本在没有任何额外区分手段的情况下无条件 fork，那么"exec 之后重新
 * 出生的这个进程"会再 fork 一次，形成一条不会停止的进程链——不是本 Lab
 * 想演示的内容，本 Lab 要演示的是"fork/exec/wait/exit 各自的机制都能
 * 正确工作"，不是设计一套自举的多进程程序。
 *
 * 用 rbx 是"第一次运行"还是"exec 之后再次运行"的判定标志——不是猜测：
 * trap_entry.S 的 syscall_entry 从来不 push/pop rbx（换栈前后 rbx 完全
 * 不被 syscall_entry 自己触碰），而 syscall_dispatch()/sys_write()/
 * sys_fork()/sys_wait() 都是普通 C 函数，System V AMD64 调用约定规定
 * rbx 是 callee-saved（被调用者如果要用，必须先保存、返回前恢复），
 * 所以一次 SYSCALL 往返（包括 sys_fork()/sys_wait() 这些会执行不少 C
 * 代码的路径）不会改变用户态 rbx 的值——这跟 proc.h 里"用户程序不依赖
 * 某个通用寄存器的值跨越一次 syscall 存活"这句话并不矛盾：那句话说的是
 * "本课程的用户程序默认不这样做"，不是"CPU/ABI 不允许这样做"，本文件
 * 这一处用法专门写了这段注释说明为什么在 sys_exec() 具体实现（不修改
 * 任何寄存器，只写 *user_rip_slot/*user_rsp_slot 这两个内存位置）之下，
 * 这个判定是可靠的、可验证的，不是脆弱的偶然行为——sys_exec() 本身也
 * 完全不改写用户态寄存器，只通过 rip_slot/rsp_slot 这两个内存位置生效
 * （见 proc.c sys_exec() 实现），所以 exec 前设的 rbx 标志在"重新出生"
 * 之后原样可见。
 *
 * 流程：
 *   if rbx != 0: jmp reborn_branch      // 见下面"关键点"说明，这一句
 *                                       // 必须是 _user_start 的*第一条*
 *                                       // 判断，在 fork() 之前
 *   write("root: forking...")
 *   rax = fork()
 *   if rax == 0:                       // ---- 子进程分支 ----
 *       write("child: running, calling exec...")
 *       mov $1, %rbx                    // 标记"接下来是 exec 之后的世界"
 *       exec()                          // 成功后不返回到这里，rip/rsp 被
 *                                       // 重置回 _user_start，rbx 保持 1
 *       jmp 1b                          // 兜底死循环，正常不会走到
 *   // ---- 父进程分支（rax != 0）----
 *   if rax < 0:
 *       write("root: fork failed")
 *       exit(1)
 *   write("root: waiting for child...")
 *   loop: rc = wait(&code)
 *         if rc < 0: jmp loop            // 本 Lab 的 wait() 不阻塞，找不到
 *                                        // 已退出的子进程就返回-1，需要
 *                                        // 自己重试（见 sys_wait() 注释）
 *   write("root: child reaped, exit_code=...")
 *   exit(0)
 * reborn_branch:
 *   write("child: reborn after exec, exiting")
 *   exit(0)
 *
 * 关键点（本 Lab 实测调试中发现过的真实设计 bug，不是纯理论风险）：
 * reborn 判断必须放在 _user_start 的最前面，在 fork() 之前，不能像
 * 早期版本那样只放在 child_branch 内部——exec 成功之后，"reborn"这条
 * 命运线是从 _user_start 的第一条指令重新执行的，不是从 child_branch
 * 那个标签重新执行；如果 rbx 检查只写在 child_branch 里，reborn 这条
 * 命运线会先跑到顶部那次无条件 fork()，自己也变成一个新的"root"，
 * 生出一个全新的、rbx==0 的子进程，那个子进程本身还会再走一遍
 * child_branch->exec->reborn 这条链——形成本文件顶部模块注释警告过的
 * 那种不会停止的进程链，跟"reborn 之后不会再次进入那个 fork"这句
 * 结论完全矛盾。早期版本的注释已经写对了结论，但落地的指令序列没有
 * 真正实现这个结论（判断的位置错了），这是"写了道理、但代码没有真的
 * 照着道理走"的典型例子——本 Lab 用这个真实教训提醒：控制流断言必须
 * 放在能真正生效的位置，不能只存在于注释里。 */
.code64

.section .text
.global _user_start

_user_start:
    /* 必须是整个文件的第一条判断，在任何 write/fork 之前——见上面模块
     * 注释"关键点"那一段：reborn 命运线从这条指令重新开始执行，如果
     * 这个判断放得靠后（比如只放在 child_branch 内部），reborn 会先
     * 经过顶部的无条件 fork()，重新触发无限进程链。
     *
     * 这里*不*需要先显式 "mov $0, %rbx" 再比较：本文件唯一需要 rbx==0
     * 的地方就是这条判断本身，初值已经由内核保证——proc_alloc_skeleton()
     * 用 kalloc_page() 分配新 struct proc（新分配的物理页保证清零，
     * kalloc.c 已说明），trapframe 里的通用寄存器字段——包括 rbx——
     * 全部起始于 0。所以任何一个全新创建的进程（kernel_main.c 创建的
     * 最初那个，或者 sys_fork() 创建的子进程）第一次执行到这里时 rbx
     * 恒为 0，直接走到下面 fork 流程；exec 之后 reborn 的那次执行不
     * 经过"新建进程"这条路径（sys_exec() 只重置 rip/rsp，不碰任何
     * 寄存器），rbx 保持 exec 之前设的 1，这里直接跳走。 */

    /* TODO 1：判断"这条命运线是不是 exec 之后重新出生的"，是就跳去
     * reborn_branch。这个判断必须是整个文件的第一条指令。
     *
     * 为什么：紧接上面这段注释和模块注释"关键点"一节——这是本文件唯一
     * 一处对位置敏感到"写错了就无限建进程"的代码。上面两段注释已经把
     * 道理讲完了（reborn 从 _user_start 重新开始执行、rbx 的初值由内核
     * 两层清零保证），这个 TODO 要做的就是让指令流真的照着道理走：比较
     * 和跳转两条指令都放在 _user_start 的最前面，前面不放任何别的东西。
     *
     * 提示：
     * cmp $0, %rbx
     * jne reborn_branch
     */

    /* TODO 2：打印 msg_root_fork，然后 fork()，再按 rax 的值分派到三条路
     * ——0 去 child_branch，负数去 root_fork_failed，正数落到下面父进程
     * 分支。
     *
     * 为什么：这是本 Lab 的核心观察点——同一条 syscall 指令之后，两个
     * 进程各自看到不同的 rax，从这里开始走上两条不同的命运线。写字符串
     * 的三行（lea 取地址 / mov 取长度 / mov $1 号）跟 Lab6 完全一样，
     * 调用约定 rax=号、rdi/rsi=参数也跟 Lab6 一致，新东西只有"fork 之后
     * 要按返回值分派"这一件。
     *
     * rax 的三种取值：子进程分支里是 0，父进程分支里是新 pid（>0）或者
     * -1（进程表满，见 sys_fork() 注释）。分支判断必须紧跟在 syscall 后
     * 面，不能有任何会碰 rax 的代码插在中间——注意 write 那三行里 rax
     * 正好被用来传系统调用号，所以顺序上只能先判断、后打印。
     *
     * 提示：
     * lea msg_root_fork(%rip), %rdi
     * mov $msg_root_fork_len, %rsi
     * mov $1, %rax // SYS_WRITE
     * syscall
     *
     * mov $3, %rax // SYS_FORK
     * syscall
     * cmp $0, %rax
     * je child_branch
     * cmp $0, %rax
     * jl root_fork_failed
     */

    /* ---- 父进程（root）分支：等子进程，回收，退出 ---- */

    /* TODO 3：打印 msg_root_wait，然后在 root_wait_retry 这个标签上自旋
     * 调用 wait(&exit_code_slot) 直到回收到子进程，打印 msg_root_reaped，
     * 最后 exit(0)。
     *
     * 为什么：本 Lab 的 sys_wait() 是非阻塞的——没有 ZOMBIE 子进程时直接
     * 返回 -1，而不是把调用者挂起（proc.c sys_wait() 注释说明了这个教学
     * 取舍）。所以"等"这件事必须由用户态自己做成重试循环：wait() 返回
     * 负数就跳回去再问一次。这个自旋循环正是让调度器真正被观察到的地方
     * ——父进程反复 syscall，中间时间片被交给子进程，子进程才有机会跑到
     * exit()，父进程下一次 wait() 才能拿到非负返回值。
     *
     * rdi 每次重试都要重新装一遍 exit_code_slot 的地址，所以那条 lea 要
     * 放在 root_wait_retry 标签之后、不是之前——syscall 之后 rdi 是否
     * 还保持原值不在本 Lab 保证的范围内（模块注释只论证了 rbx 可靠）。
     *
     * 提示：
     * lea msg_root_wait(%rip), %rdi
     * mov $msg_root_wait_len, %rsi
     * mov $1, %rax // SYS_WRITE
     * syscall
     *
     * root_wait_retry:
     * lea exit_code_slot(%rip), %rdi
     * mov $5, %rax // SYS_WAIT
     * syscall
     * cmp $0, %rax
     * jl root_wait_retry // 不阻塞，没找到就自己重试。
     *
     * lea msg_root_reaped(%rip), %rdi
     * mov $msg_root_reaped_len, %rsi
     * mov $1, %rax // SYS_WRITE
     * syscall
     *
     * mov $0, %rdi
     * mov $2, %rax // SYS_EXIT
     * syscall
     * jmp halt_loop
     */

root_fork_failed:
    /* TODO 4：打印 msg_root_fork_failed，然后 exit(1)。
     *
     * 为什么：write+exit 这一段跟 Lab6 的 TODO 1 完全一样，唯一区别是退出
     * 码用 1 而不是 0——fork 失败是异常路径，用非零退出码表示"这次运行没
     * 按预期走完"。这条分支在 NPROC=4 的本 Lab 里正常不会被走到，写它是
     * 为了让"系统调用会失败"这件事在用户程序里有一个显式的落点，而不是
     * 让 -1 被当成正数 pid 继续往下跑。
     *
     * 提示：
     * lea msg_root_fork_failed(%rip), %rdi
     * mov $msg_root_fork_failed_len, %rsi
     * mov $1, %rax // SYS_WRITE
     * syscall
     *
     * mov $1, %rdi
     * mov $2, %rax // SYS_EXIT
     * syscall
     * jmp halt_loop
     */

    /* ---- 子进程分支：一定是刚 fork() 出来的全新子进程，rbx 恒为 0 ----
     * （reborn 命运线已经在 _user_start 顶部被拦截、跳去 reborn_branch，
     * 永远不会走到这条 fork 路径，所以这里不需要、也不应该再判断一次
     * rbx——那是本文件早期版本的位置，属于上面模块注释里说的"判断的
     * 位置错了"那个真实 bug，已经挪到 _user_start 顶部。） */
child_branch:
    /* TODO 5：打印 msg_child_exec，把 rbx 置成 1，然后 exec()。
     *
     * 为什么：rbx = 1 必须写在 syscall 之前，因为 exec 之后这个进程会从
     * _user_start 重新开始执行，而 TODO 1 的判断读的就是这个值——它是
     * 这条命运线留给"下一次的自己"的唯一一条消息。本 Lab 的 exec 重新
     * 加载的是同一份嵌入的 user_prog.bin（proc.c sys_exec() 注释说明了
     * 这个简化），所以"重新出生"的程序跟当前这份是同一段代码，没有这个
     * 标志就无法区分两次执行。用 rbx 而不是别的寄存器，理由见模块注释
     * 里那段"rbx 在一次 syscall 往返里不被内核任何一段代码触碰"的论证。
     *
     * syscall 之后那条 jmp halt_loop 是兜底：正常情况 exec 成功之后不会
     * 执行到那里（rip/rsp 已经被内核改写，下一次从用户态"恢复执行"实际上
     * 是从 _user_start 重新开始，不是从这条 syscall 指令的下一条继续），
     * 但 exec 失败时会原地返回，不能让指令流冲进下面的 reborn_branch。
     *
     * 提示：
     * lea msg_child_exec(%rip), %rdi
     * mov $msg_child_exec_len, %rsi
     * mov $1, %rax // SYS_WRITE
     * syscall
     *
     * mov $1, %rbx // 标记"接下来这条命运线是 exec 之后重新出生的"。
     *
     * mov $4, %rax // SYS_EXEC
     * syscall
     * jmp halt_loop
     */

reborn_branch:
    /* exec 之后重新出生：rbx==1，不再 fork，打印消息后直接退出，
     * 保证这条命运线是单向、必然终止的，不会重新触发顶部注释说的
     * 无限链问题（本文件只有一处 fork，在 rbx==0 的分支里）。 */

    /* TODO 6：打印 msg_reborn，然后 exit(0)。
     *
     * 为什么：这条分支存在的意义就是"单向、必然终止"——它不 fork，只
     * 打印一行证明 exec 真的换掉了地址空间并从头执行，然后退出。这一行
     * 出现在串口输出里，是 exec 生效的唯一可见证据。write+exit 的写法跟
     * Lab6 完全一样。
     *
     * 提示：
     * lea msg_reborn(%rip), %rdi
     * mov $msg_reborn_len, %rsi
     * mov $1, %rax // SYS_WRITE
     * syscall
     *
     * mov $0, %rdi
     * mov $2, %rax // SYS_EXIT
     * syscall
     */

halt_loop:
    /* TODO 7：写一个死循环兜底。
     *
     * 为什么：跟 Lab6 对应位置一样——exit 之后正常不会返回，这条循环是
     * 把"理论上不会发生但万一发生"的情况变成可控挂起，而不是让 CPU 冲进
     * 后面的 .data/.rodata 当指令执行。用 jmp，不要用 hlt：hlt 在 ring 3
     * 执行是无条件 #GP。
     *
     * 提示：
     * jmp halt_loop
     */

/* 已经写好，不是 TODO：下面两个段里的 exit_code_slot 和六条消息直接给出
 * ——上面每个 TODO 都按名字引用它们，长度用 msg_X_len = . - msg_X 在汇编
 * 期算好（跟 Lab6 同一个套路，只是从一条消息变成六条)。数据本身没有新
 * 知识，改动字符串只会让 tests/expect-x86_64.txt 对不上。 */
.section .data
    /* wait() 的 exit_code_out 参数指向的存储位置：用户栈或者 .bss 都
     * 可以，选 .data 是因为这个程序完全没有用到 .bss（没有声明任何
     * 未初始化的全局数据），不值得为了一个 8 字节的槽位再多引入一个
     * 段——跟这份最小化教学代码的取向一致。 */
exit_code_slot:
    .quad 0

.section .rodata
msg_root_fork:
    .ascii "root: forking child\n"
msg_root_fork_len = . - msg_root_fork

msg_root_fork_failed:
    .ascii "root: fork failed\n"
msg_root_fork_failed_len = . - msg_root_fork_failed

msg_root_wait:
    .ascii "root: waiting for child\n"
msg_root_wait_len = . - msg_root_wait

msg_root_reaped:
    .ascii "root: child reaped\n"
msg_root_reaped_len = . - msg_root_reaped

msg_child_exec:
    .ascii "child: running, calling exec\n"
msg_child_exec_len = . - msg_child_exec

msg_reborn:
    .ascii "child: reborn after exec, exiting\n"
msg_reborn_len = . - msg_reborn
