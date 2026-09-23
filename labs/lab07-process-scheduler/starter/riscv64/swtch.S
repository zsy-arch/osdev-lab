/* Lab7 riscv64：swtch(old, new) —— 内核执行流之间的上下文切换。跟
 * x86_64 版本（../x86_64/swtch.S）同一个 xv6 经典设计，这里只写 riscv64
 * 寄存器集合上的具体差异，核心原理（"切换执行流只需要保存 callee-saved
 * 寄存器 + 一个返回地址，caller-saved 寄存器由调用约定本身保证不需要
 * swtch() 越权处理"）跟那份文件顶部模块注释完全一致，不重复展开。
 *
 * 签名：void swtch(struct context **old, struct context *new)
 *   a0 = old（二级指针）
 *   a1 = new
 * 跟 x86_64 版本用 rdi/rsi 传这两个参数是同一个"调用约定的头两个整数
 * 参数寄存器"角色，只是 riscv64 用 a0/a1。
 *
 * callee-saved 集合：riscv-abi 规范"Register Convention"表格里 ra 和
 * s0-s11 都标"Saved register"（跟 x86_64 的 rbx/rbp/r12-r15 五个比,
 * riscv64 这里是 13 个——ra 本身也算在内,因为 riscv64 没有 x86_64 call
 * 指令那种"硬件自动把返回地址压栈"的行为,call/jalr 是把返回地址放进
 * ra 寄存器,不是内存,swtch() 要把 ra 也一起 sd/ld，跟 x86_64 版本注释
 * 里"call 已经把返回地址压到栈上了，不需要手动保存"正好是相反的具体
 * 做法——但达到的效果一样：都是把"下次切回这里应该跳到哪"这份信息
 * 保存进 struct context）。
 */
.section .text

/* TODO 1：实现 swtch(old, new)。
 *
 * 为什么：riscv-abi 规范"Register Convention"表格里 ra 和 s0-s11 都标
 * "Saved register"，一共 13 个（比 x86_64 的 6 个多，且包含 ra 本身——
 * riscv64 没有 x86_64 call 指令那种"硬件自动把返回地址压栈"的行为，
 * call/jalr 是把返回地址放进 ra 寄存器，不是内存，所以 swtch() 必须
 * 像对待 s0-s11 一样显式 sd/ld ra）。先在栈上开 104 字节（13×8）保存
 * 这 13 个寄存器，把当前 sp 存进 *old，换栈到 new，再从新栈按同样顺序
 * 恢复这 13 个寄存器，最后 ret 跳到刚恢复出来的 ra。
 *
 * 提示：
 *   addi sp, sp, -104
 *   sd ra,   0(sp)
 *   sd s0,   8(sp)
 *   sd s1,  16(sp)
 *   sd s2,  24(sp)
 *   sd s3,  32(sp)
 *   sd s4,  40(sp)
 *   sd s5,  48(sp)
 *   sd s6,  56(sp)
 *   sd s7,  64(sp)
 *   sd s8,  72(sp)
 *   sd s9,  80(sp)
 *   sd s10, 88(sp)
 *   sd s11, 96(sp)
 *
 *   sd sp, (a0)
 *
 *   mv sp, a1
 *
 *   ld ra,   0(sp)
 *   ld s0,   8(sp)
 *   ld s1,  16(sp)
 *   ld s2,  24(sp)
 *   ld s3,  32(sp)
 *   ld s4,  40(sp)
 *   ld s5,  48(sp)
 *   ld s6,  56(sp)
 *   ld s7,  64(sp)
 *   ld s8,  72(sp)
 *   ld s9,  80(sp)
 *   ld s10, 88(sp)
 *   ld s11, 96(sp)
 *   addi sp, sp, 104
 *
 *   ret
 */
.global swtch
swtch:

/* trap_return：proc_alloc_skeleton() 把每个新进程 context.ra 都设成这个
 * 地址，swtch() 第一次切进一个新进程时，上面的 `ld ra, 0(sp)` 读到的
 * 正是这个地址，`ret` 跳过来——"启动一个从未运行过的新进程"和"resume
 * 一个 trap 处理完毕、正常走到这里的旧进程"落进同一段代码,跟 x86_64
 * 版本 trap_return 同一个 xv6 经典手法,proc.h 顶部模块注释已经展开过
 * 这个设计,这里不重复。
 *
 * 进来的时候 sp 已经是"这个进程自己的内核栈"（swtch() 换栈换的就是
 * 这个），且这个栈上紧跟在 context 13 个字段被 ld 完之后的位置，正是
 * proc_alloc_skeleton() 摆好的 struct trapframe——两者紧邻，trap_return
 * 不需要任何寄存器传参就能找到自己的 trapframe，直接理解成"栈顶再往上
 * 一点就是 trapframe 的起始地址"，跟 x86_64 版本同一个布局关系。
 *
 * 下面这段 ld 序列必须跟 struct trapframe（proc.h）的字段顺序逐一对应
 * ——跟 trap_entry.S 里 supervisor_trap_entry 的 sd 顺序也必须一致，
 * 三者描述的是同一种栈帧形状。不能复用 supervisor_trap_entry 内部那段
 * ld（无法复用：那段紧跟着 sret，中间没有导出成一个独立符号，且本 Lab
 * 刻意不改 trap_entry.S 里 S-mode 来源那条路径的既有形状，避免影响
 * Lab4/5/6 已经验证过的 page fault/定时器路径）——这跟 x86_64 版本
 * trap_return 无法复用 timer_stub/page_fault_stub 内部弹栈段是同一个
 * 理由。
 *
 * 最后两个字段（sepc/sstatus）不能用 ld 直接进通用寄存器再指望它们
 * "自动生效"——riscv64 没有 iretq 那种"一条指令把栈上一串字段当成
 * PC/flags/segment 自动加载"的硬件行为（trap_entry.S 顶部模块注释、
 * enter_user_mode 那段注释已经反复说明这一点：sret 不管通用寄存器,也
 * 不会自己去读栈,它只读当前 CSR 的值),必须先 ld 进一个临时通用寄存器，
 * 再 csrw 显式写进 sepc/sstatus 这两个 CSR，最后 sret 才会真正用上
 * 它们——这是 riscv64 相对 x86_64 iretq 在"恢复现场"这一步必须多做的
 * 两步，对应 x86_64 版本注释里"RIP/CS/RFLAGS/RSP/SS 留在栈上,形状正好
 * 是 iretq 期望的样子"这一整句在 riscv64 上不成立,riscv64 没有等价的
 * "栈帧形状换来硬件自动处理"这条捷径。 */
/* TODO 2：实现 trap_return。
 *
 * 为什么：sp 换栈到这里之后，紧跟在 context 13 个字段之上的位置正是
 * struct trapframe（proc.h）——要注意恢复顺序刻意跟 trapframe 的字段
 * 声明顺序不一致：CSR（sepc/sstatus，位于 trapframe 最后两个字段）
 * 必须最先恢复，因为写它们需要借用 t0/t1 当搬运暂存（riscv64 没有
 * x86_64 iretq 那种"一条指令把栈上一串字段自动加载成 PC/flags"的硬件
 * 行为，csrw 只能把值从通用寄存器搬进 CSR，必须先 ld 进 t0/t1 再
 * csrw，写完之后 t0/t1 才能被后面正常当普通寄存器 ld 覆盖）；sp/gp/tp
 * 三个"一执行就会改变当前取址/取栈基准"的寄存器同理必须放最后，一旦
 * ld sp 之后当前栈上剩余待恢复的字段就再也找不到了。
 *
 * 提示：
 *   ld t0, 248(sp)
 *   ld t1, 256(sp)
 *   csrw sepc, t0
 *   csrw sstatus, t1
 *
 *   ld ra,  0(sp)
 *   ld t0,  32(sp)
 *   ld t1,  40(sp)
 *   ld t2,  48(sp)
 *   ld t3,  56(sp)
 *   ld t4,  64(sp)
 *   ld t5,  72(sp)
 *   ld t6,  80(sp)
 *   ld s0,  88(sp)
 *   ld s1,  96(sp)
 *   ld s2, 104(sp)
 *   ld s3, 112(sp)
 *   ld s4, 120(sp)
 *   ld s5, 128(sp)
 *   ld s6, 136(sp)
 *   ld s7, 144(sp)
 *   ld s8, 152(sp)
 *   ld s9, 160(sp)
 *   ld s10,168(sp)
 *   ld s11,176(sp)
 *   ld a0, 184(sp)
 *   ld a1, 192(sp)
 *   ld a2, 200(sp)
 *   ld a3, 208(sp)
 *   ld a4, 216(sp)
 *   ld a5, 224(sp)
 *   ld a6, 232(sp)
 *   ld a7, 240(sp)
 *
 *   ld gp,   16(sp)
 *   ld tp,   24(sp)
 *   ld sp,    8(sp)
 *
 *   sret
 */
.global trap_return
trap_return:
