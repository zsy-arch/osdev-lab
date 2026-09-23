# Lab7：进程与调度

## 学习目标

- 理解"进程"在本课程语境下的最小定义：一份独立的页表 + 一份独立的内核栈 + 一份能在任意时刻被冻结/恢复的执行上下文（`struct context`）+ 一份能在任意时刻被冻结/恢复的用户态现场（`struct trapframe`）。Lab6 只有一个用户程序、一份页表、内核态忙等；本 Lab 第一次让"CPU 现在在执行哪个执行流"变成一个会随时间变化、由内核自己决定的问题。
- 亲手实现 `swtch()`——两条内核执行流之间最小的上下文切换原语，只保存/恢复调用约定要求 callee-saved 的那一部分寄存器，不经过任何特权级变化，理解它和"从用户态陷入内核态"（`trap`）是两个正交的机制：一个负责"内核执行流 A 让位给内核执行流 B"，另一个负责"用户态让位给内核态"，`trap_return` 是本 Lab 第一次把两者接在一起——resume 一个进程时，先 `swtch()` 回到它自己的内核栈，再从那份栈上的 `struct trapframe` 里把用户态现场恢复回去。
- 通过实现 `fork`/`exec`/`wait`/`exit` 四个系统调用，理解真实操作系统"创建新执行流"和"replace 当前执行流的程序内容"是两个可以正交组合的原语（fork 只复制、不换程序；exec 只换程序、不创建新执行流），本 Lab 把两者都简化到最小可教学的程度（fork 是立即整页复制，不是 copy-on-write；exec 只能换成内核自带的同一份 `user_prog.bin`，不支持任意程序），但组合方式和真实 Unix 完全一致。
- 再一次通过本 Lab 开发过程中踩到的真实 bug（见"常见坑与排查"），巩固"这次改动只影响一个函数"的直觉往往是错的——本 Lab 七个真实 bug里，两个跨越了"trap 之内没问题、trap 之外才暴露"的时间窗口（riscv64 的 `sepc` 两次因为不同触发路径被跨进程覆写），一个是硬件描述符表的一个字段少填一个字节、只在教学场景第一次真正让 CPU 长时间停留在用户态时才会暴露（x86_64 TSS），一个是汇编里少了一条数据搬运指令导致两个函数参数悄悄指向同一个地址（x86_64 `syscall_entry`），一个是编译器直接抓出来的类型错误（`g_scheduler_context` 该是指针不是值），两个分别在两个架构里各自独立发生、但根因高度相似（"某个全局的、per-hart/per-core 的 CPU 状态，只是被写了一次，没有考虑跨越一次执行流切换之后还要读一次的时间窗口"）。

## 前置 Lab

依赖 [Lab6：系统调用与用户态](../lab06-syscall-user/README.md)——本 Lab 复用 Lab6 建好的用户态基础设施（页表、GDT/`stvec` trap 入口、`sys_write`/`sys_exit` 的雏形、`enter_user_mode`）,在此之上引入多进程和调度,不重新讲解特权级切换本身的机制。

`boot.S`、`console_putc.c`、`panic_arch.c`、`memmap.c`、`pagetable.c` 里 Lab6 已有的部分、x86_64 的 `grub.cfg`/`linker.ld`、riscv64 的 `sbi.c`/`sbi.h`/`linker.ld`、两边的 `syscall.h` 里 `SYS_WRITE`/`SYS_EXIT` 两个既有常量、`user_prog.ld`、`user_blob.S` 这些文件和 Lab6 完全一样或只是在原有基础上追加,本 README 不重复讲解底层机制,只讲本 Lab 新增的部分。

## 核心概念

**`struct context`（内核执行流切换）和 `struct trapframe`（用户态现场）保存的是两类不同的状态,不能混用。** `struct context` 只包含调用约定规定 callee-saved 的那一部分寄存器（x86_64：`rbx`/`rbp`/`r12-r15`/`rip`,7 个;riscv64：`ra`/`s0-s11`,13 个)——`swtch()` 本质上是一次"手工写的函数调用",调用者（比如 `scheduler()`)在 C 语言层面完全不知道自己会被换出去、换回来之间隔了多久、中间执行过什么,只需要相信"caller-saved 的寄存器反正会被下一次函数调用自然覆盖,不用管;callee-saved 的寄存器必须原样保留",这正是编译器生成的函数调用序列本来就会遵守的约定,`swtch()` 只是把这个约定运用在了"跳到另一条完全独立的执行流"这个不寻常的场景。`struct trapframe` 则是完全不同的另一件事——它保存的是"用户态那一刻,所有寄存器（不只是 callee-saved,是*全部*)是什么样子",因为用户态代码不遵守内核的调用约定,内核不能假设任何一个用户寄存器"反正会被覆盖不用管"。两者的生命周期也不同：`struct context` 只在"内核执行流之间切换"这一个瞬间被读写；`struct trapframe` 从"这次 trap 发生"一直存活到"这次 trap 通过 `trap_return`/`sret`/`sysretq` 结束",期间可能横跨任意多次 `swtch()`。

**`trap_return` 统一了"第一次启动一个进程"和"恢复一个之前被打断的进程",这是本 Lab 在调度器设计上最核心的一步简化。** 没有它的话,"进程从未运行过,第一次要跳进用户态"和"进程之前运行到一半被定时器打断,现在轮到它了,要恢复"是两套完全不同的代码路径——但事实上,两者需要做的事完全一样：从这个进程自己的 `struct trapframe` 里恢复所有寄存器（第一次是内核手工摆好的初始值；第 N 次是上一次 trap 保存下来的真实值),然后返回用户态。本 Lab 让 `proc_alloc_skeleton()` 把每个新进程的 `context.rip`/`context.ra` 直接设成 `trap_return` 的地址——调度器第一次 `swtch()` 进这个从未运行过的进程时,`swtch()` 内部的 `ret`/等价跳转指令会直接落进 `trap_return`,后续该进程每一次被重新调度回来,也是同样落进 `trap_return`,唯一的区别只是这次 `struct trapframe` 里的内容是"内核手工摆的初始值"还是"上一次 trap 真实保存的值"——这个区别对 `trap_return` 自己完全透明,它不需要知道也不需要区分。

**riscv64 的 `sret` 天然支持"第一次和第 N 次用同一条指令",x86_64 的 `sysretq` 不支持,这导致两边 `enter_user_mode` 的命运在本 Lab 分道扬镳。** `sret` 读的 `sepc`/`sstatus.SPP`/`sstatus.SPIE` 全部是软件可写的普通 CSR,不依赖"之前必须发生过一次真正的陷入"这个硬件前提——`trap_return` 引入之后,riscv64 版本的 `enter_user_mode` 变得完全多余（它手工做的三件事——写 `sepc`、写 `sstatus`、换 `sp`——跟 `trap_return` 从 `struct trapframe` 里恢复 `sepc`/`sstatus`/`sp` 是同一件事,只是数据来源不同),本 Lab 直接删除了这个函数。x86_64 的 `sysretq` 则不同：它恢复 RIP/RSP 依赖 RCX/R11 这两个寄存器,而这两个寄存器*只有在真正执行过一次 SYSCALL 之后*才会被硬件自动写入正确的值——`trap_return` 引入之后仍然只能覆盖"这个进程之前已经陷入过一次"的场景,"从未运行过、第一次启动"这个场景依然只能靠手工构造 `iretq` 帧完成,`enter_user_mode` 因此在 x86_64 侧被保留下来,继续承担"唯一一次真正的第一次启动"的角色（`proc_alloc()` 给初始进程 `struct trapframe` 填的字段,格式跟手工构造的 `iretq` 帧其实是同一套数据,只是从"立即执行"变成了"存起来,等 `trap_return` 以后来读"——这也是为什么 x86_64 的 `enter_user_mode` 保留至今、但已经不再是 Lab6 时期那种"直接执行 `iretq` 跳过去"的独立调用点,本 Lab 里它事实上从未被真正调用,proc_alloc() 给 `struct trapframe` 填的初始值本身承担了原来 `enter_user_mode` 参数的角色）。

**调度器需要一个"per-进程的 CPU 硬件状态",这在两个架构上分别对应完全不同的硬件机制,但要解决的问题是同一个。** x86_64 侧,IDT 中断门在 CPL3→CPL0 提权时,硬件从 TSS（Task State Segment）的 `RSP0` 字段读取"该用哪个内核栈",本 Lab 第一次真正装了一个 TSS（Lab1-6 全程 `TR` 都是无效值,因为用户态代码从来不会在执行期间被定时器打断——Lab6 的用户程序执行完两次 SYSCALL 就落进死循环,`sysretq` 走的是完全独立于 IDT 的路径,不需要 TSS);SYSCALL/SYSRET 完全绕开 IDT,不查 TSS,本 Lab 为它单独引入了一个平行的全局变量 `syscall_kernel_rsp`,由调度器在每次 `swtch()` 之前一并更新。riscv64 侧,`ecall` 和 page fault/定时器共享同一个 `stvec` 入口（Lab5/6 已经确立的结构),入口切栈的目标地址原来是链接期常量 `__stack_top`（Lab6 时期只有一条执行流,永远复用同一个栈是安全的),本 Lab 把它换成一个运行期可写的全局变量 `trap_kernel_sp_top`,同样由调度器负责在每次 `swtch()` 之前指向即将运行的那个进程自己的内核栈顶。两边的分工完全一致："调度器负责写,trap 入口负责读",只是 x86_64 用一个专门的硬件结构（TSS）+ 一个自定义全局变量各管一条陷入路径,riscv64 因为只有一条共享入口,只需要一个自定义全局变量。

**fork 不是 copy-on-write,exec 不能换成任意程序——这是本 Lab 刻意选择的教学简化,不是真实内核的完整实现。** 真实的 `fork()` 用页表标记只读+写时复制来避免立即复制整个地址空间；本 Lab 的 `fork()` 在系统调用返回之前就把父进程每一个已映射页的*内容*立即复制进新分配的物理页——逻辑上更直接,不需要额外处理"写时触发缺页、内核该怎么响应"这一整套机制,代价是复制成本和地址空间大小成正比,不适合真实场景。真实的 `exec()` 接受任意路径,从文件系统读取任意 ELF 并替换当前地址空间；本 Lab 没有文件系统,`exec()` 能做的唯一一件事是把调用者自己的地址空间重新映射成内嵌的同一份 `user_prog.bin`（`user_blob.S` 里 `.incbin` 进来的那份字节,和 `proc_alloc()` 创建任何新进程时用的是同一份数据),`rip`/`sp` 重置到程序入口和栈顶——这足以演示"exec 会替换当前执行流的程序内容,而不是创建新执行流"这个核心语义,但不是一个通用的程序加载器。

## x86_64 与 riscv64 对照表

| 维度 | x86_64 | riscv64 |
|---|---|---|
| `struct context` 保存的寄存器 | `rbx`/`rbp`/`r12-r15`/`rip`，7 个，`rip` 放最后 | `ra`/`s0-s11`，13 个，`ra` 放最前——两边顺序都是各自约定，不影响正确性 |
| `struct trapframe` 相对 Lab6 的变化 | 字段不变（仍是 Lab5/6 就有的 15 GPR + iretq 5 字段），Lab7 只是第一次给这些字段起了名字（`struct trapframe` 类型），供 C 代码按名字访问（`fork()` 需要读/改子进程的 `rax`） | 从 Lab4/5/6 的 16 字段轻量帧（`ra`/`t0-t6`/`a0-a7`）真正扩展到 34 字段，新增 `sp`/`gp`/`tp`/`s0-s11`——调度器可能让一个进程在 trap 内部挂起任意长时间，s0-s11 这类 callee-saved 寄存器如果不保存，会被其间运行的其它进程的代码覆盖 |
| "resume 一个进程"的入口 | `trap_return`（新），第一次启动仍需 `enter_user_mode` 手工构造 `iretq` 帧（`sysretq` 依赖 RCX/R11 必须先发生过一次真 SYSCALL） | `trap_return`（新），第一次和第 N 次共用同一条路径，`enter_user_mode` 已删除（`sret` 不依赖任何"之前发生过陷入"的硬件前提） |
| per-进程内核栈顶如何让硬件/trap 入口知道 | TSS.RSP0（新引入 TSS，IDT 门提权时硬件自动读取）+ `syscall_kernel_rsp` 全局变量（SYSCALL 路径专用，绕开 TSS） | `trap_kernel_sp_top` 全局变量（取代 Lab6 的 `__stack_top` 直接量），`supervisor_trap_entry` 里 `la`/`ld` 直接读 |
| 调度器切换前必须同步更新的状态 | `tss_set_rsp0()` + `syscall_set_kernel_rsp()`，两处都要 | `trap_kernel_sp_top = 目标进程内核栈顶` 一处 |
| fork/exec 系统调用签名 | `sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot)` / `sys_exec(同上)`——SYSCALL 现场的用户 RIP/RSP 只以局部形式存在于 `syscall_entry` 栈帧/全局变量里，需要传地址下去才能改写 | `sys_fork(void)` / `sys_exec(void)`——等价的两个值（`sepc` CSR、`trap_saved_user_sp` 全局变量）本身就是全局可寻址的，不需要额外传指针 |
| 跨 `yield()`/`swtch()` 需要额外手动保护的 CPU 全局状态 | 无额外的（RIP/RSP 已经通过 `struct trapframe` 走 `trap_return` 路径自然保护） | `sepc`（per-hart CSR，不是 per-process 存储）——`sys_exec()` 和 `timer_interrupt_handler()` 分别独立触发过这一类真实 bug，是本 Lab riscv64 侧最核心的坑 |
| 本 Lab 新增的硬件/软件结构 | TSS（`struct tss64`，8 项 GDT，`tss_init()`） | 无新硬件结构，`ecall` 分支逻辑本身在 Lab6 基础上扩展 |

## 代码目录与关键文件

```
labs/lab07-process-scheduler/
├── Makefile                     # 在 Lab6 基础上新增 proc.c/swtch.S 两个源文件
├── README.md                    # 本文件
├── solution/
│   ├── x86_64/
│   │   ├── boot.S                # 和 Lab6 完全一样
│   │   ├── linker.ld             # 和 Lab6 完全一样
│   │   ├── grub.cfg              # 和 Lab6 完全一样
│   │   ├── console_putc.c        # 和 Lab6 完全一样
│   │   ├── panic_arch.c          # 和 Lab6 完全一样
│   │   ├── memmap.c              # 和 Lab6 完全一样
│   │   ├── pagetable.c           # 在 Lab6 基础上新增：pagetable_unmap/pagetable_copy_kernel_range/pagetable_set_kernel_root
│   │   ├── pit.c/pit.h           # 在 Lab6 基础上新增：timer_interrupt_handler（EOI 之后调用 yield()）
│   │   ├── proc.h                # 新增：struct context/struct trapframe/struct proc，进程表大小，fork/exec/wait/exit 原型
│   │   ├── proc.c                # 新增：进程表、proc_alloc、调度器主循环、fork/exec/wait/exit 的具体实现
│   │   ├── swtch.S               # 新增：swtch()（callee-saved 寄存器切换）+ trap_return（统一 resume 入口）
│   │   ├── syscall.h             # 在 Lab6 基础上新增：SYS_FORK/SYS_EXEC/SYS_WAIT
│   │   ├── trap.c                # 在 Lab6 基础上新增：TSS（struct tss64/tss_init/tss_set_rsp0）+ syscall_kernel_rsp + syscall_dispatch 新增三个分支
│   │   ├── trap_entry.S          # 在 Lab6 基础上改 syscall_entry：换栈目标从 __stack_top 变成 syscall_kernel_rsp（每进程一个内核栈），并多传两个参数给 syscall_dispatch（用户 RIP/RSP 的保存位置，供 sys_exec 改写）；page_fault_stub/timer_stub/enter_user_mode 逐字节保留，enter_user_mode 角色不变，仍是首次启动的手工 iretq 构造
│   │   ├── kernel_main.c         # 在 Lab6 基础上新增：tss_init() 调用 + proc_init()/proc_alloc()/scheduler() 取代原来的手工 enter_user_mode 调用
│   │   ├── user_prog.S           # 在 Lab6 基础上新增：fork/exec/wait 三次系统调用 + "reborn" 判断分支
│   │   ├── user_prog.ld          # 和 Lab6 完全一样
│   │   └── user_blob.S           # 和 Lab6 完全一样
│   └── riscv64/
│       ├── boot.S                # 和 Lab6 完全一样
│       ├── linker.ld             # 和 Lab6 完全一样
│       ├── sbi.c/sbi.h           # 和 Lab6 完全一样
│       ├── console_putc.c        # 和 Lab6 完全一样
│       ├── panic_arch.c          # 和 Lab6 完全一样
│       ├── memmap.c              # 和 Lab6 完全一样
│       ├── pagetable.c           # 在 Lab6 基础上新增：pagetable_unmap/pagetable_copy_kernel_range/pagetable_set_kernel_root
│       ├── proc.h                # 新增：struct context/struct trapframe（34 字段，真正扩展）/struct proc，fork/exec/wait/exit 原型
│       ├── proc.c                # 新增：进程表、proc_alloc、调度器主循环、fork/exec/wait/exit 的具体实现，write_sepc() 跨 sys_exec 保护
│       ├── swtch.S               # 新增：swtch()（ra/s0-s11 切换）+ trap_return
│       ├── syscall.h             # 在 Lab6 基础上新增：SYS_FORK/SYS_EXEC/SYS_WAIT
│       ├── trap.c                # 在 Lab6 基础上新增：trap_kernel_sp_top + timer_interrupt_handler 里的 sepc 保护 + syscall_dispatch 新增三个分支
│       ├── trap_entry.S          # 在 Lab6 基础上新增：from_user 分支换栈目标从 __stack_top 变成 trap_kernel_sp_top；删除 enter_user_mode
│       ├── kernel_main.c         # 在 Lab6 基础上新增：proc_init()/proc_alloc()/scheduler() 取代原来的手工 enter_user_mode 调用
│       ├── user_prog.S           # 在 Lab6 基础上新增：fork/exec/wait 三次系统调用 + "reborn" 判断分支
│       └── user_prog.ld          # 和 Lab6 完全一样
├── starter/                      # 结构和 solution 一一对应，教学文件带 TODO
│   ├── x86_64/
│   └── riscv64/
└── tests/
    ├── expect-x86_64.txt
    └── expect-riscv64.txt
```

`proc.h` 里 `struct context`/`struct trapframe` 具体字段顺序两边各自遵循自己已有的惯例（x86_64 沿用 Lab5/6 `timer_stub`/`page_fault_stub` 的压栈顺序；riscv64 沿用 `ra` 放最前的习惯），字段顺序本身不影响正确性，只要 `swtch.S`/`trap_entry.S` 里的偏移量跟 `proc.h` 的字段声明手工保持一致——这跟 Lab6 `ecall_handler` 里 `frame[N]` 下标必须跟 `trap_entry.S` 保存顺序同步是同一类"没有单一数据源、需要人肉对齐"的简化，本 Lab 又新增了两处（`swtch.S` 的偏移量、`struct trapframe` 的偏移量）。

## 分步实现步骤

### x86_64 路线

1. **proc.h：定义 `struct context`（`rbx`/`rbp`/`r12-r15`/`rip`，7 个 `uint64_t`）、`struct trapframe`（对齐 Lab5/6 `timer_stub` 的压栈顺序，15 个 GPR 字段 + `rip`/`cs`/`rflags`/`rsp`/`ss` 共 20 个）、`struct proc`（`state`/`pid`/`parent_pid`/`exit_code`/`pagetable`/`kstack_phys`/`tf` 指针/`context` 指针/`name[16]`）。** `sys_fork`/`sys_exec` 的签名要带两个 `uintptr_t *` 参数（`user_rip_slot`/`user_rsp_slot`）——SYSCALL 现场的用户 RIP/RSP 只以局部形式存在（RCX 寄存器、`syscall_saved_user_rsp` 全局变量里当前那次调用的值），没有第二个全局可寻址的位置能找到"这次系统调用返回后该恢复到哪里"，必须由 `trap_entry.S` 显式把这两个地址传下来。
2. **swtch.S：`swtch(struct context **old, struct context *new)`。** 只保存/恢复 callee-saved 寄存器（`rbx`/`rbp`/`r12-r15`）+ 返回地址（`rip`，靠 `call`/`ret` 本身的机制，不需要额外 push/pop）——调用约定已经保证 caller-saved 的寄存器不需要跨越这次调用保留，`swtch()` 不必比一次普通函数调用做更多事。第一个参数是二级指针（`struct context **`），因为 `swtch()` 需要把"调用者这一刻的上下文"写回调用者自己的 `struct proc` 里，供将来某次 `swtch()` 换回来时读取。
3. **swtch.S：`trap_return`。** 从当前栈顶（此刻栈顶就是 `swtch()` `ret` 落地之后、指向的那个进程自己的内核栈——这是 `proc_alloc_skeleton()` 提前摆好的布局）取出 `struct trapframe` 指针，`iretq` 弹出全部 20 个字段回到用户态——跟 `trap_entry.S` 里 `timer_stub`/`page_fault_stub` 结尾的 `iretq` 是完全相同的一条指令，只是这次数据来源是"某个进程自己存了不知道多久的 trapframe"，不是"刚发生的这一次陷入"。
4. **trap.c：GDT 扩展到 8 项 + `struct tss64` + `tss_init()`/`tss_set_rsp0()`。** TSS 描述符是 64 位系统描述符，占用 2 个 GDT 槽位（不是 1 个）——GDT 数组从 Lab6 的 6 项扩到 8 项。`tss_init()` 手工按 Intel SDM 描述的格式构造这个 16 字节描述符，**`base` 字段的最高字节（bit 31:24）必须单独打包进描述符高 8 字节的 bit 56:63**，这是最容易漏掉的一步（一旦漏掉，`base` 的 bit31 被截断为 0，只有当 TSS 结构体恰好链接在一个 bit31=1 的地址时才会真正出错——本 Lab 就是这种情况，见"常见坑与排查"）。
5. **trap.c：`syscall_kernel_rsp` 全局变量。** SYSCALL 完全绕开 IDT/TSS，`syscall_entry` 换栈时读的是这个变量，不是 TSS.RSP0——**调度器必须在每次 `swtch()` 之前，把这个变量和 TSS.RSP0 一起更新成即将运行的进程自己的内核栈顶**，漏掉任何一个都会导致该陷入路径继续复用旧的栈顶（见"常见坑与排查"关于共享栈覆盖的完整故障链路）。
6. **proc.c：`proc_init`/`proc_alloc_skeleton`/`proc_alloc`。** `proc_alloc_skeleton()` 找一个 `UNUSED` 的进程表槽位，分配内核栈、独立页表，把 `context->rip` 设成 `trap_return` 的地址——这一步是本 Lab"第一次启动"和"resume"统一的关键，`proc_alloc()` 在此基础上补齐 `struct trapframe` 里 `cs`/`ss`/`rflags` 等只有"第一次启动"才需要显式指定的字段（`rip`/`rsp` 稍后由 `map_user_prog()` 填）。
7. **proc.c：`scheduler()`。** 一个永不返回的循环，遍历进程表找 `RUNNABLE` 的进程，`tss_set_rsp0()`/`syscall_set_kernel_rsp()` 都指向它的内核栈顶，`pagetable_activate()` 切换到它的页表，`swtch(&g_scheduler_context, p->context)` 跳过去。**`g_scheduler_context` 必须声明成 `struct context *`（指针），不是 `struct context`（值）**——这是本 Lab 编译期就会被 GCC 抓到的真实类型错误（`-Wincompatible-pointer-types`），因为 `swtch()` 的第一个参数要求二级指针，传值类型的地址下去类型不匹配。
8. **proc.c：`yield()`/`sys_exit_proc()`。** `yield()` 把当前进程标记 `RUNNABLE`，`swtch()` 回 `scheduler()`（此时 `g_scheduler_context` 里存的正是 `scheduler()` for 循环内部这次调用点的上下文，`swtch()` 换回来的效果就是"回到 `scheduler()` 那次 `swtch()` 调用刚返回之后的位置，继续往下找下一个进程"）。`sys_exit_proc()` 标记当前进程 `ZOMBIE`，保存退出码，`swtch()` 回调度器（永不返回）。
9. **proc.c：`fork_copy_page()`/`sys_fork()`。** 遍历父进程页表已映射的每一页，给子进程分配一个新物理页，`memcpy` 内容过去，映射进子进程页表——这是本 Lab"fork 是立即整页复制，不是 copy-on-write"这个简化的具体实现。`sys_fork()` 用 `proc_alloc_skeleton()` 给子进程建好骨架后，把父进程当前的 `rip`/`rsp`（通过 `user_rip_slot`/`user_rsp_slot` 拿到）复制进子进程的 `struct trapframe`，**子进程的 `rax`（fork 返回值寄存器）必须是 0，父进程的 `rax` 是子进程 pid**——`proc_alloc_skeleton()` 内部对新分配的 `struct trapframe` 先 `memset` 清零，天然满足子进程 `rax=0` 这个要求，不需要额外显式赋值。
10. **proc.c：`sys_exec()`。** 对当前进程调用 `pagetable_unmap()` 撤销旧的用户程序/用户栈映射（Lab7 新增函数，见 `pagetable.c`——漏掉这一步、直接对同一个虚拟地址重新 `pagetable_map()` 会触发"重复映射"的 panic，见"常见坑与排查"），重新调用 `map_user_prog()` 建立全新的映射，把 `user_rip_slot`/`user_rsp_slot` 指向的位置改写成新的入口地址/栈顶——**这两个写入操作必须真的各自落到不同的地址上**，本 Lab 一处真实 bug（见"常见坑与排查"）就是这两个指针在 `trap_entry.S` 里因为少了一条寄存器搬运指令而互相别名，导致其中一次写入被另一次覆盖。
11. **proc.c：`sys_wait()`。** 遍历进程表找一个 `parent_pid` 等于当前进程 pid 且状态是 `ZOMBIE` 的子进程，回收它（标记 `UNUSED`，释放页表/内核栈），返回它的 pid，出参回传退出码；如果没有任何这样的子进程，返回 -1（本 Lab 不实现"阻塞等待子进程退出"，只做一次性查询，找不到就立即返回，不重试/不睡眠）。
12. **kernel_main.c：** 在 Lab6 已有的初始化序列之后，`tss_init()`（必须在 GDT 建好之后、`proc_alloc()` 创建第一个进程之前），`proc_init()` + `proc_alloc()` 创建初始进程，`scheduler()`（永不返回，取代 Lab6 直接调用 `enter_user_mode` 的位置）。
13. **user_prog.S：** 在 Lab6"打印一行、退出"的基础上，加入 fork/exec/wait 的调用序列，并在 `_user_start` **最开头**加入"reborn 判断分支"（用一个约定寄存器/内存标记区分"这是第一次执行到这里"还是"这是 exec 之后重新加载、从头跑起来的执行"，避免子进程 exec 之后无限重复 fork），这个判断必须放在整段代码的最前面，不能安插在中间——见"常见坑与排查"。

### riscv64 路线

1. **proc.h：定义 `struct context`（`ra`/`s0-s11`，13 个）、`struct trapframe`（从 Lab4/5/6 的 16 字段轻量帧真正扩展到 34 字段：新增 `sp`/`gp`/`tp`/`s0-s11`）、`struct proc`（跟 x86_64 版本字段布局一致）。** `sys_fork`/`sys_exec` 是 `void` 参数——跟 x86_64 版本需要两个指针参数不同，riscv64 这边等价的两个值（"返回后该恢复到哪个 PC"就是 `sepc` 这个 CSR 本身，"用哪个用户栈"就是 `trap_saved_user_sp` 这个全局变量）本身就是全局可寻址的，不需要额外传指针下去。
2. **swtch.S：`swtch(struct context **old, struct context *new)`。** 保存/恢复 `ra`/`s0-s11`（13 个，104 字节）——跟 x86_64 版本保存的寄存器集合不同，但角色完全一致：只覆盖调用约定要求 callee-saved 的部分。
3. **swtch.S：`trap_return`。** 从栈顶取出 `struct trapframe` 指针，依次恢复 32 个 GPR + 写 `sepc`/`sstatus`，`sret` 回用户态——跟已删除的 `enter_user_mode` 做的事情本质相同（写 `sepc`、写 `sstatus` 的 SPP/SPIE/SUM 位、换 `sp`），只是数据来源从"函数参数寄存器"变成了"内存里某个进程自己的 `struct trapframe`"。
4. **proc.c：`proc_init`/`proc_alloc_skeleton`/`proc_alloc`。** 跟 x86_64 版本逻辑一致，`context->ra` 设成 `trap_return` 的地址；`struct trapframe` 里 `sstatus` 需要显式设好 SPP=0（返回 U-mode）/SPIE=1（返回后允许被中断打断）/SUM=1（S-mode 能访问带 U 位的页，Lab6 已经踩过这个坑，本 Lab 沿用同一个初始值,不需要重新踩）。
5. **trap.c：`trap_kernel_sp_top` 全局变量。** 取代 Lab6 `trap_entry.S` 里 `la sp, __stack_top` 的直接量——每个进程现在有自己独立的内核栈，`scheduler()` 在每次 `swtch()` 之前把这个变量指向即将运行的进程自己的内核栈顶，`trap_entry.S` 只负责读，不关心它当前具体指向谁。
6. **proc.c：`scheduler()`。** 跟 x86_64 版本逻辑一致（找 `RUNNABLE` 进程、更新 `trap_kernel_sp_top`、`pagetable_activate()`、`swtch()`），riscv64 侧没有 TSS 这类硬件结构需要同步更新，只需要维护这一个全局变量。
7. **proc.c：`yield()`/`sys_exit_proc()`。** 跟 x86_64 版本逻辑一致。
8. **trap.c：`timer_interrupt_handler()` 里保护 `sepc`。** 每次定时器 tick，如果确实有进程在跑，调用 `yield()` 之前把 `read_sepc()` 存进 `p->tf->sepc`，`yield()` 返回之后再 `write_sepc(p->tf->sepc)` 写回去——**`sepc` 是 per-hart 的 CSR，不是 per-process 的存储**，`yield()` 内部的 `swtch()` 可能让其它进程运行任意长时间，其间它们自己的每一次 trap 都会覆写同一个 `sepc`，如果不在这里手动保护，这个进程被重新调度回来时读到的会是别的进程留下的值（本 Lab 一处真实 bug，见"常见坑与排查"，跟下面 `sys_exec()` 的坑是同一个根因的第二次独立发作）。
9. **proc.c：`write_sepc()` 保护 `sys_exec()`。** `sys_exec()` 改写 `rip`（即 `sepc`）不能只写 `tf->sepc`——这次 ecall 仍然停留在 `trap.c` 的 ecall 分支内部（没有经过 `yield()`/`swtch()` detour），即将执行的 `sret` 读的是硬件 CSR 本身，不是 `tf->sepc`，只写后者对这次即将发生的 `sret`没有任何效果。修复是新增 `write_sepc()`，在改写 `tf->sepc`/`tf->sp` 的同时也直接 `csrw` 改写 CSR 和 `trap_saved_user_sp` 全局变量。
10. **proc.c：`fork_copy_page()`/`sys_fork()`。** 跟 x86_64 版本逻辑一致（整页复制，不是 copy-on-write）。子进程返回值寄存器是 `a0`（不是 x86_64 的 `rax`），同样靠 `proc_alloc_skeleton()` 的 `memset` 天然置零，不需要额外赋值。`sys_fork()` 需要设 `child->tf->sepc = read_sepc()`/`child->tf->sp = trap_saved_user_sp`/`child->tf->sstatus = 初始值`。
11. **proc.c：`sys_exec()`。** 跟 x86_64 版本逻辑一致（`pagetable_unmap()` 撤销旧映射，重新 `map_user_prog()`，`write_sepc()` 改写入口/栈顶）。
12. **proc.c：`sys_wait()`。** 跟 x86_64 版本逻辑一致。
13. **trap.c：`syscall_dispatch` 新增 `SYS_FORK`/`SYS_EXEC`/`SYS_WAIT` 三个分支。** 直接转发给 `proc.c` 的同名函数，不需要额外参数搬运（跟 x86_64 版本需要额外传两个指针不同）。
14. **kernel_main.c：** 在 Lab6 已有的初始化序列之后，`proc_init()` + `proc_alloc()` 创建初始进程，`scheduler()`（永不返回，取代原来手工调用 `enter_user_mode` 的位置）——riscv64 这边没有 x86_64 版本 `tss_init()` 的等价步骤（per-进程内核栈顶完全通过 `trap_kernel_sp_top` 这一个全局变量表达，不需要额外初始化任何硬件结构）。
15. **user_prog.S：** 跟 x86_64 版本逻辑一致（fork/exec/wait 调用序列 + 最开头的 reborn 判断分支）。

## QEMU 运行命令

```bash
cd labs/lab07-process-scheduler
make ARCH=x86_64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=x86_64 LAB=lab07-process-scheduler VARIANT=solution TIMEOUT=15
```

x86_64 预期输出（本 README 撰写时在 QEMU 里实测确认，对应 `tests/expect-x86_64.txt`）：

```
Hello OS from x86_64 (Lab7: processes & scheduling)
memmap: 2 available region(s) from Multiboot2 mmap tag,
switched to page table, low identity map gone
timer armed, syscall entry armed, creating initial process
initial process created, entering scheduler
root: forking child
root: waiting for child
child: running, calling exec
child: reborn after exec, exiting
root: child reaped
```

```bash
make ARCH=riscv64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab07-process-scheduler VARIANT=solution TIMEOUT=15
```

riscv64 预期输出（本 README 撰写时在 QEMU 里实测确认，OpenSBI 启动横幅省略，对应 `tests/expect-riscv64.txt`）：

```
memmap: 1 region(s) from DTB /memory,
Hello OS from riscv64 (Lab7: processes & scheduling)
switched to page table, low identity map gone
timer armed, ecall entry armed, creating initial process
initial process created, entering scheduler
root: forking child
root: waiting for child
child: running, calling exec
child: reborn after exec, exiting
root: child reaped
```

两边的行为完全对等：初始（root）进程 fork 出一个子进程，父进程 `wait()` 阻塞式查询（本 Lab 的 `sys_wait()` 是一次性查询不是真正阻塞，父进程用忙等重试直到查到），子进程 `exec()` 成一份全新加载的同一个 `user_prog.bin`（"reborn"），走到 reborn 分支直接 `exit()`，父进程查到子进程已经变成 `ZOMBIE`，回收之后自己也退出，落进死循环，`run-qemu.sh` 靠 `TIMEOUT` 强制终止。

## GDB/QEMU Monitor 调试方法

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab07-process-scheduler VARIANT=solution
```

```gdb
target remote :1234
file solution/x86_64/build/kernel.elf
break scheduler
break sys_fork
break sys_exec
commands
  print *g_current
  continue
end
break tss_init
```

排查"调度器到底有没有真的在轮换进程"最有用的做法：在 `scheduler()` for 循环内部（找到 `RUNNABLE` 进程、即将 `swtch()` 之前）打断点，逐次核对 `p->pid`/`p->state`，确认确实在多个进程之间轮换，不是反复调度同一个；在 `sys_fork()`/`sys_exec()` 里打断点，核对子进程 `struct trapframe` 里的 `rip`/`rsp`（riscv64 是 `sepc`/`sp`）是否真的被正确设置。

x86_64 侧排查 TSS/GDT 相关的坑，`info registers` 里 `$tr`（Task Register）能确认 TSS 是否真的被 `ltr` 加载：

```gdb
info registers
print/x $tr
```

`ltr` 成功不代表 TSS 里的 `base` 地址一定正确（`ltr` 只校验描述符格式，不校验 `base` 是否指向一块有意义的内存）——真正暴露问题往往要等到第一次 CPL3→CPL0 的 IDT 门提权发生（本 Lab 是第一次用户态代码存活到被定时器打断的场景），此时如果看到级联的 `#PF`/`#GP`/三重故障，先怀疑 TSS 描述符的 `base` 字段是否完整（尤其是 bit 31:24 那一段最容易漏），用 QEMU 的中断日志能直接看到完整的故障链路：

```bash
qemu-system-x86_64 ... -d int,cpu_reset -D guest.log
```

riscv64 侧排查 `sepc` 跨进程/跨 detour 被覆写的坑，同样用中断日志比 GDB 断点更直接（问题本身就是"时序"，单步执行会改变原本的时序）：

```bash
qemu-system-riscv64 ... -d int -D guest.log
```

一旦看到某个进程的 `epc` 突然跳到另一个完全不相关的地址范围，或者同一个 ecall 的 `epc` 反复出现（无限循环），基本可以确定是某处忘了在 `yield()`/`sys_exec()` 前后保护 `sepc`。

## 自动验收测试

```bash
cd labs/lab07-process-scheduler
bash ../../scripts/test-lab.sh ARCH=x86_64 LAB=lab07-process-scheduler
bash ../../scripts/test-lab.sh ARCH=riscv64 LAB=lab07-process-scheduler
```

`tests/expect-*.txt` 用逐行子串匹配，不要求整行完全一致——`fork`/`wait` 之间的相对顺序（谁先打印"forking child"还是"waiting for child"）本 Lab 的实现选择在 fork 之后立刻打印,再进入 wait 循环,如果学生的实现调整了这个顺序,只要最终输出的固定文本片段依次按顺序出现,测试依然通过。

## 常见坑与排查

以下七个都是本 Lab 开发过程中**实测踩到、定位、修复过的真实 bug，不是猜测或者理论上可能发生的情况**，横跨两个架构：

- **（两边通用，`user_prog.S`）"reborn 判断"没有放在 `_user_start` 最开头，导致 fork/exec 无限循环。** 子进程 `exec()` 之后会从入口重新执行整段用户程序——如果判断"这是不是 exec 之后的重新加载"这个分支被安插在代码中间（比如放在第一次 fork 调用之后），子进程重新执行到这里时会把自己也当成"第一次执行的 root 进程"，再次 fork、再次 exec，形成无限增长的进程链，直到进程表耗尽后 `proc_alloc()` 返回 NULL。修复方式是把这个判断挪到 `_user_start` 逐字节意义上的第一条指令，确保子进程从 exec 之后的入口点恢复执行时，第一件事就是识别出自己的身份，不会有任何机会先执行到 fork 调用。
- **（riscv64，`proc.c` `sys_exec()`）只写 `tf->sepc`/`tf->sp`，对这次即将发生的 `sret` 没有任何效果。** 这次 ecall 全程停留在 `trap.c` 的 ecall 分支内部（没有经过 `yield()`/`swtch()` 的 detour），即将执行的 `sret` 读的是硬件 `sepc` CSR 本身，不是内存里的 `tf->sepc`——写后者只是为了保持"`struct trapframe` 反映这个进程当前状态"的一致性（给将来某次真的经过 `trap_return` 路径时用），但对这一次的 `sret` 完全不起作用。实测症状：`qemu-system-riscv64 -d int` 显示同一个 `ecall` 的 `epc`/`stval` 反复出现，`exec` 看起来"什么也没发生"，用户程序在原来的地址继续执行。修复：新增 `write_sepc()`，同时 `csrw` 改写 `sepc` CSR 本身和 `trap_saved_user_sp` 全局变量。排查方法：在 `sys_exec()` 返回前后分别 `print/x $pc`（GDB 无法直接读 CSR，需要用 QEMU monitor 的 `info registers` 或者 `-d int` 日志核对 `epc` 字段）。
- **（riscv64，`trap.c`/`proc.c` `timer_interrupt_handler()`）同一个 `sepc` 覆写问题的第二次独立发作，触发路径完全不同。** 定时器 tick 打断进程 A，A 被标成 `RUNNABLE`、`swtch()` 回 `scheduler()` 之后，`scheduler()` 的 for 循环可能先轮到进程 B——B 执行期间自己的每一次 trap（ecall、下一次定时器 tick）都会覆写同一个 `sepc` CSR，等 A 终于被重新 `swtch()` 回来、一路返回到 `sret`，CSR 里躺着的早已是 B 最后一次 trap 留下的值，不是 A 被打断那一刻的真实 `sepc`。跟上一条 bug 的根因完全相同（`sepc` 是 per-hart CSR，不是 per-process 存储），但保护窗口不同——上一条只需要保护"这次 trap 内部、没有 detour"的场景，这一条需要跨越任意长的 `yield()`/`swtch()` detour。修复：用 `struct proc` 自己的 `tf->sepc` 字段（这是进程自己的存储，不会被其它进程覆写）在 `yield()` 调用前后分别保存/恢复。排查方法同上，`-d int` 能看到某个进程的 `epc` 突然跳到另一个进程的地址范围。
- **（x86_64，`proc.c`）`g_scheduler_context` 声明成 `struct context` 而不是 `struct context *`，GCC 直接报类型错误。** `swtch()` 的第一个参数要求 `struct context **`（需要把调用者的上下文*写回*调用者能找到的位置），如果 `g_scheduler_context` 本身是值类型，`&g_scheduler_context` 算出来的是 `struct context *`，比 `swtch()` 期望的类型少一层指针，编译时直接触发 `-Wincompatible-pointer-types`——这是本 Lab 唯一一处在实际编译这个文件时被编译器直接抓到、不需要跑起来才能发现的错误，修复只是把声明类型改成指针。
- **（x86_64，`trap.c` `tss_init()`）TSS 描述符 `base` 字段的最高字节被截断，导致仅在第一次真正的 CPL3→CPL0 IDT 门提权发生时触发级联故障。** 64 位系统描述符把 32 位的 `base` 地址拆成三段分别打包进描述符不同的字节位置，其中最高 8 位（bit 31:24）需要单独一行代码打包进描述符高 8 字节的 bit 56:63——漏掉这一行，`base` 的 bit31 被静默截断为 0。本 Lab 的 TSS 结构体恰好链接在一个 bit31=1 的高地址（`0xffffffff80109040`），截断之后的 `base` 变成 `0xffffffff00109040`，落在 `KERNEL_PML4_INDEX` 覆盖的规范高地址范围之外。这个错误不会被 `ltr` 指令本身发现（`ltr` 只校验描述符格式是否合法，不校验 `base` 指向的地址是否有意义），也不会在 Lab1-6 任何时候暴露（用户态代码从未持续运行到被定时器打断，TSS.RSP0 从未被真正读取过）——本 Lab 第一次让用户态代码运行足够长时间，第一次真正触发一次 IDT 门提权时，硬件读到错误的 `base`，实测复现的完整故障链路：`#PF (CR2=0xffffffff00109044)` → 递归 `#PF`（`old:0x0e new:0x0e`）→ 尝试投递 `#DF` 时又触发 `#GP`（`old:0x08 new:0x0d`）→ 三重故障，QEMU 静默复位。排查方法：`qemu-system-x86_64 -d int,cpu_reset` 能看到完整的级联链路，从最初那个 `CR2` 值反推是 TSS/GDT 相关地址错误，而不是某个具体的内存访问逻辑写错了。
- **（x86_64，`trap.c`/`proc.c` `syscall_kernel_rsp`）忘记在调度器切换进程时同步更新，导致 SYSCALL 路径继续复用旧的共享栈，覆盖 `scheduler()` 自己挂起状态所在的位置。** SYSCALL 完全绕开 IDT/TSS，`syscall_entry` 换栈靠的是 `syscall_kernel_rsp` 这个自定义全局变量，不是 TSS.RSP0——如果 `scheduler()` 只更新了 TSS.RSP0（给 IDT 门提权路径用）却忘了同时更新这个变量，SYSCALL 路径会一直换到某个固定的旧值（比如最初的 `__stack_top`）。而 `scheduler()` 自己也是通过 `swtch()` 挂起在这个共享栈上的某个深度——如果某个进程的 SYSCALL 处理函数（比如 `sys_exit_proc()` 内部触发的 `swtch()`）在这个共享栈上压栈的深度超过了 `scheduler()` 挂起时的位置,就会直接覆盖 `scheduler()` 保存的返回地址。实测症状：`ret` 跳到 `RIP=0`,伴随一次指令取指缺页 `#PF (not-present,read,kernel), CR2=0`——`RIP=0` 是"读到一段被清零的栈内容当成返回地址"的典型特征。排查方法：GDB 在故障发生前查看故障线程的调用栈深度，跟 `scheduler()` 上一次 `swtch()` 调用点的栈深度做对比；或者直接检查 `scheduler()` 里 `tss_set_rsp0()` 和 `syscall_set_kernel_rsp()` 是否成对出现，漏了后者的话这个 bug 几乎必然发生。
- **（x86_64，`trap_entry.S` `syscall_entry`）少了一条 `mov %r9, %r8`，导致 `sys_exec()` 的两个输出参数指针互相别名。** 计算好 `user_rip_slot`（`lea 8(%rsp), %r8`，指向刚 push 的 RCX）和 `user_rsp_slot`（`lea syscall_saved_user_rsp(%rip), %r9`，全局变量地址）之后，按 SysV 调用约定把它们分别搬进第 4/5 个参数寄存器（RCX/R8）——`mov %r8, %rcx` 这一步会先把 `user_rip_slot` 的值搬进 RCX，这时候如果不紧接着执行 `mov %r9, %r8` 把 `user_rsp_slot` 的值搬进刚空出来的 R8,R8 会保留它刚才那个旧值,也就是"搬进 RCX 之前 R8 自己的值"——刚好等于 `user_rip_slot`。结果 `syscall_dispatch()` 的第 4/5 个参数其实指向同一个地址,`sys_exec()` 内部 `*user_rip_slot = USER_PROG_VADDR; *user_rsp_slot = user_stack_top;` 两条赋值全部落在同一个地方,第二条覆盖第一条,真正应该被改写的 `syscall_saved_user_rsp` 全局变量则完全没被碰到。实测症状：`exec()` 之后 `pop %rcx`/`sysretq` 跳到的地址是 `user_stack_top`（比如 `0x402000`，刚好在映射的栈页尾部之外一个字节，一个 guard page），触发 `#PF (not-present,read,user)`（取指也算一次读）——只在"刚调用完 exec、这次系统调用即将返回"这一个特定时刻出现，fork/write/wait 因为不写这两个槽位，完全不受影响。排查方法：在 `syscall_entry` 里 `mov %r8, %rcx` 之后单步，`print/x $r8`/`$r9` 核对两者是否指向不同地址。
- **（两边通用，`proc.c` `sys_exec()`）对同一个虚拟地址重复 `pagetable_map()`，触发"已映射"的 panic。** 早期实现直接对当前进程重新调用 `map_user_prog()`，没有先撤销旧的映射——`pagetable_map()` 本身的教学设计是"发现目标虚拟地址已经有映射就直接 panic，不做隐式覆盖"（延续 Lab4 建立的"不允许静默覆盖已有映射"这个约定），`exec()` 恰好需要对同一个进程重新走一遍原来已经映射过的地址。修复是新增 `pagetable_unmap()`（Lab7 新函数），`sys_exec()` 在重新映射之前先撤销旧的用户程序页/用户栈页映射。

## 挑战任务

- 给 `sys_wait()` 实现真正的阻塞（而不是本 Lab 这种"查询不到就返回 -1，调用者自己忙等重试"），需要引入一个新的进程状态（比如 `WAITING`）和"子进程退出时主动唤醒正在等它的父进程"这个动作，体会为什么真实内核需要一个显式的阻塞/唤醒机制，而不能只靠状态轮询。
- 把 `fork()` 改造成真正的 copy-on-write：父子进程初始共享同一份物理页（页表项标记只读），只有任意一方尝试写入时才触发缺页、内核在缺页处理里现场分配新页并复制内容。需要扩展本课程目前"发生 page fault 就 panic"的处理逻辑（Lab4 起一直是"不实现故障恢复"），第一次真正区分"合法但需要内核介入的缺页"和"非法访问"。
- 让 `exec()` 支持"内嵌多个不同的用户程序，按名字选择加载哪一个"，而不是本 Lab 唯一能换成的同一份 `user_prog.bin`——体会没有文件系统的情况下，"按名字查找程序"这个最基础的功能需要多少额外的手工登记（多份 `.incbin`，一个名字到地址范围的映射表），对照真实系统里这一步是文件系统 + ELF loader 共同完成的。
- 实现一个真正的优先级调度（而不是本 Lab 的简单轮转），给 `struct proc` 加一个优先级字段，调度器选择下一个进程时按优先级而不是进程表顺序——再引入一种"优先级反转"的场景（比如高优先级进程在等一个被低优先级进程持有的资源），体会为什么真实内核的调度器往往比"排序一下"复杂得多。

## 参考

- Xv6 (MIT 6.828 教学操作系统)，`proc.c`/`swtch.S`/`trap.c` 的整体结构（本 Lab `swtch()`/`trap_return`/进程表的设计思路直接受其启发，但本 Lab 的 fork/exec 简化程度比 xv6 更高）
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Chapter 8 "Task Management"（TSS 结构、64 位系统描述符格式、`base` 字段的具体打包规则）
- Intel SDM, Volume 3A, Chapter 6 "Interrupt and Exception Handling"（IDT 中断门提权时如何从 TSS 读取 `RSP0` 的权威描述）
- The RISC-V Instruction Set Manual, Volume II: Privileged Architecture，"sepc"一节（`sepc` 作为 per-hart CSR 而非 per-process 存储的权威定义，本 Lab riscv64 两个真实 bug 的根因依据）
- OSDev Wiki，"Kernel Multitasking"/"Context Switching"页面（社区整理的上下文切换常见设计，本课程从零推导后确认与其一致）

## 下一步

进入 [Lab8：简单文件系统与块设备](../lab08-filesystem/README.md)。

本 Lab 的进程只能运行编在内核镜像里的程序，数据也只存在内存里——一断电什么都不剩。Lab8 要接上第一个**持久化存储**：先写一个块设备驱动（x86_64 用 ATA PIO 端口 I/O，riscv64 用 virtio-blk 的描述符环，这是整个课程里两个架构分歧最大的一次），再在它上面实现 xv6 风格的只读 inode 文件系统（超级块、位图、inode 表、目录项），最后用 `open`/`read`/`close` 三个系统调用把**文件描述符**这个抽象交给用户程序。

本 Lab 留下的两个特性会在 Lab8 变成硬约束，届时会再展开：时钟中断处理程序会 `yield()`，所以文件系统里所有块缓冲区必须在栈上，不许用 `static`（否则会被另一个进程覆盖，症状是偶发读到别的文件的内容）；进程状态里没有"阻塞等待"这一档，所以块设备驱动只能轮询而不能用中断驱动——真正的 sleep/wakeup 要等到 Lab10。

至于本 Lab 缺的进程间通信和同步机制：管道会在 Lab9 随 shell 一起出现，自旋锁和内存序是 Lab10 的主题，而 sleep/wakeup 补在 Lab10 讨论并发时最自然。
