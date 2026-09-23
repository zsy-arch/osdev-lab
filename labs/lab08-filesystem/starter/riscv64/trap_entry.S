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
 * SPP=0（来自 U-mode，ecall,以及"定时器 tick 恰好落在用户态执行期间"
 * 这两种情况——SPP 反映的是"trap 发生那一刻 CPU 处于哪个特权级",跟
 * "trap 的类型是什么"是两个独立的问题,Lab4/5 时期只有 page fault，
 * 而 page fault 只会发生在 S-mode 自己访问非法地址时才会走到这条
 * S-mode 分支,所以那时"SPP"和"trap 类型"恰好总是绑在一起,容易被
 * 误以为是同一件事,Lab6 引入 ecall 之后两者才第一次分开)：sp 此刻是
 * *用户栈*指针，不能在上面 addi sp,sp,-128——跟 x86_64 侧
 * syscall_entry 注释里"用户栈的内容对内核不可信，且本 Lab 用户栈
 * 空间本来就很小"完全一样的顾虑。做法也完全对应：先把当前（用户）
 * sp 存进一个全局变量（trap_saved_user_sp，对应 x86_64 的
 * syscall_saved_user_rsp，同样用全局变量不用压栈——这一刻还没有可信
 * 的栈可以压），再换上 trap_kernel_sp_top 指向的地址。
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
 * 出口对称：t1 保存了入口那次 SPP 判断结果，跨越 supervisor_trap_
 * handler 调用——t1 属于 caller-saved（riscv 调用约定），普通 C 函数
 * 完全可能踩它，所以 t1 本身也要跟 t0-t6 一样存进栈帧，call 返回后
 * 从栈帧里读回来，不能指望它在 call 前后保持不变。只有这个值非零
 * （来源 U-mode）才把 sp 换回 trap_saved_user_sp；否则 sp 全程没变过，
 * 不需要恢复。
 *
 * 为什么这个 128 字节的轻量栈帧不需要跟着 struct trapframe（proc.h)
 * 一起扩到 34 个字段：本文件这段"保存->call->恢复"序列描述的是
 * "一条正在执行的内核流,被 trap 短暂打断,处理完了原样恢复"这件事,
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
 * 完整 trapframe,是完全相同的分工。
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
 * 这个真实问题*不*在本文件解决——本来尝试过在这里给 sepc 单独开一个
 * 第 17 个栈帧槽位（入口 csrr 存入,出口 csrw 写回,跟 t0-t6 用同一套
 * "冻结在自己栈上"逻辑),结果暴露了一个更根本的时序冲突：trap.c 的
 * ecall 分支需要在 `call supervisor_trap_handler` *内部*、C 代码执行
 * 期间把 sepc 显式 +4（跳过 ecall 指令本身,见 trap.c write_sepc 调用
 * 处的注释),这一步执行的时间点必然晚于本文件入口那次 csrr 快照——
 * 如果出口无条件从入口快照写回 sepc,等于把 trap.c 刚做的 +4 调整原样
 * 冲掉,sret 又跳回*那条 ecall 指令本身*,再执行一次,形成同一个 ecall
 * 被无限次重复执行的死循环——实测复现：`qemu-system-riscv64 -d int`
 * 会看到同一个 user_ecall 的 epc 反复出现,而这条 ecall 原本应该只
 * 执行一次的效果（比如打印一行消息)会跟着重复,状态被搞乱后往往接着
 * 触发一次不相关的 page fault,不容易第一眼看出真正的根因是 sepc
 * 被写回了一份过期的值。
 *
 * 真正的修法把职责放回 timer_interrupt_handler()（trap.c)自己身上：
 * 只有它内部会调用 yield()、只有它需要关心"detour"这件事,由它在
 * 调用 yield() 之前把当前 sepc 存进 g_current->tf->sepc（这个字段
 * proc.h 里本来就存在,平时只在 trap_return 首次启动一个进程时被
 * 使用,这里借用它当"防跨进程覆写"的容器,不冲突),yield() 内部的
 * swtch() 返回之后再从 g_current->tf->sepc 读回来 csrw 写回 CSR——
 * 完整推导见 trap.c timer_interrupt_handler() 上方注释。ecall 分支
 * 从不调用 yield(),不存在 detour,trap.c 里 write_sepc(read_sepc()+4)
 * 这一行本身已经足够,不需要额外的保护。 */
.section .text
.global supervisor_trap_entry
.align 4

#define SSTATUS_SPP (1 << 8)

supervisor_trap_entry:
    /* 读 SPP，此刻还完全不能碰栈——t0/t1 是唯一在"决定要不要切栈"这
     * 一步之前就需要用到的寄存器，两条分支汇合之后会把它们跟其它
     * t0-t6 一视同仁地存进栈帧，不会有任何寄存器被跳过保存。 */
    csrr t1, sstatus
    andi t1, t1, SSTATUS_SPP
    bnez t1, from_supervisor

from_user:
    /* SPP=0：sp 是用户栈指针，先存进全局变量（不压栈，理由见上面
     * 模块注释），再换上*当前进程自己的*内核栈顶——顺序不能颠倒：
     * 如果先换栈，原来的用户 sp 值就丢了，没有第二个地方能找回它。
     *
     * Lab7 改动点：trap_kernel_sp_top 是一个全局变量（proc.c 里
     * scheduler() 每次 swtch() 进某个进程之前写入),不再是 Lab6 的
     * `la sp, __stack_top` 直接量——两条 `la`/`ld` 指令的字节数完全
     * 一样,只是 __stack_top 是链接期常量地址,trap_kernel_sp_top 是
     * 运行期可写的变量,需要先取它的地址再 ld 出它存的值，比直接
     * `la sp, __stack_top` 多一步,这一步换来的是"每个进程用自己的
     * 内核栈"这个 Lab7 的核心机制,见本文件顶部模块注释里"共享栈
     * 覆盖"故障的完整描述。 */
    la t0, trap_saved_user_sp
    sd sp, 0(t0)
    la t0, trap_kernel_sp_top
    ld sp, 0(t0)

from_supervisor:
    addi sp, sp, -128
    sd ra,  0(sp)
    sd t0,  8(sp)
    sd t1,  16(sp)
    sd t2,  24(sp)
    sd t3,  32(sp)
    sd t4,  40(sp)
    sd t5,  48(sp)
    sd t6,  56(sp)
    sd a0,  64(sp)
    sd a1,  72(sp)
    /* 已经写好，不是 TODO——但值得停下来看一眼：
     *
     * Lab8 给 syscall_dispatch 加了第三个参数 a2（sys_read 需要 fd/buf/len
     * 三个），而**这个文件一行都不用改**。下面这条 `sd a2, 80(sp)` 从 Lab4
     * 起就一直在这儿，ecall_handler 只需要多读一个已经存在的栈帧槽位。
     *
     * x86_64 那边则要动 trap_entry.S 的寄存器搬移序列（见
     * ../x86_64/trap_entry.S 的 TODO 1）。
     *
     * 这不是哪个架构"设计得更好"，而是两边当初做了不同的取舍：riscv64 侧
     * 一开始就按"完整参数寄存器组"保存——照 ABI 划线，不按当时的需要裁剪；
     * x86_64 侧的 SYSCALL 路径按"当时实际用到的寄存器"保存。前者多花了几条
     * sd 指令（每次陷入都花），换来后续扩展不动汇编。
     *
     * 这类取舍在内核里到处都是，而且往往要等到"后来加需求"时才看得出代价
     * 落在哪边。本课程记下这一处，是因为它是同一个改动在两个架构上成本
     * 相差最悬殊的一次。 */
    sd a2,  80(sp)
    sd a3,  88(sp)
    sd a4,  96(sp)
    sd a5,  104(sp)
    sd a6,  112(sp)
    sd a7,  120(sp)

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

    ld ra,  0(sp)
    ld t0,  8(sp)
    ld t1,  16(sp)
    ld t2,  24(sp)
    ld t3,  32(sp)
    ld t4,  40(sp)
    ld t5,  48(sp)
    ld t6,  56(sp)
    ld a0,  64(sp)
    ld a1,  72(sp)
    ld a2,  80(sp)
    ld a3,  88(sp)
    ld a4,  96(sp)
    ld a5,  104(sp)
    ld a6,  112(sp)
    ld a7,  120(sp)
    addi sp, sp, 128

    /* t1 是刚从栈帧恢复的值（上面 `ld t1, 16(sp)`），跟入口保存进去的
     * 是同一个 SPP 判断结果——如果 supervisor_trap_handler 内部改了
     * sstatus（本 Lab 不会，但不依赖这个假设，直接用保存下来的副本
     * 更稳妥），出口分支判断依据仍然是"这次 trap 刚进来的时候来自
     * 哪个特权级"，不是"处理完之后 sstatus 变成了什么"。
     *
     * 分支目标改名为 do_sret（Lab6 时期这里叫 trap_return）：Lab7 的
     * swtch.S 引入了一个语义完全不同、但同样需要叫"trap_return"的
     * 全局符号（"resume 一个进程,恢复完整 struct trapframe 后 sret",
     * proc.h/swtch.S 顶部注释已经展开)——这里的标签只是"跳过下面两行
     * sp-恢复逻辑,直接执行 sret"这个局部小动作,没有 .global 过,不会
     * 在链接时和 swtch.S 的 trap_return 冲突,但两者同名容易在阅读时
     * 混淆成同一件事,改名 do_sret 避免这种误解。 */
    bnez t1, do_sret

    la t0, trap_saved_user_sp
    ld sp, 0(t0)

do_sret:
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
