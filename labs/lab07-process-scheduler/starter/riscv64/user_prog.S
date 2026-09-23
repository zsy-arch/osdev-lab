/* Lab7 riscv64：在 Lab6 的 write+exit 基础上，加两段独立的演示——fork+
 * wait+exit（子进程）和 exec（覆盖自己的地址空间）。跟 x86_64 那份
 * （../x86_64/user_prog.S）同一个教学取向、同一个流程设计，见那份文件
 * 顶部注释的完整推理（为什么不是线性脚本、为什么需要一个"reborn"判定
 * 标志、那个真实调试出来的"判断位置放错了会形成无限进程链"的教训），
 * 这里不重复，只说 riscv64 这一侧的寄存器角色分工差异。
 *
 * 调用约定：a7=系统调用号，a0/a1=前两个参数，返回值在 a0——跟 Lab6
 * user_prog.S 顶部注释、trap.c/trap_entry.S 已经写好的内核侧分工一致。
 *
 * reborn 判定标志用 s0，不是照抄 x86_64 侧的 rbx，是因为寄存器名字
 * 本身在两个架构下不存在对应关系，需要各自验证"这个寄存器在一次 ecall
 * 往返里是否真的不被内核任何一段代码触碰"——riscv64 这边验证过：
 * trap_entry.S 的 supervisor_trap_entry/from_supervisor 用的是 Lab4/5/6
 * 就有的 128 字节轻量栈帧（ra/t0-t6/a0-a7,16 个字段，trap_entry.S 105-
 * 168 行的 sd/ld 序列),从头到尾没有 sd/ld 过任何一个 s0-s11；trap.c 的
 * ecall_handler(uint64_t *frame) 只读写 frame[8]/frame[9]/frame[15]
 * （对应 a0/a1/a7),同样不碰 s 寄存器。riscv64 calling convention 规定
 * s0-s11 是 callee-saved，sys_fork()/sys_exec()/sys_wait() 这些普通 C
 * 函数如果内部用到 s0，返回前必须恢复原值——两层保证叠在一起，s0 在
 * 一次 ecall 往返前后的用户态可见值不会变，跟 x86_64 侧证明 rbx 可靠
 * 的论证结构完全对应，只是换了一套具体的寄存器名和栈帧字段来源。
 *
 * 跟 x86_64 侧 user_prog.S 一处刻意不同：这里的 SYS_WAIT 不传
 * exit_code_out 指针，也不打印 exit_code——riscv64 trap.c 的
 * syscall_dispatch() 对 SYS_WAIT 硬编码 sys_wait(NULL)（trap.c 155-
 * 168 行注释已经说明：ecall 路径的参数只有 a0/a1，SYS_WAIT 语义上不
 * 需要输入参数，没有槽位能装"结果写到哪"这个额外的输出指针，本 Lab
 * 选择不提供这条路径，只暴露 sys_wait() 的返回值）。本文件如果像
 * x86_64 侧那样传一个 exit_code_slot 地址进去，这个地址会被
 * syscall_dispatch() 直接丢弃，槽位永远读到初值 0——为了不写出一段
 * "看起来生效、实际是死代码"的用户程序，这里直接不传这个参数，只用
 * 返回值判断"有没有回收到子进程"。 */
.section .text
.global _user_start

_user_start:
    /* TODO 1：判断"这条命运线是不是 exec 之后重新出生的"，是就跳去
     * reborn_branch。这个判断必须是整个文件的第一条指令，在任何
     * write/fork 之前。
     *
     * 为什么：理由跟 x86_64 侧模块注释"关键点"一节完全一样——reborn
     * 命运线从这条指令重新开始执行（sys_exec() 只重置 sepc/sp，不碰任何
     * 通用寄存器，见 proc.c sys_exec() 实现），如果这个判断放得靠后，
     * reborn 会先经过顶部的无条件 fork()，重新触发无限进程链。
     *
     * 不需要先显式清零 s0 再比较：新创建的进程（kernel_main.c 创建的
     * 最初那个，或者 sys_fork() 创建的子进程）trapframe 里的通用寄存器
     * 字段全部起始于 0——proc_alloc_skeleton() 用 kalloc_page() 分配
     * 新 struct proc，物理页保证清零（kalloc.c 已说明），随后
     * memset(p->tf, 0, sizeof(*p->tf)) 显式清零一次 trapframe 本身——
     * s0 在两层清零下恒为 0，直接走到下面 fork 流程；exec 之后 reborn
     * 的那次执行不经过"新建进程"这条路径，s0 保持 exec 之前设的 1，
     * 这里直接跳走。
     *
     * 提示：
     * li t0, 0
     * bne s0, t0, reborn_branch
     */

    /* TODO 2：打印 msg_root_fork，然后 fork()，再按 a0 的值分派到三条路
     * ——0 去 child_branch，负数去 root_fork_failed，正数落到下面父进程
     * 分支。
     *
     * 为什么：这是本 Lab 的核心观察点——同一条 ecall 指令之后，两个进程
     * 各自看到不同的 a0，从这里开始走上两条不同的命运线。写字符串的三行
     * （la/la/sub 算长度）跟 Lab6 完全一样，调用约定 a7=号、a0/a1=参数
     * 也跟 Lab6 一致，新东西只有"fork 之后要按返回值分派"这一件。
     *
     * a0 的三种取值：子进程分支里是 0，父进程分支里是新 pid（>0）或者
     * -1（进程表满，见 sys_fork() 注释）。分支判断必须紧跟在 ecall 后面，
     * 不能有任何会碰 a0 的代码插在中间——注意算字符串长度的那三行正好会
     * 写 a0/a1，所以顺序上只能先判断、后打印。
     *
     * 提示：
     * la a0, msg_root_fork
     * la a1, msg_root_fork_end
     * sub a1, a1, a0
     * li a7, 1 // SYS_WRITE
     * ecall
     *
     * li a7, 3 // SYS_FORK
     * ecall
     * li t0, 0
     * beq a0, t0, child_branch
     * blt a0, t0, root_fork_failed
     */

    /* ---- 父进程（root）分支：等子进程，回收，退出 ---- */

    /* TODO 3：打印 msg_root_wait，然后在 root_wait_retry 这个标签上自旋
     * 调用 wait() 直到回收到子进程，打印 msg_root_reaped，最后 exit(0)。
     *
     * 为什么：本 Lab 的 sys_wait() 是非阻塞的——没有 ZOMBIE 子进程时直接
     * 返回 -1，而不是把调用者挂起（proc.c sys_wait() 注释说明了这个教学
     * 取舍）。所以"等"这件事必须由用户态自己做成重试循环：wait() 返回
     * 负数就跳回去再问一次。这个自旋循环正是让调度器真正被观察到的地方
     * ——父进程反复 ecall，中间时间片被交给子进程，子进程才有机会跑到
     * exit()，父进程下一次 wait() 才能拿到非负返回值。
     *
     * a0 不需要预置——SYS_WAIT 不读输入参数，见上面模块注释"刻意不同"
     * 一节，这里不传 exit_code_out 地址。
     *
     * 提示：
     * la a0, msg_root_wait
     * la a1, msg_root_wait_end
     * sub a1, a1, a0
     * li a7, 1 // SYS_WRITE
     * ecall
     *
     * root_wait_retry:
     * li a7, 5 // SYS_WAIT
     * ecall
     * li t0, 0
     * blt a0, t0, root_wait_retry // 不阻塞，没找到就自己重试。
     *
     * la a0, msg_root_reaped
     * la a1, msg_root_reaped_end
     * sub a1, a1, a0
     * li a7, 1 // SYS_WRITE
     * ecall
     *
     * li a0, 0
     * li a7, 2 // SYS_EXIT
     * ecall
     * j halt_loop
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
     * la a0, msg_root_fork_failed
     * la a1, msg_root_fork_failed_end
     * sub a1, a1, a0
     * li a7, 1 // SYS_WRITE
     * ecall
     *
     * li a0, 1
     * li a7, 2 // SYS_EXIT
     * ecall
     * j halt_loop
     */

    /* ---- 子进程分支：一定是刚 fork() 出来的全新子进程，s0 恒为 0 ----
     * （reborn 命运线已经在 _user_start 顶部被拦截、跳去 reborn_branch，
     * 永远不会走到这条 fork 路径，这里不需要、也不应该再判断一次 s0，
     * 跟 x86_64 侧同一处注释一样，避免重复一个已经在上面处理过的判断，
     * 也避免出现"判断位置错了"那个真实教训对应的写法。） */
child_branch:
    /* TODO 5：打印 msg_child_exec，把 s0 置成 1，然后 exec()。
     *
     * 为什么：s0 = 1 必须写在 ecall 之前，因为 exec 之后这个进程会从
     * _user_start 重新开始执行，而 TODO 1 的判断读的就是这个值——它是
     * 这条命运线留给"下一次的自己"的唯一一条消息。本 Lab 的 exec 重新
     * 加载的是同一份嵌入的 user_prog.bin（proc.c sys_exec() 注释说明了
     * 这个简化），所以"重新出生"的程序跟当前这份是同一段代码，没有这个
     * 标志就无法区分两次执行。
     *
     * ecall 之后那条 j halt_loop 是兜底：正常情况 exec 成功之后不会执行
     * 到那里（sepc/sp 已经被内核改写，下一次从用户态"恢复执行"实际上是
     * 从 _user_start 重新开始，不是从这条 ecall 指令的下一条继续），但
     * exec 失败时会原地返回，不能让指令流冲进下面的 reborn_branch。
     *
     * 提示：
     * la a0, msg_child_exec
     * la a1, msg_child_exec_end
     * sub a1, a1, a0
     * li a7, 1 // SYS_WRITE
     * ecall
     *
     * li s0, 1 // 标记"接下来这条命运线是 exec 之后重新出生的"。
     *
     * li a7, 4 // SYS_EXEC
     * ecall
     * j halt_loop
     */

reborn_branch:
    /* exec 之后重新出生：s0==1，不再 fork，打印消息后直接退出，保证
     * 这条命运线单向、必然终止，不会重新触发顶部模块注释说的无限链
     * 问题（本文件只有一处 fork，在 s0==0 的分支里）。 */

    /* TODO 6：打印 msg_reborn，然后 exit(0)。
     *
     * 为什么：这条分支存在的意义就是"单向、必然终止"——它不 fork，只
     * 打印一行证明 exec 真的换掉了地址空间并从头执行，然后退出。这一行
     * 出现在串口输出里，是 exec 生效的唯一可见证据。write+exit 的写法跟
     * Lab6 完全一样。
     *
     * 提示：
     * la a0, msg_reborn
     * la a1, msg_reborn_end
     * sub a1, a1, a0
     * li a7, 1 // SYS_WRITE
     * ecall
     *
     * li a0, 0
     * li a7, 2 // SYS_EXIT
     * ecall
     */

halt_loop:
    /* TODO 7：写一个死循环兜底，用 j（不是 wfi）。
     *
     * 为什么：跟 Lab6 的 TODO 2 完全一样——wfi 在用户态会触发非法指令
     * 异常，而这里需要的只是"exit 之后万一真的返回了，别让 CPU 冲进
     * 后面的 .rodata 当指令执行"。
     *
     * 提示：
     * j halt_loop
     */

/* 已经写好，不是 TODO：六条消息的内容和 msg_X/msg_X_end 成对的写法直接
 * 给出——上面每个 TODO 都按名字引用它们，长度在使用处用
 * la a0,msg / la a1,msg_end / sub a1,a1,a0 算出来（跟 Lab6 同一个套路，
 * 只是从一条消息变成六条)。字符串本身没有新知识，改动它只会让
 * tests/expect-riscv64.txt 对不上。 */
.section .rodata
msg_root_fork:
    .ascii "root: forking child\n"
msg_root_fork_end:

msg_root_fork_failed:
    .ascii "root: fork failed\n"
msg_root_fork_failed_end:

msg_root_wait:
    .ascii "root: waiting for child\n"
msg_root_wait_end:

msg_root_reaped:
    .ascii "root: child reaped\n"
msg_root_reaped_end:

msg_child_exec:
    .ascii "child: running, calling exec\n"
msg_child_exec_end:

msg_reborn:
    .ascii "child: reborn after exec, exiting\n"
msg_reborn_end:
