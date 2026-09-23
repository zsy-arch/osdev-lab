/* Lab6 x86_64：page_fault_stub/timer_stub 原样保留自 Lab5（SYSCALL 是
 * 独立于 IDT 的第三条陷入路径，不影响前两条，见 trap.c 顶部注释）。
 * 新增的是 syscall_entry——SYSCALL 指令触发时 CPU 直接跳到 IA32_LSTAR
 * 里存的这个地址，不经过 IDT，也不像 IDT 中断门那样自动压栈保存现场：
 * 硬件只做了这几件事（AMD64 手册 SYSCALL 一节明确写出来的，不是本课程
 * 猜测）——
 *   RCX  = 用户态 RIP（SYSRET 用它恢复）
 *   R11  = 用户态 RFLAGS（SYSRET 用它恢复）
 *   RFLAGS = RFLAGS AND NOT(IA32_FMASK)（trap.c 里配置成只清 IF）
 *   CS/SS = 按 IA32_STAR[47:32] 算出的内核段选择子
 *   RIP  = IA32_LSTAR
 * %rsp 完全不变（还是用户栈指针）——这是 SYSCALL 跟 IDT 陷入路径最大的
 * 区别，必须手动换栈，见下面的注释。
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

/* syscall_entry：进来的时候 %rsp 还是用户栈，%rcx=用户RIP，%r11=用户
 * RFLAGS，%rax=系统调用号，%rdi/%rsi=参数（用户程序按这个约定填好，见
 * user_prog.S）——这四个约定寄存器（rax 号+rdi/rsi/rdx/r10/r8/r9 六个
 * 参数位，本 Lab 只用到前两个）都不能在换栈之前被破坏，换栈本身（mov
 * %rsp 到临时变量、再换成内核栈）不涉及这些寄存器，天然安全。
 *
 * 换栈用一个全局变量（syscall_saved_user_rsp）暂存用户 RSP，不是压栈
 * ——这一刻 %rsp 还是用户栈指针，如果先 push 保存内核栈上的东西，会把
 * 数据压到用户栈里（用户栈的内容对内核不可信，且本 Lab 的用户程序栈
 * 空间本来就很小，教学取向不依赖"用户栈还有余量能借用几个字节"这个
 * 假设）；反过来，全局变量在切栈动作发生之前就能被写入，不依赖任何
 * 栈的存在。
 *
 * ── Lab9：那个全局变量只能当"过渡"，不能当"保管" ──────────────────
 *
 * Lab7/Lab8 的版本把用户 RSP 一直留在 syscall_saved_user_rsp 这个全局
 * 变量里，直到最后一行 `mov syscall_saved_user_rsp(%rip), %rsp` 再读
 * 出来。那两个 Lab 里这是对的，但对的理由很窄：全局变量意味着"全系统
 * 只有一份"，而一份就够用的前提是"任何时刻最多只有一个进程处在
 * 系统调用中间"。
 *
 * Lab7/Lab8 恰好满足这个前提。会让出 CPU 的系统调用只有 sys_exit_proc
 * 一个，而它*永不返回*——进程已经是 ZOMBIE，再也走不到读回用户 RSP
 * 那一步。所以那个全局变量从写入到读出之间，中间不可能插进另一个进程
 * 的系统调用。
 *
 * Lab9 这个前提没了，一次多出三个会"让出 CPU 之后还要正常返回"的系统
 * 调用：阻塞的 sys_wait、管道空了的 read、管道满了的 write。它们都会
 * yield() 出去，让别的进程跑，然后回来接着往下执行、最终走到
 * sysretq。中间那个"别的进程"只要做了任何一次系统调用，就会把
 * syscall_saved_user_rsp 改成*它自己*的用户栈指针。等第一个进程回来
 * 读，读到的是别人的栈。
 *
 * 这个 bug 的症状特别不像内存 bug：两个进程的用户栈指针都是合法地址，
 * sysretq 不会失败，进程会带着错误的 rsp 继续跑用户代码——读到的局部
 * 变量是另一个进程的、函数返回时 ret 到另一个进程压的返回地址。现场
 * 看起来像"程序逻辑乱了"，而且只在两个进程的系统调用恰好交错时出现。
 *
 * 修法：全局变量只当"从用户栈切到内核栈这几条指令之间的过渡"，一拿到
 * 内核栈立刻把它 push 到内核栈上——内核栈是*每进程一份*的，这就天然
 * 解决了保管问题。过渡期间不可能被别人破坏，因为这中间没有任何一条
 * 指令会切换进程：SYSCALL 进来时硬件已经按 IA32_FMASK 清掉了 IF
 * （trap.c 里配置的），中断进不来，而不被中断就没有 yield 的机会。
 *
 * ── 压栈的槽位和对齐 ──────────────────────────────────────────────
 *
 * 从高地址到低地址（即 call syscall_dispatch 之前那一刻，%rsp 指向的
 * 是最低的那一格）：
 *
 *     [rsp+72]  用户 RSP     ← 原来留在全局变量里的那个值
 *     [rsp+64]  用户 RIP     （硬件放在 rcx 里的）
 *     [rsp+56]  用户 RFLAGS  （硬件放在 r11 里的）
 *     [rsp+48]  rbp（父进程 syscall 陷入那一刻的原值，fork() 寄存器
 *               快照，见下方 Lab9 模块注释）
 *     [rsp+40]  rbx（同上）
 *     [rsp+32]  r12（同上）
 *     [rsp+24]  r13（同上）
 *     [rsp+16]  r14（同上）
 *     [rsp+8]   r15（同上）
 *     [rsp+0]   对齐填充
 *
 * 那个填充槽不是凑数。System V AMD64 ABI 要求 `call` 执行的那一刻
 * %rsp 是 16 的倍数。内核栈顶本身是 16 对齐的（proc.c 里
 * kstack_kva_top = 页对齐地址 + 整数个页），Lab8 压两个值共 16 字节，
 * 正好还是 16 对齐；Lab9 第一次改动（只加 user_rsp_slot 那次）压三个
 * 值是 24 字节，rsp 变成 8 mod 16，违反 ABI，所以补了一个 8 字节
 * 填充，凑到 32 字节、重新 16 对齐。
 *
 * 违反的后果是真实的：内核不是用 -mgeneral-regs-only 编译的（那是
 * 用户程序的选项），GCC 完全可以在 syscall_dispatch 或它调用的任何
 * 函数里用 movaps 这类要求 16 字节对齐的 SSE 指令搬结构体——遇到
 * 8 mod 16 的栈就是 #GP。而它会不会真的这么做取决于优化级别和函数
 * 内容，也就是说这是一个"改一行无关的 C 代码就可能突然出现或消失"
 * 的崩溃。
 *
 * Lab9 第二次改动（修复 fork() 寄存器丢失 bug，见下方模块注释）又插进
 * 六个 push（rbp/rbx/r12-r15，共 48 字节），加上原来的填充+3 个值共
 * 32 字节，call 之前总共压了 80 字节，80 mod 16 = 0，恰好还是对齐的
 * ——填充槽继续保留，只是现在它对齐的是"1+3+6=10 个 8 字节槽"而不是
 * "1+3=4 个"，原理不变：只要保证 call 之前总压栈字节数是 16 的倍数，
 * 填充要不要存在、存在多大，都是从这个约束反推出来的，不是先验的
 * 常数。（第 7 个参数 gpr_snapshot 后来用 push %r10 又临时压了一份，
 * 但那条 push 发生在 call 指令的操作数准备阶段、被 add $8,%rsp 立刻
 * 撤销，不改变"call 那一刻栈是否 16 对齐"这个不变量：push 之后是
 * 88 字节，88 mod 16 = 8，看起来不对——但 ABI 的 16 对齐要求本来就是
 * "被调函数入口处 %rsp 对齐"，也就是 call 执行*之后*，不是执行之前；
 * push 完再 call，call 自己压 8 字节返回地址，函数入口处正好是
 * 88+8=96 字节，96 mod 16 = 0，对齐。） */
/* ── Lab9 修复：fork() 丢失父进程 callee-saved 寄存器的 bug ──────────
 *
 * 症状：/init 第一次 fork() 之后，子进程立刻触发缺页——x86_64 上是一次
 * "写地址 0xfffffffffffffffc"的错误（GDB 确认故障指令是子进程
 * main() 里 `movl %eax, -0x4(%rbp)`，也就是 fork() 刚返回、把返回值存
 * 进局部变量 pid 的那一句），riscv64 上是"从地址 0 取指"（同一类问题，
 * 见 riscv64 版本 trap_entry.S 对应位置的注释）。
 *
 * 根因：sys_fork() 只把子进程 trapframe 的 rip/rsp/cs/ss/rflags 五个
 * 字段设成"父进程这次 syscall 返回后该恢复到的现场"，其余通用寄存器
 * （rbp/rbx/r12-r15/r8-r11/rdi/rsi/rdx/rcx，见 proc.h struct trapframe）
 * 保持 proc_alloc_skeleton() memset 出来的 0——这份设计明确写在 proc.c
 * sys_fork() 的模块注释里，理由是"本课程的用户态代码只依赖 rax/rdi/rsi
 * 在系统调用前后保持约定关系,不需要完整寄存器现场"。这句话对 Lab7/Lab8
 * 手写的 user_prog.S 是真的——那段汇编从不使用 rbp 做帧指针，也不依赖
 * fork() 返回后 rbp 还是原来的值。Lab9 换成 GCC 编译的 init.c/sh.c 之后
 * 这句话不再成立：编译器生成的 fork() 调用点前后是普通的 C 函数调用，
 * fork() 返回后紧跟着的第一条指令往往就是 `mov %eax, -N(%rbp)`（把返回
 * 值存进栈帧里的局部变量）——这要求 rbp 在 fork() 前后不变，这正是 C
 * 调用约定对"任何函数调用"的基本承诺（rbp 是 callee-saved），fork()
 * 没有理由是例外。子进程的地址空间是父进程栈内容的逐页拷贝（uvm_copy()），
 * 栈上的数据是按父进程的 rbp 值摆放的；子进程恢复执行时如果 rbp 是 0，
 * 就会用一个跟栈内容完全对不上的基址去算局部变量地址——0 - 4 正好解释
 * 了 0xfffffffffffffff**c**这个具体数值。
 *
 * 为什么这一路走下来"看起来能读到父进程的寄存器"其实不可靠：rbx/r12-
 * r15/rbp 是 System V AMD64 调用约定里的 callee-saved 寄存器，
 * syscall_entry 调 syscall_dispatch、syscall_dispatch 再调 sys_fork()，
 * 这两层 C 函数调用*本身*不要求任何一层去主动破坏它们——但这只保证
 * "调用者看到的返回后的值不变"，不保证"被调函数内部执行期间寄存器里
 * 还是调用者的原值"：syscall_dispatch 自己的栈帧建立就有
 * `push %rbp; mov %rsp, %rbp`，进入函数体的第一件事就已经把 rbp 改写成
 * 它*自己*的帧基址——sys_fork() 内部读 rbp 读到的早就不是父进程的原值，
 * 而是 syscall_dispatch/sys_fork 自己的栈帧指针。rbx/r12-r15 这次编译
 * 恰好没被 syscall_dispatch 用作临时寄存器（objdump 可以验证），但这是
 * 编译器优化决策的偶然结果，不是任何人保证过的行为，换一个 GCC 版本或
 * 优化级别随时可能不再成立。
 *
 * 唯一可靠的做法是本文件已经在用的模式：在硬件/软件把值破坏之前，用
 * 汇编把它显式存到*调用者能一路传下去*的位置——rcx（用户 RIP）/r11
 * （用户 RFLAGS）已经是这个模式的先例。Lab9 把它扩展到 rbx/rbp/r12-r15：
 * syscall_entry 换栈之后、在调用 syscall_dispatch 之前，立刻把这六个
 * callee-saved 寄存器原样 push 到本进程的内核栈上——这一刻它们还是
 * 硬件刚从用户态带进来的原值，push 完之后不管 syscall_dispatch 内部
 * 怎么用这几个寄存器都不影响已经存好的这份快照。快照的地址（不是值本身，
 * 原因跟 rip_slot/rsp_slot 一样：sys_fork() 只需要读，不需要写，但统一
 * 传地址不增加复杂度，还跟 rip_slot/rsp_slot 保持同一种参数形式）通过
 * 新增的第 7 个参数 gpr_snapshot 传给 syscall_dispatch，再原样转发给
 * sys_fork()——sys_write/sys_exec/sys_wait 等不修改 rip/rsp 之外任何
 * 寄存器的系统调用不需要它，参数照样统一传，理由跟 rip_slot/rsp_slot
 * 一致（见上面模块注释）。
 *
 * gpr_snapshot 的 C 类型是 `struct context *`（proc.h 已有的类型，
 * swtch.S 用来保存内核态切换现场的同一个结构体)——不是 struct
 * trapframe。两者都描述"保存下来的寄存器现场"，但 trapframe 是完整的
 * 20 个字段（给 IDT 陷入路径用），这里的快照只有 6 个 callee-saved
 * 寄存器，借用 context 现成的类型定义比再声明一个新结构体更省事，且
 * 语义也贴切：这份快照本质上就是"如果这一刻对父进程做一次 swtch() 会
 * 保存下来的那六个字段"，只是保存的手段是 push 而不是 swtch()。
 *
 * 没有把 rax/rdi/rsi/rdx/rcx/r8-r11 也做成快照：这些是调用约定里
 * caller-saved 的寄存器，C 语言层面"调用 fork()"这件事本身就已经允许
 * 它们被破坏（编译器生成 fork() 调用点时，从不假设这些寄存器在调用后
 * 还保留调用前的值）——子进程作为"fork() 调用的另一种返回方式"，遵守
 * 同一条约定即可，不需要、也不应该额外保证这些寄存器的值（真实 Linux
 * 的 fork() 语义也是如此：只有 rax 的返回值有意义，其余 caller-saved
 * 寄存器不承诺）。
 *
 * ---- push 顺序为什么是 r15..rbx（跟 struct context 字段顺序反着来），
 *      这里曾经真的因为顺序搞反而踩过一个真实 bug，留档 ----
 *
 * 第一版这里直接照抄 struct context 声明的字段顺序去 push（rbx/rbp/
 * r12/r13/r14/r15，跟 swtch.S 的 push 序列字面上一样），当时的推理是
 * "swtch.S 也是这么 push 的，跟着抄一定对"——这个推理本身就是错的：
 * swtch.S 里 struct context* 这个指针的值，从头到尾只被当成一个*不透明
 * 的地址*在 swtch() 内部原样传递（mov %rsp,(%rdi) 存下来、mov %rsi,%rsp
 * 读回去），没有任何地方真的按字段名对它做过 ptr->rbx 这种偏移量解引用
 * ——proc_alloc() 唯一一次"按偏移量摆位置"（p->context = p->tf -
 * sizeof(struct context)）用的也是整块 memset 成 0，从不关心某个具体
 * 偏移量此刻该叫 rbx 还是 r15。换句话说，swtch.S 的 push 顺序和 struct
 * context 字段顺序"字面相同"这件事，在 swtch.S 自己的使用方式下根本
 * 不产生任何约束——save 和 restore 是同一段代码互相镜像（push rbx 最先/
 * pop rbx 最后），只要内部自洽，跟字段名对不对得上完全无关。
 *
 * sys_fork() 的 gpr_snapshot 是全代码库*第一次*真的把 struct context*
 * 当成一个"按字段名取偏移量"的指针来解引用（gpr_snapshot->rbx 等），
 * 这时候"push 顺序跟字段声明顺序字面相同"反而是错的：push 到栈上之后，
 * *最先* push 的寄存器（rbx）落在*最高*地址，*最后* push 的（r15）落在
 * *最低*地址——而 C 结构体的字段是从指针地址往*高*地址方向排列的（字段
 * 0 在偏移 0，字段 1 在偏移 8，以此类推）。指针如果指向"最低地址"（也
 * 就是 r15 实际存放的位置）,ptr->rbx（偏移 0）读到的其实是 r15 的值,
 * ptr->rbp（偏移 8）读到的是"6 个寄存器 push 完之后再往上一个槶位"，
 * 那个位置存的是 push 顺序里*紧挨着 rbx 之前*的那个值——在本文件的完整
 * push 序列里正好是 push %r11（用户 RFLAGS）。
 *
 * 症状（QEMU+GDB 实测，晚于 user_rip_slot/user_rsp_slot 偏移量那个 bug
 * 被修好之后才暴露出来，因为这两个 bug 互相独立、需要先后分别触发)：
 * /init 的第一次 fork()+exec("/sh") 已经能正常工作,但 sh 自己为了跑
 * /initrc 第一条命令再次 fork() 时,子进程立刻缺页——GDB 里看到的现场
 * 是 %rbp = 0x202（正是 sys_fork() 填进子进程 trapframe.rflags 的那个
 * 常量,被错当成 rbp 读出来了）,故障地址 cr2 = 0x1fe,正好是
 * 0x202 - 4——跟本文件顶部那个"0 - 4"的原始 bug 是同一种失败模式
 * （子进程恢复执行后第一条 `mov %eax, -4(%rbp)` 用一个错误的 rbp 去算
 * 局部变量地址），只是这次 rbp 不是 0，是另一个寄存器槽位的值,数值不同
 * 但根因完全同构。
 *
 * 修法：push 顺序反过来，改成 r15/r14/r13/r12/rbp/rbx（先 push 的落在
 * 高地址，对应 struct context 里偏移更大的字段；最后 push 的 rbx 落在
 * 最低地址,对应偏移 0）——gpr_snapshot 指针指向这次 push 序列里*最后*
 * 一条（push %rbx）落地的那个地址，往上数正好是 rbp/r12/r13/r14/r15,
 * 跟 struct context 字段声明顺序在*内存地址*意义上真正对应,不再只是
 * "汇编源码里写的寄存器名字顺序看起来一样"这种表面相似。
 *
 * 这是本文件第三次印证同一条经验（前两次分别是"Lab7 在这里踩过的一个
 * 真实 bug"和"user_rip_slot/user_rsp_slot 偏移量算错"）：涉及手工栈
 * 布局和指针偏移量的代码，"抄一段看起来相关的现成代码"和"援引一个
 * 现成的类型定义"都不能替代从头把 push/pop 顺序和该类型的字段偏移量
 * 重新数一遍——两段代码"用了同样几个寄存器名字"不代表它们在内存里的
 * 摆放方向也相同，方向（栈从高地址向低地址增长，结构体从指针地址向高
 * 地址排列）恰恰是最容易被直觉忽略、又最容易一错到底的那一个维度。 */
syscall_entry:
    mov %rsp, syscall_saved_user_rsp(%rip)
    mov syscall_kernel_rsp(%rip), %rsp

    /* 过渡结束：用户 RSP 从全局变量搬到本进程的内核栈上。从这里往后，
     * 别的进程怎么折腾那个全局变量都不影响这次系统调用。 */
    pushq syscall_saved_user_rsp(%rip)

    /* rcx（用户 RIP）/r11（用户 RFLAGS）是 SYSRET 唯一会读的两个
     * 寄存器，syscall_dispatch 是一个普通 C 函数，System V AMD64
     * 调用约定里 rcx 是 callee-clobbered（第四个整数参数寄存器）——
     * 如果不在调用前保存，call 指令本身（不需要 rcx）大概率不会碰它，
     * 但 syscall_dispatch 函数体内部完全可能把 rcx 用作临时寄存器，
     * 保存下来是唯一不依赖"这个函数恰好没用到 rcx"这种脆弱假设的
     * 做法。r11 同理是 callee-clobbered（调用约定里的易失寄存器）。 */
    push %rcx
    push %r11

    /* fork() 寄存器快照：rbx/rbp/r12-r15，理由见上方新增的模块注释。
     * 必须在这里做，不能挪到 syscall_dispatch 或更深的函数里——一旦
     * 进了任何 C 函数，rbp 就已经被那个函数自己的 `push %rbp` 覆盖，
     * 原始值就再也读不到了。
     *
     * push 顺序是 struct context 字段声明顺序（rbx/rbp/r12-r15）的
     * *反序*——不是笔误，是刻意的，理由见上方模块注释末尾那段"push
     * 顺序为什么是 r15..rbx"的完整推导：栈从高地址向低地址增长，最先
     * push 的落在最高地址；C 结构体字段从指针地址向高地址排列，偏移 0
     * 的字段在最低地址。要让 gpr_snapshot（一个真的会被按字段名解引用
     * 的 struct context *）在地址意义上对应上，必须让*最后* push 的
     * 寄存器（对应字段声明里的第一个字段 rbx）落在这六个槽位里*最低*
     * 的地址——也就是先 push r15，最后 push rbx。 */
    push %r15
    push %r14
    push %r13
    push %r12
    push %rbp
    push %rbx

    /* 对齐填充，理由见函数头注释末尾那一段。用 sub 而不是再 push 一个
     * 寄存器：push 需要挑一个寄存器当"垫片"，而这一刻每个寄存器都装着
     * 有用的东西（rax/rdi/rsi/rdx 是用户传进来的参数，还没搬到位）；
     * sub 不碰任何寄存器，意图也更清楚——这 8 个字节不存任何值。 */
    sub $8, %rsp

    /* Lab7 新增第 4/5 个参数：user_rip_slot（这次 SYSCALL 恢复现场用的
     * rcx，此刻已经 push 在栈上，见上面两行）的地址，以及 user_rsp_slot
     * （syscall_saved_user_rsp 这个全局变量本身)的地址——sys_fork/
     * sys_exec 需要读（有时还需要写)这两个值，理由见 proc.h 里 sys_
     * fork/sys_exec 声明处的大注释：SYSCALL 路径完全不经过 proc.h 的
     * struct trapframe（那是 IDT 路径——timer_stub/page_fault_stub——
     * 专用的现场格式，两条陷入路径在本 Lab 里是分开的，见 trap.c 顶部
     * 模块注释"SYSCALL 是独立于 IDT 的第三条陷入路径"）。
     *
     * 两个都传地址，不传值：sys_exec() 需要能*修改*"这次 syscall 返回
     * 后会恢复到哪个 RIP/RSP"，如果只传值,sys_exec() 改了也没用（改的
     * 是它自己函数调用栈上的一份拷贝),必须能通过指针写回这两个位置
     * 本身——下面 pop %rcx / mov syscall_saved_user_rsp(%rip), %rsp
     * 才会读到 sys_exec() 写进去的新值。sys_fork() 不需要写它们（子
     * 进程走的是完全独立的另一份 trapframe，父进程自己这次 syscall
     * 正常返回,两个值都不用改),但统一成同一种参数类型更简单,不需要
     * 给 sys_fork/sys_exec 定义两种不同的签名。
     *
     * 此刻栈上从低到高是：填充(0)、r11(8)、rcx(16)、用户 RSP(24)，所以
     * lea 16(%rsp), %r8 算的是"push %rcx 那次写入的内存位置"，
     * lea 24(%rsp), %r9 算的是"pushq syscall_saved_user_rsp 那次写入的
     * 内存位置"。两条都用 lea 算地址、不读值：sys_exec() 需要能*修改*
     * 这两个位置,只传值的话它改的是自己栈帧上的一份拷贝,下面的
     * pop %rcx / mov 24(%rsp), %rsp 读不到。
     *
     * 地址在 call 之前就算好,不依赖 call 自己压返回地址之后 %rsp 还会
     * 怎么变——绝对地址一旦算出来就固定了,callee 内部 %rsp 怎么动都
     * 不影响它。
     *
     * Lab9 第一次改动就是 r9 这一条：从"全局变量的地址"变成"本进程内核
     * 栈上那个槽位的地址"。sys_exec() 往里写新栈顶,写的是这次系统调用
     * 私有的一份,不会被并发的另一个进程的系统调用覆盖。(exec 会换页表,
     * 但内核栈落在 self-map 窗口里、被 pagetable_copy_kernel_range()
     * 复制进了每个进程的页表,换完页表这个槽位照样读得到。)
     *
     * Lab9 第二次改动：中间插进来的 r15/r14/r13/r12/rbp/rbx 六个 push
     * （fork() 寄存器快照，见上方新增的模块注释）把 rcx/用户RSP 两个
     * 槽位往高地址方向推了 48 字节（6*8，不是 40——这里曾经真的算错成
     * 40，见下面"留档"那一段）。此刻栈上从低到高依次是：填充(0)、
     * rbx(8)、rbp(16)、r12(24)、r13(32)、r14(40)、r15(48)、r11(56)、
     * rcx(64)、用户RSP(72)——所以 user_rip_slot 是 lea 64(%rsp)（对应
     * rcx 那个槽位），user_rsp_slot 是 lea 72(%rsp)（对应用户RSP 那个
     * 槽位）；gpr_snapshot（新增第 7 个参数,类型是 struct context *，
     * 指向 rbx 那个槽位，也就是 push 顺序里*最后*压的那个、地址*最低*
     * 的那个——跟 struct context 字段声明第一个是 rbx 对应)是
     * lea 8(%rsp)，正好是栈顶：push 顺序刻意反过来（先 r15 后 rbx，
     * 见上方模块注释"push 顺序为什么是 r15..rbx"那一段完整推导）就是
     * 为了让 rbx 落在最低地址、也就是这一刻的栈顶，gpr_snapshot 才能
     * 直接指向栈顶而不需要再往栈里数第几个槶位——这不是巧合，是那次
     * push 顺序反转专门换来的效果。
     *
     * ---- 这里也曾经踩过一个真实 bug，留档 ----
     *
     * push 顺序从 rbx..r15 反转成 r15..rbx 的那次修改（见上方模块注释
     * "push 顺序为什么是 r15..rbx"）只改了 push 指令本身、下面的 pop
     * 序列、以及紧邻的这段地址表注释,却漏改了 gpr_snapshot 的 lea 立即
     * 数——那一行当时还留着反转前的旧值 `lea 48(%rsp)`,而反转前 48 正好
     * 是 rbx（那时候是最先 push 的）所在的地址,反转后 48 变成了 r15
     * （现在最先 push 的)所在的地址,rbx 真正的位置变成了 8。这一行
     * "看起来没被这次改动碰到"（改的人大概检查了 push/pop/注释三处,
     * 却没意识到 lea 的立即数也是这次反转隐含依赖的第四处)，于是
     * gpr_snapshot 指针实际指向了 r15 而不是 rbx——sys_fork() 读
     * gpr_snapshot->rbx 时读到的其实是 r15 原来的值,gpr_snapshot->rbp
     * 读到的是紧挨着 r15 之后 push 的 r14,以此类推,整份快照相当于
     * 逐字段"错位"了六个位置中的五个。
     *
     * 更隐蔽的是 QEMU+GDB 实测看到的现场不是"错位的寄存器值"，而是
     * 直接读到了栈上*更早*的、跟这次系统调用完全无关的垂悬数据（此处
     * gpr_snapshot 的值本身——不是它指向的内容,是这个指针变量的值——
     * 变成了一串看不出规律的位模式,user_rip_slot/user_rsp_slot 两个
     * 指针也一起表现出同样的症状)：因为这个 bug 发生在 syscall_dispatch
     * 的参数传递链路最前端（lea 算出来的值直接决定 r8/r9/r10 装的是什么,
     * 这三个寄存器又直接决定接下来四条 `mov` 搬移和 `push %r10` 传参),
     * 一旦这一步本身算错,后面所有基于它的操作都在处理一个错误的地址,
     * 但错误本身在汇编器/CPU 层面完全合法（lea 8(%rsp) 和 lea 48(%rsp)
     * 都是合法指令),不会在这一步触发任何故障,只会在 sys_fork() 真正
     * 尝试解引用这个错误指针（*gpr_snapshot 或 gpr_snapshot->rbx）时才
     * 暴露——如果这个错误地址凑巧落在未映射页,会在 proc.c 内部触发缺页
     * （比本文件顶部两个 bug 更早一步崩，因为读的动作发生在 sys_fork()
     * 函数体内,而不是等到 sysretq 之后回到用户态才暴露）；这次实测中
     * 这个地址凑巧没有立刻触发缺页（读到了栈上别的合法数据,只是数值
     * 毫无意义),所以现场表现成"子进程带着一份看似随机的寄存器现场恢复
     * 执行,随后在用户态某条访存指令上缺页"，跟本文件顶部原始 bug
     * （子进程 rbp=0)的外部症状（子进程 fork() 返回后立刻缺页)高度相似,
     * 但没有验证 gpr_snapshot 指针值本身之前，很容易误以为是同一个 bug
     * 又出现了一次。GDB 直接 break sys_fork、打印 gpr_snapshot 的值
     * （而不是先入为主地去解引用它）是唯一能把这类"参数本身已经错了"
     * 和"参数是对的、指向的内容错了"这两类症状区分开的办法——这也是本
     * 文件第四次印证同一条经验：手工偏移量算术只要有一处引用没有跟着
     * 相关的改动同步更新，就会留下一个看起来能编译、能链接、甚至大部分
     * 时候能跑但在特定路径下彻底错误的地雷,唯一可靠的排查方法是对着
     * 实际的寄存器/内存内容重新核对,不能只读代码和注释就下结论。
     *
     * ---- 这里曾经踩过的一个真实 bug，留档 ----
     *
     * 第一版这两行写的是 lea 56(%rsp)/lea 64(%rsp)——比正确值 64/72
     * 都少了 8，等于把新插入的六个 push 算成了 40 字节（5 个寄存器)
     * 而不是 48 字节（6 个)。跟函数头模块注释里"这五个"错写成"这六个"
     * 是同一次疏忽的两个症状：数错了寄存器个数，一处只改了文字说明,
     * 这两行算偏移量的立即数没有跟着改。
     *
     * 后果不是"读错值"，是"整条 sysretq 返回路径全错"：sys_exec() 往
     * user_rip_slot 写新程序入口地址,实际写进的是 r11（用户 RFLAGS)
     * 那个槽位；往 user_rsp_slot 写新用户栈顶,实际写进的是 rcx（用户
     * RIP)那个槽位。返回时 pop %r11/pop %rcx 读到的是被写错的这两个
     * 值——RFLAGS 变成了 ELF 入口地址,RIP 变成了用户栈顶地址,
     * sysretq 之后 CPU 从一个栈地址开始取指,几乎必然缺页。
     *
     * 症状（QEMU 实测)：/init fork() 出的子进程 exec("/sh") 之后立刻
     * 缺页,error_code 显示 instruction-fetch,故障地址是一个看似
     * 无关的小数值（0x202)——这不是巧合,是 sys_fork() 给子进程
     * trapframe 填的 rflags 常量本身。/init 先 fork() 再在子进程里
     * exec()，这条路径上*先*调用一次 sys_fork()（这次没问题,因为
     * sys_fork() 只读 user_rip_slot/user_rsp_slot,不写)，父进程带着
     * 这份被污染的偏移量正常返回、继续往下跑到 wait()；子进程带着
     * fork() 时那份原始的、正确的 rax=0 现场恢复执行、调用 exec()——
     * exec() 内部才第一次*写*这两个槽位,这一写才真正暴露偏移量算
     * 错了这件事。0x202 具体怎么出现在 CR2 里，本质上是"两个本该分开
     * 的 8 字节槽位因为偏移量差了一格而合并读写"这一类错误的必然结果,
     * 具体数值依赖当时两个槽位当前分别存的是什么、写入顺序，不需要也
     * 不值得逐字节还原——找到偏移量算错这个根因、改成 64/72 之后,
     * 缺页当场消失,足以确认诊断正确。
     *
     * 这类 bug 是"改动范围看起来很小、影响面却牵动全局"的典型：新插入
     * 六个 push 只影响*它们自己往上*的槽位偏移，理应只需要机械地把
     * 每个受影响的常量加 48——但"48"这个数字本身来自"数一数插入了
     * 几个 push"，如果这一步本身数错，后面所有基于它的算术都会带着
     * 同一个错误一起偏移，而且偏移量还是错的这件事从汇编器角度完全
     * 合法（16(%rsp) 和 64(%rsp) 都是合法的寻址），编译、链接、甚至
     * 大部分系统调用（sys_write/sys_wait 等不写这两个槽位）都不会
     * 触发它——只有真正*写*了 user_rip_slot/user_rsp_slot 的 sys_exec()
     * 才会。这是本文件里第二次印证同一条经验（上面"Lab7 在这里踩过的
     * 一个真实 bug"是第一次）：涉及手工偏移量算术的代码，唯一可靠的
     * 验证方式是拿真实的 push/pop 顺序重新数一遍，不能依赖"改了一处、
     * 其余照抄"的直觉。 */
    lea 64(%rsp), %r8      /* user_rip_slot：指向 push %rcx 那次写入
                             * 的槽位（用户 RIP） */
    lea 72(%rsp), %r9      /* user_rsp_slot：指向 pushq
                             * syscall_saved_user_rsp 那次写入的槽位
                             * （用户 RSP） */
    lea 8(%rsp), %r10      /* gpr_snapshot：struct context *，指向
                             * rbx（第一个字段，push 顺序里*最后*
                             * push 的那个，见上方 push 顺序注释），
                             * 稍后转移到栈上传递，因为整数参数寄存器
                             * 只有 6 个，见下方 push %r10 那一行 */

    /* System V AMD64 调用约定：第一参数 rdi，第二 rsi，第三 rdx，第四
     * rcx，第五 r8，第六 r9，第七个及以后从栈上传（调用者负责压栈,
     * 被调函数按 [rbp+16]/[rbp+24]/... 或等价的 [rsp+N] 形式读取，
     * 具体怎么读是 syscall_dispatch 编译产物的事,调用方只需要保证
     * "第七参数在 call 那一刻位于栈顶"）。
     *
     * Lab8 的 syscall_dispatch 签名是
     *   (num, a0, a1, a2, user_rip_slot, user_rsp_slot)
     * 六个参数刚好占满六个寄存器参数位：
     *   rdi=num  rsi=a0  rdx=a1  rcx=a2  r8=rip_slot  r9=rsp_slot
     *
     * Lab9 加了第 7 个参数 gpr_snapshot，六个寄存器参数位已经占满，
     * 只能走栈传参——`push %r10` 把它放上栈顶，但这条 push 本身也会
     * 让 %rsp 往下移 8 字节，如果放在下面四条 mov 搬移之前做，会导致
     * 那几条 mov 依赖的相对偏移全部错位（它们操作的是寄存器，不是栈,
     * 所以其实不受影响——之所以仍然把 push 放在最后、`call` 之前紧
     * 贴着做，是为了让"第 7 个参数入栈"和"call 压返回地址"这两步在
     * 视觉上挨在一起，避免以后有人在中间插入别的指令而破坏 ABI 要求的
     * "call 执行前 %rsp 是 16 的倍数"这条隐含约束——第 7 个参数占 8
     * 字节，加上前面 sub $8 那份填充，两者一起让 call 之前的 %rsp
     * 依旧是 16 的倍数,详见函数头注释末尾关于对齐的推导）。
     *
     * 上面三条 lea 已经把 rip_slot/rsp_slot 直接算进了 r8/r9——正好就是
     * 它们该在的位置，不需要任何搬移。剩下的工作只是把用户传进来的
     * rax/rdi/rsi/rdx 往后挪一位，给 num 腾出 rdi，再把 gpr_snapshot
     * push 上栈顶充当第 7 个参数。
     *
     * 搬移顺序必须"从高参数位往低参数位"：每一条的目标寄存器都是下一条
     * 的源寄存器，反着来会在读到之前就把源覆盖掉。这是寄存器重排里最
     * 经典的一类错误（等价于交换两个变量忘了用临时变量），而且症状不是
     * 崩溃而是"参数错位"——sys_read 会拿到 (buf, len, 垃圾) 这种看起来
     * 像随机数的参数，很难一眼看出是搬移顺序的问题。
     *
     * ---- Lab7 在这里踩过的一个真实 bug，留档 ----
     *
     * Lab7 的 syscall_dispatch 只有五个参数（没有 a2），当时的写法是先
     * `mov %r8, %rcx`（把 rip_slot 塞进第四参数位 rcx），再 `mov %r9, %r8`
     * （把 rsp_slot 挪进第五参数位 r8）。第二条一旦漏掉，r8 会保持
     * "user_rip_slot 的地址"这个刚搬进 rcx 的旧值不变（没人覆写它，
     * 此刻 r8==rcx，两个寄存器指向*同一个*栈上槽位），于是
     * syscall_dispatch() 读到的 user_rsp_slot
     * 参数实际上是 user_rip_slot 的地址，两者在 sys_exec() 内部变成
     * 同一个写入目标——`*user_rip_slot = USER_PROG_VADDR;` 先写对，
     * 紧接着 `*user_rsp_slot = user_stack_top;` 因为指向同一处内存,
     * 把刚写进去的 USER_PROG_VADDR 原地覆盖成了 user_stack_top,而
     * syscall_saved_user_rsp 这个全局变量本身（真正应该被 sys_exec()
     * 写入新栈顶的地方）完全没被触碰,仍然是这次 syscall 陷入时的旧
     * 用户栈指针。返回路径 `pop %rcx`（读到的是被覆盖过的
     * user_stack_top,不是预期的 USER_PROG_VADDR)接着
     * `mov syscall_saved_user_rsp(%rip), %rsp`（读到的是没被更新的旧
     * rsp)最终 sysretq——现场变成 RIP=user_stack_top（比如
     * 0x402000,即 USER_STACK_VADDR 本身,栈顶再往上一个字节都没映射
     * 过的守护地址),RSP=一个仍然合法但已经过期的旧值,CPU 从
     * RIP=0x402000 处取指直接 #PF（not-present,read,user——指令取指
     * 本身在硬件层面就是一次"读"),现象是 exec 之后子进程立刻缺页崩溃,
     * 而 fork()（sys_fork() 不需要写这两个槽位,从来不会暴露这个问题)
     * 和普通 sys_write/sys_wait（同样不写这两个槽位)全程正常,只有
     * "调用了 exec 的那个进程,exec 生效的那一刻"才会踩中——跟参数
     * 传递 bug 完全对应。
     *
     * Lab8 把 a2 加进来之后，rip_slot/rsp_slot 各自往后挪了一位、正好
     * 落在 lea 算出来的 r8/r9 上，那两条搬移指令自然消失了，这个 bug
     * 也就没有了发生的余地。这是一个值得记下来的规律：需要手工搬移的
     * 寄存器越少，这类错误的空间就越小——能通过"把值一次算到最终位置"
     * 消掉的搬移，就别留着。 */
    mov %rdx, %rcx
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov %rax, %rdi
    push %r10           /* 第 7 个参数 gpr_snapshot，见上方注释 */
    call syscall_dispatch
    /* syscall_dispatch 的返回值在 rax（C 调用约定），恰好也是 SYSRET
     * 传回用户态返回值时约定用的寄存器（本课程沿用 Linux syscall ABI
     * 的"rax 传返回值"惯例，见 trap.c syscall_dispatch 声明处注释），
     * 不需要额外搬移。 */

    add $8, %rsp        /* 丢掉刚 push 的第 7 个参数 */
    add $8, %rsp        /* 丢掉对齐填充 */

    /* 弹出 fork() 寄存器快照，顺序跟上面 push %r15/%r14/../%rbp/%rbx
     * 完全相反（先 push 的最后 pop，这是任何 push/pop 序列要正确复原
     * 现场的基本要求，跟"是不是 gpr_snapshot"无关）——这里弹的是父
     * 进程自己的原值。子进程走的是完全独立的 uvm_copy() 拷出来的一份
     * 内核栈,跟父进程这份互不相干,父进程弹完这几行,该恢复什么就恢复
     * 什么,不受 sys_fork() 刚才往子进程 trapframe 里抄了一份同样的值
     * 这件事影响。
     *
     * ---- 这里也曾经踩过一个真实 bug，留档 ----
     *
     * push 顺序从 rbx..r15 反转成 r15..rbx 之后，这里的 pop 序列原样
     * 保留了反转前的顺序（pop %r15 在最前面）——没有意识到"pop 必须
     * 是 push 的镜像"这条约束在改 push 顺序的同时也必须跟着改 pop
     * 顺序,否则 pop 出来的值和寄存器名对不上：仍然按"先 push 的先
     * pop"的（反转前）旧规则去 pop，而不是"先 push 的最后 pop"这条
     * 一直没变过的真规则。举例说明后果：反转后的 push 顺序是 r15 先、
     * rbx 后,栈上从高地址到低地址依次是 r15,r14,r13,r12,rbp,rbx；
     * 如果 pop 顺序也写成 r15 先、rbx 后（跟这里当时的错误一样），
     * 第一条 `pop %r15` 会从栈顶（也就是 rbx 所在的位置）弹出*rbx 的
     * 值*塞进 %r15，第二条 `pop %r14` 会弹出 rbp 的值塞进 %r14,以此
     * 类推——六个寄存器的值两两之间发生了错位交换，父进程恢复执行后
     * 用的 rbx/rbp/r12-r15 全部是别的寄存器原来的值,不是自己的。
     *
     * 这类 bug 不会导致父进程立刻崩溃（六个值只是换了位置,不是丢失
     * 或清零,压栈用的 8 个槽位始终存在合法数据），症状是"父进程从
     * 这次系统调用返回之后,局部变量的值变得莫名其妙"——比子进程
     * rbp=0 那个原始 bug、或者 gpr_snapshot 指针本身算错那个 bug 更
     * 难定位，因为父进程的执行路径在这之前一直正常,崩溃或者错误结果
     * 会出现在*后面某条*恰好用到某个被换错的 callee-saved 寄存器的
     * 代码里,跟这次系统调用在源码位置上可能相距很远。这是本文件第五
     * 次印证同一条经验：push 和 pop 是一对,任何一边单独改动都必须
     * 立刻核对另一边是不是还严格镜像,不能假设"看起来顺序差不多"就
     * 足够。 */
    pop %rbx
    pop %rbp
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    pop %r11
    pop %rcx

    /* 最后一步：把用户 RSP 从本进程内核栈上那个槽位读回 %rsp。
     *
     * 此刻栈顶就是那个槽位（上面两个 pop 已经把 r11/rcx 弹掉了），所以
     * `pop %rsp` 也能做到同样的事。这里写成 mov 是因为 `pop %rsp` 要求
     * 读者去回忆"x86-64 的 pop %rsp 是先读内存再改 rsp、还是反过来"
     * ——答案是前者，结果正确，但这是一条需要查手册才能确认的指令。
     * mov (%rsp), %rsp 没有这个歧义：读栈顶那 8 个字节，写进 rsp。
     *
     * 读的是内核栈而不是 syscall_saved_user_rsp 那个全局变量,这正是
     * Lab9 修掉的那个 bug（函数头注释里"那个全局变量只能当过渡"一节)。
     * 如果这次系统调用中途 yield 过（阻塞的 sys_wait、空管道的 read、
     * 满管道的 write),期间别的进程做的任何一次系统调用都会把那个全局
     * 变量改成它自己的用户栈指针;而内核栈是每进程一份的,这个槽位从
     * 压进去到读出来只有本进程碰过。 */
    mov (%rsp), %rsp
    sysretq

/* enter_user_mode(entry, stack)：第一次从 ring0 跳进 ring3，不能用
 * sysretq——sysretq 只是"回到刚才那次 SYSCALL 调用点"的镀像，它读取
 * 的 rcx/r11 必须是*之前一次 SYSCALL 硬件自动填入*的用户 RIP/RFLAGS，
 * 这里完全没有发生过 SYSCALL，没有这两个值的合法来源，硬凑一个值
 * 塞进 rcx/r11 然后 sysretq 在语义上是错的（即使可能凑巧不崩，也是在
 * 滥用一条指令去做它不是为此设计的事）。真正对应"凭空构造一次特权级
 * 切换"的机制是 iretq——这正是 IDT 中断/异常路径回到低特权级时用的
 * 同一条指令，只是这次由软件手工搭出 iretq 期望的栈帧，而不是硬件
 * 在异常发生时自动压栈。
 *
 * iretq 从栈顶按顺序弹出五个 64 位值：RIP、CS、RFLAGS、RSP、SS——跟
 * page_fault_stub/timer_stub 里硬件自动压的栈帧形状完全一样（这不是
 * 巧合，这就是 IDT 路径本来的样子，这里只是手工搭一份等价的），意味着
 * push 的顺序必须反过来（SS 最先 push，因为它在栈帧里地址最高，最后
 * 被弹出）。 */
enter_user_mode:
    /* System V AMD64 调用约定：entry=rdi，stack=rsi——先保存到别的
     * 寄存器再开始 push，因为 push 会改变 rsp，但我们还没决定好最终
     * 要用哪个栈（这个函数从内核栈调用，执行期间应该始终留在内核栈上，
     * 直到 iretq 那一刻由硬件原子地切到栈帧里指定的用户 RSP）。 */
    mov %rdi, %rax /* entry */
    mov %rsi, %rdx /* stack */

    push $0x20 | 3 /* SS：用户数据段选择子，RPL=3（trap.c 里的
                    * GDT_SEL_USER_SS_RPL3，这里直接写字面量是因为
                    * .S 文件不方便 #include trap.c 的宏定义——两边
                    * 的数值必须保持同步，跟 user_prog.S 里手写
                    * SYS_WRITE/SYS_EXIT 字面量是同一类"跨文件手工
                    * 契约"，README 会一并指出）。 */
    push %rdx      /* RSP：用户栈顶（USER_STACK_VADDR，kernel_main.c
                    * 传进来的第二个参数）。 */
    push $0x202    /* RFLAGS：IF=1（bit 9）+ bit 1（Intel 手册规定
                    * 永远置 1 的保留位）——用户程序需要能被定时器
                    * 中断打断（不像 SYSCALL 路径故意通过 FMASK 清
                    * IF，用户态代码是可以被中断的常规执行环境）。 */
    push $(0x28 | 3) /* CS：用户代码段选择子，RPL=3（GDT_SEL_USER_CS_
                      * RPL3）。 */
    push %rax      /* RIP：用户程序入口（USER_PROG_VADDR）。 */

    iretq
