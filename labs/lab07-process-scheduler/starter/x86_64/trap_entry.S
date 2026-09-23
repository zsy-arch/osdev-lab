/* Lab6/7 x86_64：page_fault_stub/timer_stub 原样保留自 Lab5（SYSCALL 是
 * 独立于 IDT 的第三条陷入路径，不影响前两条，见 trap.c 顶部注释）。
 * syscall_entry 是 Lab6 新增的——SYSCALL 指令触发时 CPU 直接跳到
 * IA32_LSTAR 里存的这个地址，不经过 IDT，也不像 IDT 中断门那样自动压栈
 * 保存现场：硬件只做了这几件事（AMD64 手册 SYSCALL 一节明确写出来的，
 * 不是本课程猜测）——
 *   RCX  = 用户态 RIP（SYSRET 用它恢复）
 *   R11  = 用户态 RFLAGS（SYSRET 用它恢复）
 *   RFLAGS = RFLAGS AND NOT(IA32_FMASK)（trap.c 里配置成只清 IF）
 *   CS/SS = 按 IA32_STAR[47:32] 算出的内核段选择子
 *   RIP  = IA32_LSTAR
 * %rsp 完全不变（还是用户栈指针）——这是 SYSCALL 跟 IDT 陷入路径最大的
 * 区别，必须手动换栈，见下面的注释。
 *
 * Lab7 对本文件的改动全部集中在 syscall_entry 里，一共两处，都跟"现在
 * 有多个进程了"直接相关：
 *   1. 换栈目标从 Lab6 的 `lea __stack_top(%rip), %rsp`（链接期固定的
 *      全局唯一内核栈）改成 `mov syscall_kernel_rsp(%rip), %rsp`（运行期
 *      由 scheduler() 写入的、当前进程自己的内核栈顶）。
 *   2. 多算两个参数（user_rip_slot/user_rsp_slot 的地址）传给
 *      syscall_dispatch，供 sys_fork/sys_exec 读写"这次 syscall 返回后
 *      RIP/RSP 应该是什么"。
 * page_fault_stub/timer_stub 逐字节不变——它们走 IDT 路径，换栈由硬件
 * 通过 TSS.RSP0 完成（trap.c 的 tss_set_rsp0()），不需要本文件操心。
 */
.code64

.extern syscall_dispatch
.extern syscall_saved_user_rsp
.extern syscall_kernel_rsp
.extern page_fault_handler
.extern timer_interrupt_handler
.global page_fault_stub
.global timer_stub
.global syscall_entry
.global enter_user_mode

/* 已经写好，不是 TODO：page_fault_stub/timer_stub 跟 Lab5/Lab6 逐字节
 * 一样。这两条走 IDT 中断门，跨特权级时硬件自己会从 TSS.RSP0 取新栈顶，
 * 不需要在汇编里手动换栈——这正是 Lab7 引入 TSS 之后"IDT 路径不用改一个
 * 字节、SYSCALL 路径必须自己管栈"这个对比的具体体现。 */
page_fault_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov 120(%rsp), %rdi
    call page_fault_handler

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $8, %rsp
    iretq

timer_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    call timer_interrupt_handler

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    iretq

/* TODO 1：实现 syscall_entry。
 *
 * Lab6 已经写过它，Lab7 在两处改动（见本文件顶部模块注释的总结）。整体
 * 骨架跟 Lab6 一样，先回忆一遍前提：
 *
 * 进来的时候 %rsp 还是用户栈，%rcx=用户RIP，%r11=用户RFLAGS，%rax=系统
 * 调用号，%rdi/%rsi=参数（用户程序按这个约定填好，见 user_prog.S）——这些
 * 约定寄存器在换栈之前都不能被破坏，换栈本身（mov %rsp 到全局变量、再换
 * 成内核栈）不涉及它们，天然安全。
 *
 * 换栈用全局变量 syscall_saved_user_rsp 暂存用户 RSP，不是压栈——这一刻
 * %rsp 还是用户栈指针，push 会把内核数据写进用户栈（用户栈的内容对内核
 * 不可信，且本 Lab 的用户栈空间本来就很小）。全局变量在切栈动作之前就能
 * 写入，不依赖任何栈的存在。
 *
 * 步骤（★ 标记的是 Lab7 相对 Lab6 的改动）：
 *   1. ★ 换栈：暂存用户 RSP 之后，换到 syscall_kernel_rsp 指向的栈顶
 *      （Lab6 是 `lea __stack_top(%rip), %rsp`）。为什么必须改：Lab7 的
 *      scheduler() 本身跑在 __stack_top 那个栈上，它 swtch() 进某个进程
 *      时，自己的现场（rbx/rbp/r12-r15+返回地址）就保存在那个栈的某个
 *      深度上；如果这个进程接着 SYSCALL 进来又把 %rsp 重置回 __stack_top
 *      这个固定地址，随后的调用链（syscall_dispatch→sys_exit_proc→swtch）
 *      一旦压到那个深度，就会覆盖 scheduler() 保存的返回地址，swtch()
 *      "跳回调度器"时 ret 到垃圾地址。完整推导见 trap.c 里
 *      syscall_kernel_rsp 定义处的大注释。
 *   2. 保存 rcx（用户 RIP）/r11（用户 RFLAGS）——SYSRET 唯一会读的两个
 *      寄存器，而 syscall_dispatch 是普通 C 函数，System V AMD64 约定里
 *      rcx/r11 都是 callee-clobbered，函数体内部完全可能拿它们当临时
 *      寄存器。保存下来是唯一不依赖"这个函数恰好没用到 rcx"的做法。
 *   3. ★ 算出第 4/5 个参数：user_rip_slot（刚 push 在栈上的那个 rcx
 *      槽位）的*地址*，和 user_rsp_slot（syscall_saved_user_rsp 这个
 *      全局变量本身）的*地址*。
 *      传地址不传值，是因为 sys_exec() 需要能*修改*"这次 syscall 返回后
 *      恢复到哪个 RIP/RSP"；只传值的话它改的是自己栈帧上的一份拷贝，
 *      下面的 `pop %rcx` / `mov syscall_saved_user_rsp(%rip), %rsp` 读不
 *      到。sys_fork() 不需要写它们（子进程走独立的另一份 trapframe，父
 *      进程这次 syscall 正常返回），但统一成同一种签名更简单。
 *      `lea 8(%rsp), %r8`：此刻栈顶是 r11（最后 push 的），往下 8 字节
 *      就是 rcx。这条 lea 在 call 之前执行，算的是绝对地址，callee 内部
 *      %rsp 怎么变都不影响它。
 *   4. ★ 参数搬移：num=rax→rdi，a0=rdi→rsi，a1=rsi→rdx（顺序必须
 *      从后往前挪，否则中间值被自己覆盖——这条 Lab6 已经讲过），再把两个
 *      slot 地址搬到第 4/5 参数位 rcx/r8。
 *      注意第四参数位本来是 rcx，但 rcx 此刻还压在栈上、寄存器里存的是
 *      旧的用户 RIP；把 user_rip_slot 的地址搬进 rcx 没问题，因为旧值
 *      已经安全留在栈上了。
 *      ⚠ 这里最容易漏的一条是 `mov %r9, %r8`——把 r8 让给 rcx 之后，
 *      真正的 user_rsp_slot 地址还在 r9 里，必须再搬进 r8。漏掉它，
 *      r8 会保持刚搬进 rcx 的那个旧值不变（r8 == rcx，两个寄存器指向
 *      *同一个*栈上槽位），于是 sys_exec() 里 `*user_rip_slot = ...`
 *      和 `*user_rsp_slot = ...` 写的是同一处内存，后一句把前一句刚写
 *      对的 USER_PROG_VADDR 覆盖成 user_stack_top，而
 *      syscall_saved_user_rsp 完全没被更新。返回时 sysretq 的现场变成
 *      RIP=0x402000（USER_STACK_VADDR 本身，一个没映射过的守护地址）、
 *      RSP=过期的旧值，取指立刻 #PF（not-present,read,user——取指在
 *      硬件层面也算一次"读"）。症状很有辨识度：fork/write/wait 全程
 *      正常，只有"调用了 exec 的那个进程、exec 生效的那一刻"崩。
 *      这是本 Lab 实测抓到的真实 bug，README 常见坑里也列了一条。
 *   5. call syscall_dispatch——返回值在 rax，恰好也是 SYSRET 传回用户态
 *      返回值时约定用的寄存器，不需要额外搬移。
 *   6. 恢复 r11/rcx（顺序跟保存时相反）。
 *   7. 恢复用户栈（从 syscall_saved_user_rsp 读回 rsp——注意这时读到的
 *      可能已经是 sys_exec() 写进去的新值，这正是第 3 步传地址的意义）。
 *   8. sysretq。
 *
 * 提示：
 * syscall_entry:
 *     mov %rsp, syscall_saved_user_rsp(%rip)
 *     mov syscall_kernel_rsp(%rip), %rsp
 *
 *     push %rcx
 *     push %r11
 *
 *     lea 8(%rsp), %r8
 *     lea syscall_saved_user_rsp(%rip), %r9
 *
 *     mov %r8, %rcx
 *     mov %r9, %r8
 *     mov %rsi, %rdx
 *     mov %rdi, %rsi
 *     mov %rax, %rdi
 *     call syscall_dispatch
 *
 *     pop %r11
 *     pop %rcx
 *
 *     mov syscall_saved_user_rsp(%rip), %rsp
 *     sysretq
 */

/* TODO 2：实现 enter_user_mode(entry, stack)。
 *
 * 为什么：跟 Lab6 完全一样，一个字没改，照抄即可。但它在 Lab7 还存在这件
 * 事本身值得注意——riscv64 那边 Lab7 把对应的 enter_user_mode *删掉*了
 * （见 ../riscv64/trap_entry.S 末尾的说明），x86_64 这边必须保留。
 *
 * 原因是 sysretq 和 sret 的一处根本差异：Lab7 引入 struct trapframe/
 * trap_return 之后，riscv64 可以用同一条 sret 同时承担"第一次启动一个
 * 进程"和"resume 一个被打断的进程"（sret 读的 sepc/sstatus 全是软件可写
 * 的 CSR，不要求"之前必须发生过一次 trap"）；x86_64 的 sysretq 读的
 * rcx/r11 必须来自*之前一次 SYSCALL 硬件自动填入*的值，"第一次"这个场景
 * 没有合法来源，只能继续走 iretq 这条手工构造特权级切换的路子。所以
 * x86_64 即使有了 trap_return，仍然需要 enter_user_mode 独立存在。
 *
 * iretq 从栈顶按顺序弹出五个 64 位值：RIP、CS、RFLAGS、RSP、SS——跟
 * page_fault_stub/timer_stub 里硬件自动压的栈帧形状完全一样（这不是巧合，
 * 这就是 IDT 路径本来的样子，这里只是手工搭一份等价的），意味着 push 的
 * 顺序必须反过来（SS 最先 push，因为它在栈帧里地址最高，最后被弹出）。
 *
 * System V AMD64 调用约定：entry=rdi，stack=rsi——先搬到别的寄存器再开始
 * push，因为 push 会改变 rsp，而这个函数执行期间应该始终留在内核栈上，
 * 直到 iretq 那一刻由硬件原子地切到栈帧里指定的用户 RSP。
 *
 * 五个值的含义：
 *   SS     = 0x20 | 3（GDT_SEL_USER_SS_RPL3）
 *   RSP    = stack 参数（USER_STACK_VADDR）
 *   RFLAGS = 0x202，即 IF=1（bit 9）+ bit 1（Intel 手册规定永远置 1 的
 *            保留位）——用户程序需要能被定时器中断打断，这跟 SYSCALL
 *            路径故意通过 FMASK 清 IF 是相反的取向
 *   CS     = 0x28 | 3（GDT_SEL_USER_CS_RPL3）
 *   RIP    = entry 参数（USER_PROG_VADDR）
 * 段选择子在这里直接写字面量，是因为 .S 文件不方便 #include trap.c 的宏
 * 定义——两边数值必须手工保持同步，跟 user_prog.S 里手写 SYS_WRITE/
 * SYS_EXIT 字面量是同一类"跨文件手工契约"。
 *
 * 提示：
 * enter_user_mode:
 *     mov %rdi, %rax // entry
 *     mov %rsi, %rdx // stack
 *
 *     push $0x20 | 3
 *     push %rdx
 *     push $0x202
 *     push $(0x28 | 3)
 *     push %rax
 *
 *     iretq
 */
