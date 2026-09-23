/* Lab6/7 riscv64：在 Lab5"单一 stvec 入口,不切栈"的基础上,第一次让
 * trap 可能从 U-mode 发起（ecall）——这打破了 Lab5 注释里写的前提
 * （"本课程目前只有内核态代码,sp 本来就已经是合法的内核栈"）,必须
 * 补一个栈切换机制,跟 x86_64 版本（../x86_64/trap_entry.S）
 * syscall_entry 的 syscall_saved_user_rsp 是同一个问题的同一种解法,
 * 只是触发方式不同——x86_64 的 SYSCALL 完全不经过 IDT,天然需要独立的
 * syscall_entry 入口；riscv64 的 ecall 和 page fault/定时器共享同一个
 * stvec 入口（Direct 模式的本质——所有 trap 只有一个跳转目标）,不能
 * 像 x86_64 那样"写一个新入口只处理 ecall",必须在*唯一的*
 * supervisor_trap_entry 里,用一条分支区分"这次 trap 从哪个特权级来的",
 * 分别决定要不要切栈。
 *
 * 怎么判断来源特权级、且*在还没有任何栈可用之前*就能做到：sstatus
 * 的 SPP 位（bit 8，riscv-privileged 规范"sstatus"一节——硬件规定的
 * 行为，不是本课程自定义：trap 发生时，硬件自动把 SPP 设成 0（来自
 * U-mode）或 1（来自 S-mode）,sret 返回时还会读它决定回哪个特权级、
 * 然后清零——这里只用它的"读"这一面，sret 那一步的语义不需要本文件
 * 操心）——csrr 是纯读 CSR 的指令，不依赖栈，可以在决定"要不要切栈"
 * 之前安全执行。
 *
 * Lab9 补上了这句话里一直缺的半句：csrr 确实不依赖栈,但它需要一个
 * *目的寄存器*,而此刻每一个通用寄存器里都躺着被打断那一方的活数据。
 * Lab4~Lab8 直接用了 t0/t1,这是一个真实的 bug（用户的 t0/t1 在被存进
 * 栈帧之前就被覆盖),只是被"用户程序是逐条手写的汇编、作者知道避开
 * t0/t1"掩盖了五个 Lab。Lab9 的用户程序第一次由 GCC 编译,寄存器分配
 * 权不在人手里,必须真正修掉：用 sscratch（riscv 给 S-mode 准备的通用
 * 暂存 CSR)先腾出一个寄存器,详见下面 supervisor_trap_entry 第一条
 * 指令处的注释。
 *
 * SPP=0（来自 U-mode，ecall,以及"定时器 tick 恰好落在用户态执行期间"
 * 这两种情况——SPP 反映的是"trap 发生那一刻 CPU 处于哪个特权级",跟
 * "trap 的类型是什么"是两个独立的问题,Lab4/5 时期只有 page fault，
 * 而 page fault 只会发生在 S-mode 自己访问非法地址时才会走到这条
 * S-mode 分支,所以那时"SPP"和"trap 类型"恰好总是绑在一起,容易被
 * 误以为是同一件事,Lab6 引入 ecall 之后两者才第一次分开)：sp 此刻是
 * *用户栈*指针，不能在上面 addi sp,sp,-TF_SIZE——跟 x86_64 侧
 * syscall_entry 注释里"用户栈的内容对内核不可信，且本 Lab 用户栈
 * 空间本来就很小"完全一样的顾虑。必须先把当前（用户）sp 找个地方存起来,
 * 再换上 trap_kernel_sp_top 指向的地址。
 *
 * "找个地方"这件事 Lab6~Lab8 用的是全局变量 trap_saved_user_sp（对应
 * x86_64 的 syscall_saved_user_rsp)——Lab9 把它删掉了,换成栈帧里的
 * TF_SP 槽位。原因和 Lab7 把 `la sp, __stack_top` 换成
 * trap_kernel_sp_top 是同一类,但走向相反：那次是"从链接期常量变成
 * 运行期全局变量",这次是"从全局变量变成 per-process 存储"。全局变量
 * 在 Lab8 之前够用,是因为那时 ecall 分支从不调用 yield()——从写入到
 * 读出之间不可能有别的进程插进来覆写。Lab9 打破了这个前提：sys_wait/
 * pipe_read/pipe_write/console_read 全都在 ecall 内部 yield(),被挂起
 * 期间别的进程每次陷入都会覆写同一个全局变量,换回来时 sret 恢复的
 * 就是别人的用户栈指针。存进栈帧之后这个问题自动消失——栈帧跟进程
 * 一对一,被挂起时整帧连同它一起冻结在自己的内核栈上。
 *
 * Lab7 新增：trap_kernel_sp_top 取代了 Lab6 的 `la sp, __stack_top`
 * 直接量——Lab6 时期只有一条执行流（内核本身),永远复用同一个固定栈
 * 顶是安全的；Lab7 起每个进程都有自己独立的内核栈（proc.h/proc.c),
 * scheduler() 在 swtch() 进某个进程之前会把这个全局变量指向那个进程
 * 自己的内核栈顶（跟 x86_64 版本 syscall_kernel_rsp 是同一个变量,同一个
 * "调度器负责写入,trap 入口负责读取"分工),trap 入口这里只负责*读*,
 * 不关心它当前具体指向谁的栈——这是本文件相对 Lab6 唯一的行为改动，
 * 下面"保存寄存器->call->恢复寄存器"这一段（S-mode 来源和 U-mode 来源
 * 汇合之后的部分）逐字节保持 Lab5/6 已经验证过的样子不变。
 *
 * 不用全局变量固定值而是"取代直接量"这个改法背后的教训见 proc.c/
 * kernel_main.c 对应位置——如果这一步漏做（比如 Lab6 遗留的
 * `la sp, __stack_top` 没换掉）,每个进程的 ecall/定时器陷入都会切到
 * *同一个*共享栈,一旦某个进程的系统调用处理函数自己又触发 swtch()
 * （比如 sys_exit_proc()),这条调用链在共享栈上压栈的深度一旦超过
 * scheduler() 挂起状态存放的位置,就会直接覆盖 scheduler() 保存的
 * 返回地址——这正是 x86_64 版本 proc.c scheduler() 注释里详细描述过的
 * 那个真实故障（"RIP=0/CR2=0 的取指缺页"),riscv64 这里是同一个根因,
 * 同一种修法。
 *
 * SPP=1（来自 S-mode，本课程范围内只有"内核自己访问非法地址触发的
 * page fault"会走到这里，Lab4/5 原有路径,一律视为不可恢复错误直接
 * panic——不会经过 yield()/swtch(),因此不需要 per-process 栈这层
 * 考虑,复用当前 sp 本身即可)：sp 已经是合法内核栈，跟 Lab5 完全一样，
 * 不做任何切换——下面两个分支在"保存寄存器->call->恢复寄存器"这一段
 * 用的是完全相同的一份代码（no fallthrough duplication，两条路径
 * 汇合到同一段 body），保证 S-mode 来源时的指令序列跟 Lab5 逐字节
 * 等价，不会因为新增 U-mode 分支而意外改变这条已经验证过的路径。
 *
 * 出口不再有分支（Lab6~Lab8 这里有一条 `bnez t1, do_sret`,依赖 t1
 * 把入口那次 SPP 判断结果跨越 call 带到出口)：sp 无条件从 TF_SP 槽位
 * 恢复,对两种来源都恰好正确——U-mode 来源时槽位里是用户栈指针,
 * S-mode 来源时是开帧之前的内核 sp,数值上正好等于原来 `addi sp, sp,
 * 128` 的结果。这不是巧合,是"把值从 CPU 全局存储搬到 per-process
 * 存储"附带的简化：原来必须分支,是因为全局变量里的值只对 U-mode 来源
 * 有意义；现在值在自己帧里,来源特权级不再影响"该从哪里取 sp"。
 *
 * 这个轻量栈帧本来（Lab4~Lab9 前半段)不需要跟着 struct trapframe
 * （proc.h)一起扩到 34 个字段：本文件这段"保存->call->恢复"序列描述的
 * 是"一条正在执行的内核流,被 trap 短暂打断,处理完了原样恢复"这件事,
 * 从头到尾都停留在*同一条*执行流内部——即使这次 trap 内部调用链一路
 * 深入到 yield()->swtch(),那也只是把这条执行流冻结在它自己的内核栈上
 * （连同这里已经保存的 ra/t0-t6/a0-a7,和 C 函数调用规则下 gcc 自己在
 * 更深栈帧里保存的 s0-s11)——将来 scheduler() 再 swtch() 回这个进程时,
 * 恢复的正是"回到 yield() 内部那次 swtch() 调用刚返回之后"这一点,
 * 一路 return 回 supervisor_trap_handler、回到这里,走的正是下面这段
 * "ld 恢复->sret"逻辑,不需要经过 struct trapframe/trap_return——那条
 * 路径只在"一个进程从未运行过、第一次被 swtch() 进去"这一种场景下
 * 才会被用到（proc_alloc_skeleton() 把 context.ra 设成 trap_return,
 * 首次 swtch() 的 ret 落进那段代码,由它去恢复一份手工摆好的完整
 * trapframe),跟本文件这段轻量帧处理的是两个完全不同的场景,互不冲突,
 * 也不需要合并成同一套逻辑——这跟 x86_64 版本 page_fault_stub/
 * timer_stub 保持 Lab5 的 15 寄存器轻量帧不变、trap_return 独立处理
 * 完整 trapframe,是完全相同的分工。这段论证对"resume 同一条执行流"
 * 这一种场景始终是对的,一个字都不用改。
 *
 * 但 Lab9 的 sys_fork() 撕开了一个这段论证完全没设防的缺口：fork() 的
 * ecall 处理过程*不是* resume 同一条执行流,而是要在 proc.c 里从零
 * *合成*另一个进程（子进程)的完整 struct trapframe，数据来源正是这次
 * ecall 自己在栈上开的这份轻量帧（trap.c sys_fork() 通过 syscall_
 * dispatch 转发过来的 caller_frame 指针)。子进程的 struct trapframe
 * 不会经过任何"gcc 自动帮你把 s0-s11 存好"的路径——它是被 memset()
 * 清零之后手工摆出来的一份*全新*现场,如果轻量帧里没有 s0-s11 可抄,
 * 子进程的 s0-s11 就只能停留在 0。
 *
 * 这不是假设性的缺口,是实测复现过的真实 bug：本课程 Lab9 的用户程序
 * 第一次由 GCC 编译（不再是手写汇编),user/init.c 的 main() 被编译成
 * "用 s0 当帧指针访问栈上局部变量"（`sw a5, -20(s0)` 这类指令,fork()
 * 返回值先经 a0->a5 搬运,再靠 s0 相对偏移存进栈上的 pid 局部变量)。
 * 子进程 s0=0 时,这条 fork() 返回后的第一条store 指令算出的地址是
 * `0 + (-20)`,也就是 `0xffffffffffffffec`（-20 的无符号 64 位表示),
 * 直接触发一次 "page fault: addr=0xffffffffffffffec reason=write"——
 * 用 GDB 断在 trap.c 的故障诊断行能看到此刻 g_current 正是 fork() 的
 * 子进程,sepc 落在 init 的 main() 里 `sw a5, -20(s0)` 那条指令上,跟
 * objdump 反汇编逐条对得上。手写汇编时代（user_prog.S)这个缺口从来
 * 不会暴露,是因为作者从不用 s 寄存器当帧指针,不是因为缺口不存在。
 *
 * 修法：把这份轻量帧从 18 个槠位（144 字节)再扩 14 个——s0-s11 十二个,
 * 加上 gp/tp 两个,凑齐 RISC-V calling convention 里*完整*的 callee-saved
 * 集合（跟 struct context 保存的是同一组,只是 struct context 服务
 * "swtch() 切换到另一条完全不同的执行流"这个场景,这里服务"合成一份
 * 全新的 struct trapframe"这个场景，两个场景第一次需要同一组数据)。
 * gp/tp 严格来说不是"每次 ecall 都需要保护"的东西（本课程范围内 gp
 * 从未被使用、tp 更是从未初始化,见 crt0_riscv64.S 对 gp 的说明),但
 * 既然已经要扩帧、已经要给 sys_fork() 传完整的 callee-saved 集合,补
 * 齐这两个槠位是"跟 RISC-V ABI 定义完全对齐"这个更简单的心智模型,
 * 不需要再解释"为什么这两个不算在内"。
 *
 * 代价：每一次 trap（不只是 fork,包括跟 fork 完全无关的 page fault/
 * 定时器/其它系统调用)都要多付 14 条 sd + 14 条 ld 的开销——这份轻量帧
 * 对*所有* trap 类型是同一段代码,没有"只在 SYS_FORK 时才多存几个"这种
 * 按需的路径（ecall_handler 分发到具体是哪个系统调用号,发生在保存动作
 * *之后*、C 代码里，trap_entry.S 汇编层完全不知道这次 trap 会不会是
 * fork)。本课程的取舍是接受这份统一开销,换来"轻量帧对所有消费者
 * （syscall_dispatch 的六个分支、page fault 诊断、sys_fork())完整可靠"
 * 这个更简单的心智模型,不引入"轻量帧本身还分'基础版'和'完整版'两种"
 * 的复杂度。
 *
 * 但这条"冻结在自己栈上"的论证只覆盖通用寄存器（ra/t0-t6/a0-a7/
 * s0-s11),不覆盖 sepc——sepc 不是寄存器,是一个 CSR,这段代码全程不
 * 把它读出来存进栈帧,它只在最后一条 `sret` 执行的那一刻被硬件读取,
 * 读到的是"当前 sepc CSR 此刻的值"。如果这次 trap 处理过程中经过了
 * yield()->swtch()（定时器 tick 走的正是这条路径),在 swtch() 让出
 * CPU 到这个进程被重新调度回来这段时间里,*其它*进程自己的每一次 trap
 * 都会在硬件层面覆写同一个 sepc CSR（per-hart,不是 per-process）,
 * 这个进程终于被 swtch() 回来时 sret 读到的会是别的进程留下的值,不是
 * 自己被打断时的那个 sepc。
 *
 * Lab9 在这里给 sepc 开了第 17 个槽位（TF_SEPC),入口 csrr 存入、
 * 出口 csrw 写回,跟 t0-t6 用同一套"冻结在自己栈上"的逻辑。这件事
 * Lab6~Lab8 尝试过一次并且失败了,失败的经过值得留在这里,因为它是
 * 一个很典型的"同一个值有两个存储位置"故障:
 *
 *   trap.c 的 ecall 分支需要在 `call supervisor_trap_handler` *内部*、
 *   C 代码执行期间把 sepc 显式 +4（跳过 ecall 指令本身,否则 sret 会
 *   回到刚才那条 ecall 再执行一次)。当时 trap.c 写的是 CSR
 *   （write_sepc(read_sepc()+4)),而本文件出口写的是"入口那次 csrr
 *   快照"——两处写同一个逻辑值的两份不同存储,出口的写在后,于是 +4
 *   被原样冲掉,sret 跳回那条 ecall,同一个 ecall 无限重复执行。实测
 *   复现：`qemu-system-riscv64 -d int` 会看到同一个 user_ecall 的 epc
 *   反复出现,这条 ecall 本该只执行一次的效果（比如打印一行消息)跟着
 *   重复,状态被搞乱后往往接着触发一次不相关的 page fault,第一眼很难
 *   看出根因是 sepc 被写回了一份过期的值。
 *
 *   Lab8 当时的修法是退一步：本文件不碰 sepc,改由
 *   timer_interrupt_handler() 自己在调用 yield() 前后手工存取
 *   g_current->tf->sepc。这修好了定时器路径,代价是 sepc 的保护逻辑
 *   散落在 C 代码里、且只覆盖"有人记得加这段代码"的那条路径。
 *
 * Lab9 的修法方向反过来：不是让出口迁就 trap.c,而是让 trap.c 不再碰
 * CSR——ecall_handler 改成 `frame[16] += 4`,+4 直接落在槽位上,出口
 * 无条件写回的就是已经 +4 过的值。同一个逻辑值只剩*一个*存储位置,
 * CSR 只在 sret 那一刻被硬件读一次,冲突从根上不存在了。
 * timer_interrupt_handler() 里那段手工存取 g_current->tf->sepc 的代码
 * 随之整段删除——槽位天生就在这个进程自己的内核栈上,不需要任何一条
 * 路径特殊照顾。
 *
 * 这是 riscv64 侧第三个、也是最后一个"CPU 全局存储装 per-process 值"
 * 类 bug 的修复（前两个：Lab7 的 trap_kernel_sp_top 从固定栈顶变成
 * 调度器写入的变量,Lab9 的 trap_saved_user_sp 从全局变量变成 TF_SP
 * 槽位)。三个的症状完全不同（栈覆盖 / 返回到别人的用户栈 / 返回到
 * 别人的 PC),根因是同一句话：一个值如果对每个进程都不一样,它就不能
 * 住在整个 CPU 只有一份的地方。 */
.section .text
.global supervisor_trap_entry
.align 4

#define SSTATUS_SPP (1 << 8)

/* 栈帧槽位的符号名。Lab4~Lab8 这里是 16 个裸数字偏移,Lab9 槽位涨到 18
 * 个、并且其中两个（TF_SEPC/TF_SP)在入口和出口被*不同*的指令序列碰到,
 * 裸数字开始容易错位,换成符号名。数值本身没变——TF_RA..TF_A7 仍然是
 * 0..120,所以 trap.c 里 frame[0]..frame[15] 那套下标契约逐字有效,
 * Lab9 只是在末尾追加了 frame[16]/frame[17]。 */
#define TF_RA     0
#define TF_T0     8
#define TF_T1     16
#define TF_T2     24
#define TF_T3     32
#define TF_T4     40
#define TF_T5     48
#define TF_T6     56
#define TF_A0     64
#define TF_A1     72
#define TF_A2     80
#define TF_A3     88
#define TF_A4     96
#define TF_A5     104
#define TF_A6     112
#define TF_A7     120
#define TF_SEPC   128   /* frame[16]：Lab9 新增 */
#define TF_SP     136   /* frame[17]：Lab9 新增 */
#define TF_S0     144   /* frame[18]：Lab9 二次新增,见下方 save_rest 里
                          * s0-s11/gp/tp 那一段注释 */
#define TF_S1     152
#define TF_S2     160
#define TF_S3     168
#define TF_S4     176
#define TF_S5     184
#define TF_S6     192
#define TF_S7     200
#define TF_S8     208
#define TF_S9     216
#define TF_S10    224
#define TF_S11    232
#define TF_GP     240
#define TF_TP     248
#define TF_SIZE   256   /* 32*8 = 256,16 的倍数,不需要补 pad */

supervisor_trap_entry:
    /* 第一条指令就把 t0 挪进 sscratch。sscratch 是 riscv-privileged
     * 规范给 S-mode 准备的一个通用暂存 CSR,硬件对它的内容没有任何解释,
     * 存在的理由正是本文件这一刻的处境：trap 刚发生,还没有任何可信的栈,
     * 想读 sstatus 判断来源特权级就得先有一个空闲寄存器,而此刻*每一个*
     * 通用寄存器里躺着的都是被打断那一方的活数据。csrw 是寄存器↔CSR
     * 之间的搬运,不碰内存、不需要栈,是唯一能"凭空腾出一个寄存器"的手段。
     *
     * Lab4~Lab8 这里是 `csrr t1, sstatus` 直接开始——那是一个真实的
     * bug,只是被"用户程序是手写汇编"掩盖了 5 个 Lab：t1（以及紧接着
     * 被用作地址暂存的 t0)在被存进栈帧*之前*就被覆盖了,如果被打断的
     * 用户代码此刻正在 t0/t1 里存着活值,那个值就永久丢了,用户程序从
     * ecall 返回后会读到内核留下的垃圾。Lab8 之前的用户程序（user_prog.S）
     * 是逐条手写的,作者知道有这个坑就避开了 t0/t1；Lab9 的用户程序第一次
     * 由 GCC 编译（-O2),寄存器分配权在编译器手里,而 ulib 里 ecall 是
     * 一段内联汇编,编译器完全可以把一个跨越它的活值放在 t0——这个 bug
     * 从"凑巧不触发"变成"随时可能触发",必须在 Lab9 修掉。
     *
     * 为什么不担心 sscratch 原来的值被冲掉：本课程从不在别处使用
     * sscratch,它进入这段代码之前的内容是死数据。也不担心"下面这几条
     * 指令之间又来一次 trap 把 sscratch 覆写":trap 只可能由中断或异常
     * 引发,而 S-mode trap 入口一进来硬件就清了 sstatus.SIE(中断关掉),
     * 这段窗口里唯一能触发异常的指令是 `ld sp, 0(sp)`（访问一个内核
     * 全局变量,永远映射着)——窗口是安全的。真实的多核内核会用 sscratch
     * 存 per-hart 结构体指针,那时这里需要换成 xv6 的做法（用户态/内核态
     * 两个 stvec 入口),但那属于多核话题,不在本课程范围内。 */
    csrw sscratch, t0
    csrr t0, sstatus
    andi t0, t0, SSTATUS_SPP
    bnez t0, from_supervisor

from_user:
    /* SPP=0：sp 是用户栈指针，不能在它上面 addi sp,sp,-TF_SIZE——跟
     * x86_64 侧 syscall_entry 注释里"用户栈的内容对内核不可信，且本 Lab
     * 用户栈空间本来就很小"完全一样的顾虑。
     *
     * 此刻 t0 寄存器里是上面 andi 的结果（0,死值),用户原本的 t0 在
     * sscratch 里。csrrw 一条指令同时完成"把 sscratch 的旧值读进 t0"
     * 和"把用户 sp 写进 sscratch"两件事,交换之后：t0=用户 t0,
     * sscratch=用户 sp,两个需要保存的值都还活着,而 sp 寄存器本身已经
     * 可以随便覆盖了。
     *
     * Lab7 起 trap_kernel_sp_top 是全局变量（proc.c 里 scheduler() 每次
     * swtch() 进某个进程之前写入),不再是 Lab6 的 `la sp, __stack_top`
     * 直接量,所以是"取地址再 ld 出内容"两条指令。这里 la/ld 的目标寄存器
     * 都是 sp 自己——sp 当前的值已经无用,拿它当地址暂存寄存器正好,省下
     * 一个本来就没有的空闲寄存器。这一步换来的是"每个进程用自己的内核栈"
     * 这个 Lab7 的核心机制,见本文件顶部模块注释里"共享栈覆盖"故障的
     * 完整描述。 */
    csrrw t0, sscratch, sp
    la sp, trap_kernel_sp_top
    ld sp, 0(sp)
    addi sp, sp, -TF_SIZE
    sd t0, TF_T0(sp)
    csrr t0, sscratch
    sd t0, TF_SP(sp)
    j save_rest

from_supervisor:
    /* SPP=1：sp 已经是合法内核栈（本课程范围内只有"内核自己访问非法
     * 地址触发的 page fault"走这条路,Lab4/5 原有路径,一律 panic）,
     * 不需要切栈。但 TF_SP 槽位仍然要填——出口不再有分支,统一从 TF_SP
     * 恢复 sp（见下面出口那段注释),所以这里存进去的必须是"trap 发生
     * 那一刻的 sp",也就是开帧之前的值。 */
    mv t0, sp
    addi sp, sp, -TF_SIZE
    sd t0, TF_SP(sp)
    csrr t0, sscratch
    sd t0, TF_T0(sp)

save_rest:
    /* 两条分支在这里汇合。TF_T0/TF_SP 两个槽位已经由各自分支填好,
     * t0 寄存器现在是自由的暂存器。剩下 15 个通用寄存器 + sepc 的
     * 保存序列对两种来源完全相同——跟 Lab4~Lab8 一样,S-mode 来源时
     * 走的指令序列和 U-mode 来源时是同一份代码,不存在"只在某一条
     * 路径上验证过"的半截逻辑。 */
    sd ra,  TF_RA(sp)
    sd t1,  TF_T1(sp)
    sd t2,  TF_T2(sp)
    sd t3,  TF_T3(sp)
    sd t4,  TF_T4(sp)
    sd t5,  TF_T5(sp)
    sd t6,  TF_T6(sp)
    sd a0,  TF_A0(sp)
    sd a1,  TF_A1(sp)
    sd a2,  TF_A2(sp)
    sd a3,  TF_A3(sp)
    sd a4,  TF_A4(sp)
    sd a5,  TF_A5(sp)
    sd a6,  TF_A6(sp)
    sd a7,  TF_A7(sp)

    /* Lab9 新增：把 sepc 也存进栈帧。这正是本文件顶部模块注释里
     * Lab6~Lab8 版本明确写过"尝试过、放弃了"的那个第 17 个槽位——
     * 当时放弃的唯一理由是出口无条件 csrw 回去会把 trap.c 刚做的
     * "sepc+=4 跳过 ecall 指令"冲掉,形成同一条 ecall 无限重复的死循环。
     *
     * Lab9 把这个矛盾解决掉了,而且方向是反过来的：不是让出口迁就
     * trap.c,而是让 trap.c 不再碰 CSR——ecall_handler 改成 `frame[16]
     * += 4`（trap.c 里那一行),+4 直接落在这个槽位上,出口无条件写回的
     * 就是已经 +4 过的值。两边不再各写一处存储,只有*一个*存储位置
     * （槽位),CSR 只在 sret 那一刻被硬件读一次。
     *
     * 换来的是第三个"全局变量过期"类 bug 的修复（前两个是
     * trap_kernel_sp_top 和 trap_saved_user_sp)：sepc 是 per-hart CSR,
     * 定时器 tick 打断用户态之后如果调用链走到 yield()->swtch(),这个
     * 进程被挂起期间别的进程每次 trap 都会覆写同一个 sepc,它被换回来
     * sret 时读到的就是别人的返回地址。Lab8 的修法是让
     * timer_interrupt_handler() 自己在 yield() 前后手工存取
     * g_current->tf->sepc；Lab9 有了槽位之后那段手工代码可以整段删掉——
     * 槽位天生就在这个进程自己的内核栈上,跟 t0-t6 同一套"冻结在自己
     * 栈上"的逻辑,不需要任何一方特殊照顾。 */
    csrr t0, sepc
    sd t0, TF_SEPC(sp)

    /* Lab9 二次新增：s0-s11/gp/tp 十四个槠位。理由见本文件顶部模块
     * 注释里"sys_fork() 撕开的缺口"那一大段——这十四个寄存器对"resume
     * 同一条执行流"这个场景本来就不需要保护（gcc 自己会存),只有
     * sys_fork() 合成子进程 struct trapframe 这一种场景才需要从这里
     * 读出去,但保存动作不区分场景,对每一次 trap 都执行。 */
    sd s0,  TF_S0(sp)
    sd s1,  TF_S1(sp)
    sd s2,  TF_S2(sp)
    sd s3,  TF_S3(sp)
    sd s4,  TF_S4(sp)
    sd s5,  TF_S5(sp)
    sd s6,  TF_S6(sp)
    sd s7,  TF_S7(sp)
    sd s8,  TF_S8(sp)
    sd s9,  TF_S9(sp)
    sd s10, TF_S10(sp)
    sd s11, TF_S11(sp)
    sd gp,  TF_GP(sp)
    sd tp,  TF_TP(sp)

    /* supervisor_trap_handler(uint64_t *frame)：跟 Lab4/5 的零参数
     * 签名不同（见 trap.c ecall_handler 上方注释里"折中方案"那段的
     * 完整解释)——这里把当前 sp（栈帧基址,刚保存完的 16 个寄存器就
     * 从这里开始)搬进 a0,C 函数侧按 frame[N] 下标访问,不依赖任何
     * 具体寄存器名,只依赖"sd 的顺序"这一份契约,跟 trap.c 里的下标
     * 注释一一对应。mv 而不是 sd/直接传：a0 本身也已经被上面 `sd a0,
     * 64(sp)` 存进了栈帧,这里的 mv 只是把 sp 的值*读*到 a0 寄存器里
     * 当函数参数传走,不会覆盖栈帧里刚保存的 a0 副本（栈帧和寄存器是
     * 两个独立的存储位置),call 返回之后下面 `ld a0, 64(sp)` 会把
     * a0 寄存器还原成用户程序真正的 a0（如果是 ecall,这个槽位可能
     * 已经被 ecall_handler 改写成返回值,取决于 trap.c 那边的行为,
     * 这里只负责搬运,不关心内容)。 */
    mv a0, sp
    call supervisor_trap_handler

    /* sepc 先写回 CSR,趁 sp 还指着栈帧。这一步必须在下面恢复通用寄存器
     * *之前*或*之中*完成——`ld sp, TF_SP(sp)` 一执行完,栈帧就再也找不到
     * 了（sp 已经指向别处,没有任何寄存器还留着帧基址)。 */
    ld t0, TF_SEPC(sp)
    csrw sepc, t0

    /* Lab9 二次新增：s0-s11/gp/tp 的恢复,跟保存那一段是同一批槠位的
     * 反向操作。放在这里（跟 t1-t6/a0-a7 一起、sp 恢复之前)而不是放到
     * 最后,是因为它们跟 t0/sp 之间没有"谁先恢复会毁掉另一个的地址来源"
     * 这种依赖——所有这些 ld 都以*当前* sp 为基址,只要 sp 自己留到最后
     * 恢复,顺序在其余寄存器之间随意。 */
    ld s0,  TF_S0(sp)
    ld s1,  TF_S1(sp)
    ld s2,  TF_S2(sp)
    ld s3,  TF_S3(sp)
    ld s4,  TF_S4(sp)
    ld s5,  TF_S5(sp)
    ld s6,  TF_S6(sp)
    ld s7,  TF_S7(sp)
    ld s8,  TF_S8(sp)
    ld s9,  TF_S9(sp)
    ld s10, TF_S10(sp)
    ld s11, TF_S11(sp)
    ld gp,  TF_GP(sp)
    ld tp,  TF_TP(sp)

    ld ra,  TF_RA(sp)
    ld t1,  TF_T1(sp)
    ld t2,  TF_T2(sp)
    ld t3,  TF_T3(sp)
    ld t4,  TF_T4(sp)
    ld t5,  TF_T5(sp)
    ld t6,  TF_T6(sp)
    ld a0,  TF_A0(sp)
    ld a1,  TF_A1(sp)
    ld a2,  TF_A2(sp)
    ld a3,  TF_A3(sp)
    ld a4,  TF_A4(sp)
    ld a5,  TF_A5(sp)
    ld a6,  TF_A6(sp)
    ld a7,  TF_A7(sp)

    /* 最后两条：t0 和 sp。顺序是被迫的——两条 ld 都拿 sp 当基址寄存器,
     * 谁先恢复谁就毁掉另一条的地址来源,所以 sp 必须是*最后*一个,而
     * t0 只能排在它前面。
     *
     * Lab9 的收获：这里不再有 `bnez t1, do_sret` 那条出口分支,也不再有
     * trap_saved_user_sp 这个全局变量。两种来源统一成一条路径——TF_SP
     * 槽位里存的是"这次 trap 发生那一刻 sp 是多少",U-mode 来源时它是
     * 用户栈指针,S-mode 来源时它是开帧之前的内核 sp（正好等于
     * 帧基址+TF_SIZE,跟原来 `addi sp, sp, 128` 的效果逐字节相同),
     * 无条件 ld 回来对两种情况都恰好正确。
     *
     * 这是把值从"CPU 全局存储"搬到"per-process 存储"之后附带的简化：
     * 原来的分支存在,是因为全局变量 trap_saved_user_sp 里的值只对
     * U-mode 来源有意义,S-mode 来源时它属于别人,碰不得；现在值在自己的
     * 栈帧里,来源特权级不再影响"该从哪里取 sp"这个问题,分支也就没有
     * 存在的理由了。入口那次 SPP 判断依然需要（决定要不要换栈),但它的
     * 结果不必再跨越 call 活到出口,所以 Lab8 注释里"t1 保存了入口那次
     * SPP 判断结果,跨越 supervisor_trap_handler 调用"这件事在 Lab9
     * 已经不存在——SPP 的判断结果在入口那两条分支里就地用完了。 */
    ld t0, TF_T0(sp)
    ld sp, TF_SP(sp)
    sret

/* enter_user_mode 已在 Lab7 移除：Lab6 时期它是"第一次从 S-mode 跳进
 * U-mode"的唯一手段（kernel_main.c 直接调用一次)——riscv64 的 sret
 * 不像 x86_64 sysretq 那样依赖"之前必须发生过一次真正的陷入",可以
 * 被直接手写 sepc/sstatus 后拿来用,这也是它当时存在的全部原因。
 *
 * Lab7 引入 struct trapframe/trap_return（swtch.S)之后,"第一次启动
 * 一个进程"和"resume 一个之前被打断的进程"统一成同一条路径：
 * proc_alloc_skeleton() 把 context.ra 设成 trap_return 的地址,首次
 * swtch() 进某个进程时,ret 直接落进 swtch.S 的 trap_return,从那个
 * 进程自己 struct trapframe 里恢复 sepc/sstatus/sp 然后 sret——跟
 * enter_user_mode 手工 csrw sepc/csrw sstatus/mv sp 做的是完全相同
 * 的三件事,只是数据来源从"函数参数寄存器"变成了"内存里的 struct
 * trapframe 字段",不再需要一个专门的独立函数。这正是 proc.h 顶部
 * 模块注释、swtch.S trap_return 那段注释反复提到的"riscv64 的 sret
 * 第一次和第 N 次用的是同一条指令"这个设计优势的具体体现——x86_64
 * 版本因为 sysretq 无法承担"第一次"这个角色,即使引入了 trap_return,
 * 仍然必须保留 enter_user_mode 独立存在（../x86_64/trap_entry.S),
 * riscv64 这边则可以彻底删掉,是两边这一处差异在代码结构上的直接
 * 后果。 */
