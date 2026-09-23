# Lab9：Shell 与用户程序

## 学习目标

- 理解"从磁盘加载并运行一个真正的、编译产物是标准 ELF 格式的程序"这件事，本质上是内核对一份**不受信任的字节流**做验证、再把验证通过的内容映射进一片新地址空间——本 Lab 第一次让内核读的不是自己链接时嵌进镜像的固定字节（Lab7/Lab8 的 `user_prog.bin`），而是文件系统里任意一个、名字任意、内容格式必须自己解析的文件。`exec_load()` 因此被严格切成两半：只读的校验阶段（任何失败都能干净返回 -1，调用者的地址空间完全不受影响）和不可逆的提交阶段（`uvm_clear()` 之后没有回头路，任何失败只能 panic）——这条"提交线"是本 Lab ELF 加载器最核心的设计决策，直接决定了 shell 打错命令名时能不能优雅地打印"command not found"然后继续，而不是让内核直接崩溉。
- 亲手实现一条单向管道（`pipe.c`）：一个环形缓冲区、一对**单调递增**的读写计数器（不是回绕的头尾下标）、四个独立的开启端引用计数（`nread_open`/`nwrite_open`）。理解为什么用计数器而不是布尔值——本 Lab 的 `struct file` 直接嵌在每个进程的 PCB 里（不是 xv6 那种全局共享、按引用计数的 file 表），`close()` 是"每个进程的每个 fd 各调用一次"，如果某一端只用布尔值记录"是否关闭"，父进程和其中一个子进程谁先 `close()` 都会把管道错误地标记成已关闭，另一端还在读写的一方会立刻看到 EOF 或者错误。
- 通过实现 shell 本身（`sh.c`）体会"用户态程序"和"内核提供的机制"之间的真实分界：管道、重定向、`fork`/`exec`/`wait` 全部是内核已经提供好的原语，shell 要做的只是**组合**它们——`cmd1 | cmd2` 翻译成"两次 fork，用 `close()`/`dup()` 把其中一个子进程的 fd 1 换成管道写端、另一个子进程的 fd 0 换成管道读端，父进程两次 `wait()`"。全程没有任何新的内核态代码支持这个操作，纯粹是系统调用的排列组合——这跟真实 Unix shell 的实现思路完全一致，只是本 Lab 的 `sh.c` 只支持单级管道。
- 再一次通过本 Lab 开发过程中踩到的真实 bug，巩固"当前测试用例没有暴露某个缺口"和"这个缺口不存在"是两件不能划等号的事——本 Lab riscv64 侧一处 bug 的修复过程里，第一次修复的说明文字曾经写下"init.c/sh.c 在 fork() 调用点前后的代码足够简单，编译产物没有触发这个缺口"，这个判断被下一轮更彻底的测试（GDB 断点 + `objdump` 反汇编比对）证明是错的：GCC 编译 `init.c` 的 `main()` 时用 `s0` 做帧指针，而当时的轻量 trap 帧根本不保存 `s0`。这个教训——以及它被记录下来而不是被悄悄改写覆盖——本身就是本 Lab 想传达的工程方法论，详见"常见坑与排查"。

## 前置 Lab

依赖 [Lab8：简单文件系统与块设备](../lab08-filesystem/README.md)——本 Lab 复用 Lab8 建好的块设备驱动、只读 inode 文件系统（超级块、位图、inode 表、目录项）、`open`/`read`/`close` 三个系统调用的雏形，在此之上引入"从文件系统加载并运行任意程序"和"进程间管道通信"，不重新讲解文件系统本身的机制。

两个架构的 `boot.S`、`console_putc.c`、`panic_arch.c`、`memmap.c`、`pagetable.c`、x86_64 的 `grub.cfg`、`ide.c`、`pit.c`/`pit.h`，riscv64 的 `sbi.c`/`sbi.h`、`virtio.c`，以及两边的 `fs.c`/`fs.h`/`blk.h`/`fs_format.h`，这些文件和 Lab8 完全一样或只是在原有基础上追加，本 README 不重复讲解，只讲本 Lab 新增的部分：ELF 加载器（`exec.c`）、管道（`pipe.c`/`pipe.h`）、共享的 `elf.h`/`syscall.h`，以及 `solution/user/` 下的六个用户程序和极简 libc。

## 核心概念

**`exec_load()` 的"提交线"（committal line）——本 Lab 最重要的单一设计决策。** 把一份不受信任的字节流解析成可执行程序，天然分成两类操作：只读的检查（ELF magic 对不对、每个 program header 的地址范围合不合法、加起来需要多少物理页）和有副作用的修改（真的去分配物理页、真的把当前进程的地址空间清空重建）。如果这两类操作交织在一起——比如边解析 program header 边直接映射——一旦在第三个 segment 才发现地址不合法，前两个 segment 已经真实占用了物理页、已经真实修改了页表，而调用者的旧地址空间早已被破坏，无法回退，也无法继续用旧程序运行下去。本 Lab 的解法是把 `exec_load()` 严格分成两次遍历 program header：第一遍只做校验、累加需要的页数（不接触地址空间），只有全部通过之后才跨过"提交线"——调用 `uvm_clear()` 销毁旧地址空间；第二遍才真正加载每个 segment。跨过这条线之后任何失败都只能 `panic()`，因为已经没有可以安全返回的旧状态了；线之前任何失败都清理干净返回 -1。这条线直接决定了 shell 输错命令名时的行为：`exec()` 在校验阶段发现文件不存在或者不是合法 ELF，直接返回 -1，调用者（`sh.c` 的 `run_child()`）的地址空间完好无损，只是这次 `exec()` 没有成功——但因为 `run_child()` 是 `fork()` 出来的子进程,失败后直接 `exit(1)`，父 shell 用 `wait()` 收尸，提示行照常打出来，不受影响。

**argv 暂存缓冲区——因为参数字符串活在即将被销毁的地址空间里。** `exec(path, argv)` 的 `argv` 数组和它指向的每一个字符串，都存在调用者当前的用户地址空间——而 `exec_load()` 跨过提交线之后要做的第一件事就是销毁这个地址空间。如果不预先把这些字符串的内容复制到内核自己的存储里，销毁地址空间的那一刻这些字符串就变成悬空指针，之后再往新程序的栈上搭建 argv 时读到的是垃圾或者已经被复用的物理页内容。本 Lab 的做法是在校验阶段（提交线之前）把所有 argv 字符串逐个拷进一个内核静态缓冲区（`ubuf`/`uoff`/`unarg`/`ubytes`），提交之后再从这份内核自己的拷贝往新栈上搭建，不再触碰旧地址空间。这个静态缓冲区能安全复用而不需要加锁,依赖一个必须显式说明的前提：**exec 系统调用全程不会被切走**——两个架构上系统调用执行期间中断都是关闭的，`exec()` 这次调用从进入到返回之间不会有另一个进程插进来同时也调用 `exec()` 抢占这同一块缓冲区。

**ELF program header 的两段式加载统一了三种看起来不同的段形状。** 一个 program header 描述"文件里有 `p_filesz` 字节，加载到内存后要占 `p_memsz` 字节"——这两个数字的关系有三种情况：全等（纯代码/只读数据段，文件内容和内存内容一样多）、`p_filesz=0`（纯 `.bss`，内存要清零但文件里什么都没有）、`0 < p_filesz < p_memsz`（前一部分是初始化过的 `.data`，后一部分是紧跟着的 `.bss`，混在同一个 segment 里，这是链接器为了省一个 segment 做的常见优化）。本 Lab 的 `load_segment()` 不对这三种情况写三段独立分支，而是用统一的按页遍历：每一页先清零（`memset`），再判断这一页与文件内容的重叠区间（可能是整页、可能是 0 字节、可能是一部分），有重叠就从文件读那一部分覆盖过去，最后映射进页表——纯代码段恰好是"重叠区间等于整页"、纯 `.bss` 恰好是"重叠区间为 0"，混合段恰好是这两者之间的某个值，三种情况被同一段代码自然处理，不需要特判。六个用户程序里，`sh.c` 的全局变量 `prompt[]`（`static char prompt[] = "$ "`，可写的 `.data` 数组，故意不用 `const char *`）是唯一真正产生"混合段"的程序——这是刻意设计，为了让这条代码路径不是死代码。

**管道用单调递增计数器而不是回绕下标，回避空/满二义性。** 一个固定大小的环形缓冲区，如果只用两个在 `[0, PIPEBUF)` 范围内回绕的下标表示读写位置，`head == tail` 既可能表示"空"也可能表示"满"，需要额外的标志位或者少用一个槽位来区分。本 Lab 的 `struct pipe` 用两个**永不回绕、只增不减**的 `uint32_t` 计数器 `nread`/`nwrite`（依赖无符号整数运算的模 2^32 语义，写满 4G 字节才会真正溢出一次，实际使用中不会发生）：当前缓冲区里的字节数直接是 `nwrite - nread`（无符号减法，天然处理"写指针数值上比读指针小"的情况，因为两者都在同一个模空间里），空是 `nwrite == nread`，满是 `nwrite - nread == PIPEBUF`，取具体字节位置时再对 `PIPEBUF` 取模——这跟 Linux `kfifo` 用的是同一个技巧。

**管道的引用计数必须是真计数器，不能是布尔值，理由跟本 Lab 的 `struct file` 布局直接相关。** `pipe_alloc()` 创建时 `nread_open = nwrite_open = 1`；每次 `fork()` 复制文件描述符表时，如果某个 fd 指向管道，对应的计数器 `+1`；每次 `close()` 释放某个 fd 时，对应计数器 `-1`，减到 0 才真正释放这个管道槽位、并让另一端后续的读/写返回 EOF/错误。`sh.c` 跑 `cmd1 | cmd2` 时，管道的写端会同时存在于父 shell 自己的 fd 表（`pipe()` 刚创建时）、子进程 1（fork 继承）里，短暂地有两个引用；父 shell 和子进程 2 各自 `close()` 掉自己不需要的那一端时,只有当所有引用清零,写端才真正关闭,读端才会看到 EOF——如果用布尔值代替计数器,父 shell 一 `close()` 就会把管道错误标记为已关闭，子进程 1 还在写的数据可能被判定为写入已关闭的管道。

**没有锁,靠"系统调用期间中断关闭"提供事实上的原子性——这是本 Lab 明确标注为脆弱的简化。** `pipe.c` 全文没有一把锁,`pipe_read()`/`pipe_write()` 内部检查缓冲区空/满状态、决定是否要 `yield()` 让出 CPU 等待对端,这几步操作之间没有互斥保护。能这样写而不出错,依赖的前提跟 argv 暂存缓冲区那条一样：两个架构的系统调用执行期间中断都是关闭的，单核环境下,从进入 `pipe_read()`到第一次真正调用 `yield()` 之前,不会有任何其它执行流插进来读写同一个 `struct pipe`。这跟 xv6 教学操作系统里"持锁 sleep,唤醒后重新持锁"的模式起的是同一个作用,只是本 Lab 的"原子性"是系统调用规则白送的，不是真的锁——模块注释里明确写了：一旦引入多核或者允许系统调用被中断打断，这里就需要真正的每管道自旋锁，这是留给 Lab10/挑战任务的工作。

**`syscall.h` 从"每个架构一份"挪到 Lab 根目录，是一次对 Lab6 决策的真正推翻，不是延续。** Lab6 引入这个文件时，它只被内核自己的两处代码用到（`trap.c` 的分发逻辑、用户态汇编里手写的系统调用号），两边都在内核里，算"内核内部约定"，放每个架构自己的目录合情合理。本 Lab 里，`syscall.h` 的使用者变成四类：两个架构的内核 `trap.c`，以及新增的用户态 C 代码（`user.h` 的系统调用包装函数）和用户态汇编（`usys_<arch>.S`）——后两者跨越了特权级边界，这份文件的性质从"内核内部约定"变成了"内核与用户态之间的 ABI 契约"，契约的两侧必须看到同一份定义，放在每个架构目录各自一份意味着未来任何一次修改都要人肉同步两份、还要跟四个不同的消费者对齐,风险远高于收益。这跟 Lab8 `fs_format.h`（磁盘格式在"格式化工具的今天"和"内核读取的未来"之间必须一致）是同一个原则,只是这次跨越的是特权级边界而不是时间。

**`SYS_DUP` 的必要性——单有 `SYS_PIPE` 不足以实现管道重定向。** `sh.c` 执行 `cat f | grep x` 时，需要让 `cat` 的标准输出（fd 1）变成管道写端，而不是让 `cat` 自己知道"这次输出要写到管道"——`cat.c` 完全不需要修改，它只管往 fd 1 写。做法是标准的 `close(1); dup(pipe_wfd); close(pipe_wfd);` 三步：先关掉 fd 1 腾出这个编号，`dup()` 复制管道写端的文件描述符，`dup()` 的 POSIX 语义保证它返回**当前最小的空闲 fd**——刚好是 1，因为上一步腾出来了；最后关掉原来那个多余的 fd 编号。这个模式依赖 `dup()` 返回"最小空闲 fd"是一个必须遵守的行为，不是随便返回哪个空闲编号都可以的实现细节。

**fd 类型标签取代了 Lab8"fd==1 硬编码为控制台"的特判。** `enum fd_type { FD_NONE, FD_CONSOLE, FD_INODE, FD_PIPE }`——每个 `struct file` 显式携带自己的类型，`write`/`read`/`close` 系统调用先看这个标签再决定调哪个具体实现（写控制台、读写文件、读写管道）。这不是为了好看：Lab8 里 fd 1 永远等于控制台是一条硬编码的规则，本 Lab 一旦要支持"把 fd 1 换成管道写端"这种重定向,fd 1 就必须是一个可以被任意替换内容的、普通的文件描述符表条目,不能再有任何特判。

## x86_64 与 riscv64 对照表

| 维度 | x86_64 | riscv64 |
|---|---|---|
| trap 帧里用户 PC/SP 的存储方式 | 一直是 per-process 的（`struct trapframe` 里的 `rip`/`rsp` 字段），`sysretq` 依赖的 RCX/R11 只在 SYSCALL 那一瞬间有效，Lab7 起就没有全局变量能替代它们 | Lab7/Lab8 时期 `sepc`（用户 PC）和用户 `sp` 是**全局变量**（`g_current->tf->sepc`模式、`trap_saved_user_sp`），因为当时没有系统调用会在执行期间 `yield()`。本 Lab 引入会阻塞的系统调用（`sys_wait`/`pipe_read`/`pipe_write`/`console_read`），全局变量会被另一个进程的 trap 覆写——修复是把 `sepc`/用户 `sp` 挪进每次 trap 自己的栈帧槽位（`TF_SEPC`/`TF_SP`，`trap_entry.S` 里定义，`trap.c` 用 `FRAME_SEPC`/`FRAME_SP` 这两个 C 侧偏移常量镜像，又是一处"没有单一数据源、需要人肉对齐"的手工契约） |
| `sys_fork`/`sys_exec` 签名 | `sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot, struct context *gpr_snapshot)`——三个参数从 Lab7 起就有前两个（硬件强制：用户 RIP 活在寄存器里，必须传地址下去才能改写），本 Lab 新增第三个 | `sys_fork(uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot, uintptr_t *caller_frame)`——前两个参数是本 Lab **新增**才有的（上一行提到的 TF_SEPC/TF_SP 重构，把原来 riscv64 靠全局变量就能做到的事，改成跟 x86_64 一样显式传指针），第三个参数命名不同（`caller_frame`，不是 `struct trapframe *`——轻量 ecall 帧是跟完整 trapframe 不同、更小的独立布局） |
| fork 的 callee-saved 寄存器保存范围 | 一开始只保存 `rbx`/`rbp`/`r12-r15`（调用约定 callee-saved 集合），够用——GCC 编译 `init.c`/`sh.c` 时确实只用到了这几个寄存器做跨函数调用保留 | 经历了**两阶段修复**：第一次只扩展到 `ra`/`t0-t6`/`a0-a7`（16 字段，跟 Lab4 起 `trap_entry.S` 保存的集合一致），且当时的说明文字判断"当前测试用例的代码足够简单，不会触发这个缺口"；第二轮更彻底的测试（GDB 断点+`objdump`反汇编 `init.c` 编译产物）发现 GCC 用 `s0` 做帧指针（`sw a5, -20(s0)`），而 `s0` 完全不在保存范围内，导致子进程读到帧指针为 0，真实触发一次地址 `0xffffffffffffffec` 的缺页——最终扩展到完整 ABI callee-saved 集合（`s0-s11`+`gp`+`tp`，32 字段/256 字节，代价是**每次 trap**（不只是 fork 路径）多付 14 组 `sd`/`ld` |
| 为什么 x86_64 没有触发同等严重的 fork 寄存器丢失问题 | GCC 给 x86_64 生成的 `init.c`/`sh.c` 代码里，跨 `fork()` 调用点确实用到了 callee-saved 寄存器，但本 Lab 一开始保存的集合（`rbx`/`rbp`/`r12-r15`）恰好是 SysV ABI 的完整 callee-saved 集合，没有像 riscv64 的 `s0`/帧指针那样被漏掉的子集 | 同一类 bug（"fork 只复制了部分寄存器，遗漏了编译器实际依赖的那部分"）在 riscv64 独立发作了两次——本身就是"当前测试没暴露"≠"缺口不存在"这条教训的两次真实印证，第一次的错误判断被写进代码注释、又被下一轮测试推翻并如实记录，而不是悄悄改写 |
| 进程表大小 / 内核栈大小 | `NPROC` 从 Lab7/8 的 4 提到 8，`PROC_KSTACK_PAGES` 从 1 提到 2——两边改动理由相同（见下） | 同上（两个常量在两个架构的 `proc.h` 里各自定义，数值相同） |
| 本 Lab 新增的架构无关文件 | `exec.c`/`pipe.c`/`pipe.h` 两边各一份、要求逐字节相同（跟 `fs.c` 同一类"硬件差异已被更底层的文件挡掉"的文件） | 同上 |

## 代码目录与关键文件

```
labs/lab09-shell-userspace/
├── Makefile                     # 在 Lab8 基础上：新增 exec.c/pipe.c 到 ARCH_SRCS；新增用户程序构建流水线；
│                                 # fs.img 输入从 fsroot/ 文本文件扩展到 "文本文件 + 6 个用户程序 ELF"；
│                                 # check-fs-iface 改名 check-shared-iface，覆盖面扩大到 exec.c/pipe.c/pipe.h
├── README.md                    # 本文件
├── elf.h                        # Lab 根目录共享：ELF64 格式定义（Ehdr/Phdr/常量），System V ABI 规定，两个架构逐字节相同
├── syscall.h                    # Lab 根目录共享：系统调用号，SYS_PIPE=9/SYS_DUP=10 是本 Lab 新增；
│                                 # 从"每个架构一份"改为"根目录一份"（见"核心概念"）
├── fs_format.h                  # 和 Lab8 完全一样
├── fsroot/                      # 打进磁盘镜像的文本文件
│   ├── initrc                    # init 启动后自动跑的 shell 脚本（本 Lab 新增）
│   ├── motd.txt                   # 和 Lab8 一样
│   └── exact.txt                  # 和 Lab8 一样
├── mkfs/mkfs.c                  # 和 Lab8 完全一样（只是被喂了更多输入文件）
├── solution/
│   ├── x86_64/
│   │   ├── boot.S/linker.ld/console_putc.c/panic_arch.c/memmap.c/pit.c/pit.h/ide.c   # 和 Lab8 完全一样
│   │   ├── pagetable.c            # 在 Lab8 基础上追加地址空间常量（USER_MIN_VADDR/USER_STACK_TOP 等）
│   │   ├── fs.c/fs.h/blk.h         # 和 Lab8 完全一样
│   │   ├── proc.h                 # 新增 enum fd_type/struct file/struct uvm_page；
│   │   │                          # sys_fork 新增第三参数 gpr_snapshot（fork 寄存器丢失 bug 的修复）
│   │   ├── proc.c                 # fork/exec/wait 适配 ELF 加载 + fd 表 + 管道
│   │   ├── exec.c                 # 新增：ELF 加载器（本 Lab 核心）
│   │   ├── pipe.c/pipe.h          # 新增：管道实现
│   │   ├── trap.c                 # syscall_dispatch 新增 SYS_OPEN/READ/CLOSE/PIPE/DUP 分支
│   │   ├── trap_entry.S           # syscall_entry 新增：捕获 gpr_snapshot 传给 sys_fork
│   │   └── kernel_main.c          # 六步引导序列和 Lab8 完全一致，只是加载 /init 而不是内嵌 blob
│   └── riscv64/
│       ├── boot.S/linker.ld/sbi.c/sbi.h/console_putc.c/panic_arch.c/memmap.c/virtio.c   # 和 Lab8 完全一样
│       ├── pagetable.c            # 同 x86_64 侧
│       ├── fs.c/fs.h/blk.h         # 和 Lab8 完全一样
│       ├── proc.h                 # 除 x86_64 侧新增内容外，struct trapframe 新增 TF_SEPC/TF_SP 相关字段；
│       │                          # sys_fork/sys_exec 新增 user_rip_slot/user_rsp_slot/caller_frame 参数
│       ├── proc.c                 # 同 x86_64 侧 + write_sepc 类似的显式 CSR 同步逻辑
│       ├── exec.c/pipe.c/pipe.h    # 和 x86_64 侧逐字节相同（架构无关文件）
│       ├── trap.c                 # 同 x86_64 侧 + TF_SEPC/TF_SP 的 FRAME_SEPC/FRAME_SP 偏移常量
│       ├── trap_entry.S           # 轻量 ecall 帧从 16 字段扩展到 32 字段（fork 寄存器丢失 bug 的最终修复）
│       └── kernel_main.c          # 同 x86_64 侧
├── solution/user/                # 六个用户程序 + 极简 libc，两个架构共用源码
│   ├── user.h/ulib.c              # 系统调用包装（write/exit/fork/exec/wait/open/read/close/pipe/dup）+
│   │                              # ulib（strlen/strcmp/memset/memcpy/fputs/puts/fputd）
│   ├── user.ld                    # 两个架构共用的用户程序链接脚本（ENTRY=_start，加载地址 0x400000）
│   ├── crt0_x86_64.S/crt0_riscv64.S   # 每个架构的启动桩：设好栈指针，call main，main 返回后 exit()
│   ├── usys_x86_64.S/usys_riscv64.S   # 每个架构的系统调用陷入指令（syscall/ecall）包装
│   ├── init.c                     # pid 1：fork+exec /initrc 并 wait，之后无限循环 fork+exec 交互 shell
│   ├── sh.c                       # shell：readline/parse/管道/重定向/fork+exec
│   ├── ls.c/cat.c/echo.c/grep.c    # 四个最简单的用户程序
│   └── types.h                    # 用户态自己的基础类型定义（不能引用内核头文件）
├── starter/                      # 结构和 solution 一一对应，教学文件带 TODO
│   ├── x86_64/
│   ├── riscv64/
│   └── user/                     # 六个用户程序 TODO 化程度较轻，重点在 sh.c 的 parse/管道逻辑
└── tests/
    ├── expect-x86_64.txt
    └── expect-riscv64.txt
```

`exec.c`/`pipe.c`/`pipe.h` 两个架构逐字节相同，跟 Lab8 的 `fs.c` 是同一类文件——它们描述的是跟硬件无关的逻辑（ELF 解析、环形缓冲区管道），架构差异已经被 `blk.h`/`pagetable.h` 这一层挡掉了。`elf.h`/`syscall.h`/`fs_format.h` 三个头文件放在 Lab 根目录、不放每个架构目录下，是同一个原则在头文件层面的体现：它们描述的契约（ELF 格式、系统调用号、磁盘格式）本身跟架构无关。`solution/user/` 下六个用户程序的源码本身也是两个架构共用的（只有 `crt0_<arch>.S`/`usys_<arch>.S` 两个文件按架构区分——启动桩和系统调用陷入指令必然是架构相关的，其它一切都是标准 C）。

## 分步实现步骤

### 两个架构共用的部分（`elf.h`/`syscall.h`/用户程序）

1. **`elf.h`：`struct Elf64_Ehdr`（64 字节）/`struct Elf64_Phdr`（56 字节）+ `EI_*`/`PT_*`/`PF_*` 常量，`_Static_assert` 校验两个结构体大小。** ELF 是 System V ABI 规定的格式，不是本课程自定义的，没有"按架构调整"的余地——结构体布局两个架构完全一样，唯一按架构变化的是 `e_machine` 字段的*数值*（`EM_X86_64=62` / `EM_RISCV=243`），这是数据差异不是格式差异，`exec.c` 里用一处 `#if` 判断即可，不需要拆成两份头文件。**`p_flags` 在 ELF32 是倒数第二个字段，ELF64 挪到了第二位**（为了在把地址/长度字段加宽到 64 位之后避免额外的 padding）——这是端口 ELF 定义时的经典陷阱，务必对照 ELF64（不是 ELF32）的官方定义。**`PF_X=1`/`PF_W=2`/`PF_R=4`，跟熟悉的 Unix `rwx=4/2/1` 顺序正好相反**——`exec.c` 翻译成本课程自己的页表权限位时必须逐位判断，不能整体位运算平移，一旦搞反,某个只读数据段会被标记成可执行,不会报错、没有任何症状，只是留下一个不该有的可写或可执行权限。
2. **`syscall.h`：新增 `SYS_PIPE=9`/`SYS_DUP=10`。** 放在 Lab 根目录（不是每个架构目录），因为这份文件现在同时被两个架构的内核 `trap.c`、用户态的 `user.h`/`usys_<arch>.S` 四方共同依赖，是一份跨越特权级边界的 ABI 契约，见"核心概念"。
3. **`solution/user/user.ld`：链接脚本，两个架构共用一份。** `ENTRY(_start)`，加载地址 `0x400000`，显式列出 `.text`/`.rodata`/`.data`/`.bss`（含 riscv64 专属的 `.sdata`/`.sbss`——写进脚本对 x86_64 是无害的空匹配，漏掉的话 riscv64 的全局变量会静默错位）。跟 Lab7/8 手写汇编用户程序的链接脚本不同,这次必须区分并列出全部四类段,因为 C 编译产物真的会用到全部四类。
4. **`crt0_<arch>.S`：极简启动桩。** 设好初始栈指针（`user.ld` 定义的栈顶符号），`call`/`jal` 到 `main()`，`main()` 返回后调用 `exit()`——这是真实系统里 `crt1.o` 角色的最小实现。
5. **`usys_<arch>.S`：系统调用陷入指令的汇编包装。** 每个系统调用一段小汇编：把参数摆进约定寄存器，执行 `syscall`（x86_64）/`ecall`（riscv64），返回值留在 `rax`/`a0`。
6. **`user.h`/`ulib.c`：C 语言可调用的系统调用包装函数 + 最小 ulib。** `write`/`exit`/`fork`/`exec`/`wait`/`open`/`read`/`close`/`pipe`/`dup` 十个包装函数各自调用 `usys_<arch>.S` 里对应的汇编入口；`strlen`/`strcmp`/`memset`/`memcpy`/`fputs`/`puts`/`fputd` 是给六个用户程序共用的最小字符串/输出工具。**用户态不能直接 `#include` 内核的 `string.c`**——两者活在完全不同的地址空间，内核代码里的符号对用户态链接器完全不可见，必须重新写一份。
7. **`init.c`：两阶段设计。** 阶段一 `fork()` + `exec("/sh", {"sh", "/initrc", NULL})`，然后阻塞 `wait()`——这是本课程第一个真正会阻塞等待的系统调用（Lab8 的 `sys_wait` 是非阻塞查询,查不到立即返回 -1）。`wait()` 返回后打印 `init: initrc finished`——这一行是自动化测试的锚点,把"输出顺序正确"变成一条可以直接 grep 的性质。阶段二：无限循环 `fork()` + `exec("/sh", {"sh", NULL})`（交互式，不带脚本参数）+ `wait()`,确保 pid 1 永不退出（本课程内核没有孤儿进程收养机制,pid 1 退出会导致后续行为未定义）。
8. **`sh.c`：`readline()`。** 一次读一个字节（不是整块读），因为内核控制台读没有行缓冲（没有 termios canonical mode 的等价物）——如果按大块读，会一直阻塞到攒够 N 字节或者遇到 EOF，跟"按下 Enter 立即响应"这个交互期望冲突。
9. **`sh.c`：`parse()`。** 原地 tokenizer（直接在输入缓冲区里插入 `\0` 分割 token,不额外分配内存),按空白字符切分,产出 `struct cmd {char **argv; char *infile; char *pipe_rest;}`。**`|`/`<` 前后必须有空白字符才会被识别**（没有真正的词法分析器,只是按空白切分),`a|b` 会被解析成一个完整的单一参数,不是管道。
10. **`sh.c`：`run_child()`（noreturn）。** 处理输入重定向（`infile` 非空则 `open()` 它、`close(0)`、`dup()` 换上去、`close()` 掉多余的 fd),然后 `exec()` 目标程序——**这里的 `exec()` 失败（比如命令不存在）必须让子进程 `exit(1)`,不能让子进程带着旧程序继续跑**,这正是"exec.c 的提交线设计让失败清晰可辨"这个核心概念在用户态的直接体现。
11. **`sh.c`：`run_cmd()`。** 无管道情况：`fork()` 一次,子进程调 `run_child()`,父进程 `wait()`。单级管道情况：`pipe()` 一次,`fork()` 两次（左边命令的子进程把 fd 1 换成管道写端,右边命令的子进程把 fd 0 换成管道读端),父进程负责关闭自己手里两端多余的管道 fd、两次 `wait()`——**整个实现里一共有六次 `close()` 调用（左子进程 2 次,右子进程 2 次,父进程 2 次),这是最容易漏、漏了最难查的部分**,每一次都需要对照 `pipe.c` 的 `nread_open`/`nwrite_open` 语义单独论证"为什么这里必须 close",漏掉任何一个都会表现成一次看起来是"对方程序卡住了"的挂起,但根因其实在管道的引用计数没清零。
12. **`sh.c`：`main()`。** 判断启动参数：带一个路径参数（脚本模式,`sh /initrc`）就打开这个文件当作标准输入的来源,一次性读完执行；不带参数（交互模式）就无限循环打印 `prompt[]`（故意声明成 `static char prompt[] = "$ "` 而不是 `const char *`,让 sh 成为唯一一个真正产生"部分初始化、部分补零"混合 ELF 段的用户程序,覆盖 `load_segment()` 的第三种情况）+ `readline()` + `parse()` + `run_cmd()`。**脚本模式的存在纯粹是为了自动化测试**：测试脚手架没有可靠的办法给 QEMU 轮询式的 UART 及时喂 stdin 字节而不产生时序竞争,把输入换成磁盘上一份固定文件,彻底消除了这种不确定性。
13. **`ls.c`/`cat.c`/`echo.c`/`grep.c`：四个最简单的用户程序。** 直接用 `user.h` 包装好的系统调用和 `ulib.c` 的字符串工具实现,没有额外的复杂度,主要作为练习"独立的、完整的 ELF 可执行文件"这个概念，同时给 `sh.c` 的重定向/管道逻辑提供真实的被驱动对象。

### x86_64 内核侧

1. **`elf.h`/`syscall.h` 引入之后，`proc.h`：新增 `enum fd_type`/`struct file`/`struct uvm_page`，`struct proc` 里加入 fd 表和已映射用户页的追踪列表。** `struct file` 直接嵌进 `struct proc`（不是全局共享表),`enum fd_type` 区分 `FD_CONSOLE`/`FD_INODE`/`FD_PIPE`,替代 Lab8"fd==1 硬编码是控制台"的特判。`NPROC` 从 4 提到 8（一条管道命令同时活着 init/sh/左子进程/右子进程 4 个进程,加上等待被收尸的僵尸子进程,原来的 4 已经零余量）,`PROC_KSTACK_PAGES` 从 1 提到 2（`exec` 的调用链 `syscall_entry→syscall_dispatch→sys_exec→exec_load→load_segment→fs_read→read_inode→blk_read` 比 Lab7/8 任何路径都深,栈上还有 `Elf64_Ehdr`/`Elf64_Phdr` 缓冲区,当前没有栈溢出保护页,课程选择宁可多给余量）。
2. **`exec.c`：`check_ehdr()`/`check_phdr()`/`flags_of()`。** 校验 ELF magic（`ELFMAG0-3`）、`EI_CLASS==ELFCLASS64`、`EI_DATA==ELFDATA2LSB`、`e_type==ET_EXEC`、`e_machine==EM_X86_64`；对每个 `PT_LOAD` program header 校验地址范围落在 `[USER_MIN_VADDR, USER_STACK_TOP)` 之内且不与已知区域重叠;`flags_of()` 把 `PF_R`/`PF_W`/`PF_X` 逐位翻译成本课程自己的页表权限位（务必逐位判断,见上面 `elf.h` 步骤里 `PF_*` 顺序反常的提示）。
3. **`exec.c`：`stage_argv()`。** 在提交线之前,把 `argv` 数组和每个字符串内容复制进内核静态缓冲区（`ubuf`/`uoff`/`unarg`/`ubytes`),依赖"exec 系统调用全程不会被切走"这个前提（两个架构系统调用期间中断都关闭）。
4. **`exec.c`：`exec_load()` 的两遍 program header 遍历 + 提交线。** 第一遍只校验、累加 `npages`,不接触地址空间;全部通过后调用 `uvm_clear()` 跨过提交线（销毁调用者当前的地址空间——这一步之后任何失败都只能 `panic`);第二遍 `load_segment()` 真正加载每个 `PT_LOAD` segment,再 `build_stack()` 把 `stage_argv()` 暂存的 argv 内容搬到新栈上,构造好 `argc`/`argv` 供 `crt0_x86_64.S` 传给 `main()`。
5. **`exec.c`：`load_segment()` 的按页统一加载。** 见"核心概念"——对每一页做"清零→有重叠就拷贝对应文件字节→映射",三种段形状（纯代码/纯 bss/混合）不写三条独立分支。
6. **`pipe.c`/`pipe.h`：`pipe_alloc()`/`pipe_read()`/`pipe_write()`/`pipe_close()`/`pipe_dup()`，静态 `pipe_table[NPIPE=8]`。** `PIPEBUF=512`（`_Static_assert` 校验是 2 的幂,方便取模用位运算),`nread`/`nwrite` 单调递增计数器,`nread_open`/`nwrite_open` 真计数器（不是布尔值）。这两个文件和 riscv64 侧逐字节相同,不含任何 `#if`。
7. **`trap.c`：`syscall_dispatch` 新增 `SYS_OPEN`/`SYS_READ`/`SYS_CLOSE`/`SYS_PIPE`/`SYS_DUP` 分支。** `sys_fork()` 签名新增第三参数 `struct context *gpr_snapshot`——在 `syscall_entry` 换栈之后立即捕获 `rbx`/`rbp`/`r12-r15`,复制进子进程 trapframe 的对应字段（`rax`/`a0` 除外,保持 `proc_alloc_skeleton()` 的 memset 零值,这正是 fork 子进程返回值应该是 0 的语义)——这是本 Lab 修复"fork 丢失 callee-saved 寄存器"这个真实 bug 的关键改动，详见"常见坑与排查"。
8. **`trap_entry.S`：`syscall_entry` 新增几条指令,在换栈之后立即把 `rbx`/`rbp`/`r12-r15` 打包传给 `syscall_dispatch` 作为 `gpr_snapshot` 参数。** x86_64 这边不需要像 riscv64 那样扩展轻量帧的大小——SysV ABI 的完整 callee-saved 集合本来就只有这几个寄存器,一次性够用,不需要经历两阶段修复。
9. **`kernel_main.c`：确认六步引导序列跟 Lab8 完全一致。** memmap 发现→建页表→GDT/TSS/IDT/syscall/PIT 初始化→blk+fs 初始化→`proc_init()`+一次 `proc_alloc()`→`scheduler()`。本 Lab 全部新内容都活在 `proc_alloc()`/`exec_load()`/用户态代码里,引导序列本身零改动——唯一的可见区别是这次加载的是磁盘上的 `/init`,不再是内嵌进内核镜像的字节块。

### riscv64 内核侧

1. **`proc.h`：跟 x86_64 侧相同的 `enum fd_type`/`struct file`/`struct uvm_page` + fd 表,`NPROC`/`PROC_KSTACK_PAGES` 同样提到 8/2。** 额外的、riscv64 独有的改动：**把 `sepc`/用户 `sp` 从全局变量重构成每次 trap 自己栈帧里的槽位**（`TF_SEPC`/`TF_SP`,定义在 `trap_entry.S`,`trap.c` 用 `FRAME_SEPC`/`FRAME_SP` 两个 C 侧偏移常量镜像）。Lab7/8 时期这两个值是全局变量是安全的,因为没有系统调用会在执行期间 `yield()`;本 Lab 引入 `sys_wait`/`pipe_read`/`pipe_write`/`console_read` 这些会阻塞的系统调用之后,全局变量会被另一个被调度进来的进程的 trap 覆写,必须挪进 per-trap 的存储。`sys_fork`/`sys_exec` 的签名因此新增 `uintptr_t *user_rip_slot, uintptr_t *user_rsp_slot` 两个参数——从"riscv64 不需要传指针,全局变量就够"变成跟 x86_64 从 Lab7 起就有的签名趋同,只是原因不同（x86_64 是硬件强制,riscv64 是为了正确性主动选择）。
2. **`trap_entry.S`：轻量 ecall 帧扩展。** 这一步经历了两阶段（见"常见坑与排查"完整叙述）：第一次只扩展到 `ra`/`t0-t6`/`a0-a7`（16 字段/128 字节),第二次（真正的最终版本）扩展到完整 ABI callee-saved 集合 `s0-s11`+`gp`+`tp`（32 字段/256 字节)。**这个代价是全局的,不是只有 fork 路径付**——每一次 trap（包括跟 fork 完全无关的 `write`/`read` 系统调用、每一次定时器 tick)都要多做这 14 组 `sd`/`ld`。
3. **`proc.c`：`write_sepc()`（如果 Lab7/8 尚未引入,本 Lab 首次或者继续沿用）。** `sys_exec()` 改写用户入口地址不能只写 `tf->sepc`——这次 `ecall` 全程停留在 trap 内部,即将执行的 `sret` 读的是硬件 `sepc` CSR 本身,不是内存里的字段,必须同时 `csrw` 改写 CSR。
4. **`exec.c`/`pipe.c`/`pipe.h`：直接复用 x86_64 侧源码,逐字节相同。** 这两个/三个文件不含任何架构相关内容,ELF 解析和环形缓冲区管道逻辑跟硬件无关,架构差异已经被 `pagetable.h`/`blk.h` 挡掉。
5. **`trap.c`：`syscall_dispatch` 新增同样五个分支（`SYS_OPEN`/`READ`/`CLOSE`/`PIPE`/`DUP`）+ 上面提到的 `FRAME_SEPC`/`FRAME_SP` 偏移常量。** `sys_fork()` 第三参数命名 `caller_frame`（不是 `struct trapframe *`——轻量 ecall 帧是独立、更小的布局,不能直接当满帧用),内部按偏移量取出 `s0-s11`/`gp`/`tp` 复制进子进程 trapframe。
6. **`kernel_main.c`：跟 x86_64 侧同样确认引导序列跟 Lab8 完全一致,零新增步骤。**

## QEMU 运行命令

x86_64：

```bash
bash ../../scripts/run-qemu.sh ARCH=x86_64 LAB=lab09-shell-userspace VARIANT=solution TIMEOUT=15
```

riscv64：

```bash
bash ../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab09-shell-userspace VARIANT=solution TIMEOUT=15
```

本 README 撰写时在 QEMU 里实测确认，x86_64 的完整输出（节选，`ls` 的顺序、inode 号取决于 `mkfs` 处理 `FS_IMG_INPUTS` 的顺序，是稳定值不是随机的）：

```
$ echo lab9 shell up
lab9 shell up
$ ls
exact.txt
initrc
motd.txt
init
sh
ls
cat
echo
grep
$ cat /motd.txt
osdev-lab9: userspace is alive
this file lives on a virtual disk, in a lab filesystem
read by cat, filtered by grep, both running in user mode
$ grep lab < /motd.txt
osdev-lab9: userspace is alive
this file lives on a virtual disk, in a lab filesystem
$ cat /motd.txt | grep lab
osdev-lab9: userspace is alive
this file lives on a virtual disk, in a lab filesystem
$ echo initrc done
initrc done
init: initrc finished
```

riscv64 的输出结构完全一致（`motd.txt` 同样是 inode 4、143 字节），只是引导阶段的日志行不同（virtio 而不是 IDE、Sv39 页表而不是四级 x86_64 页表）。之后 `initrc` 那六行 shell 交互输出（`echo`/`ls`/`cat`/`grep <`/`cat | grep`/`echo`）跟 x86_64 逐字节相同——这正是"ELF 加载器和管道逻辑架构无关"这条设计的直接证据：两个架构从磁盘加载同一批 ELF 文件、跑同一份 shell 脚本，用户可见的行为没有任何差异。

## GDB/QEMU Monitor 调试方法

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab09-shell-userspace VARIANT=solution
# 或
bash scripts/debug-gdb.sh ARCH=riscv64 LAB=lab09-shell-userspace VARIANT=solution
```

**排查 exec.c 提交线附近的问题**：在 `exec_load()` 里 `uvm_clear()` 调用前后各设一个断点，确认失败路径（比如故意 `exec()` 一个不存在的文件）永远停在 `uvm_clear()` 之前返回 -1，不会有任何一条日志/内存修改发生在提交线之后——这是验证"提交线"设计正确性最直接的手段：

```
(gdb) break exec_load
(gdb) commands
> print *ehdr
> continue
> end
(gdb) break uvm_clear
(gdb) commands
> print "crossing the committal line"
> continue
> end
```

**排查 fork 丢失 callee-saved 寄存器的问题（本 Lab 两个架构都真实踩过）**：在 `sys_fork()` 返回之后、子进程第一次真正运行起来的位置设断点，对比父子进程同一个寄存器（比如 riscv64 的 `s0`，x86_64 的 `rbp`）的值是否符合"子进程应该继承父进程 fork 调用点那一刻的值"这个预期：

```
(gdb) break sys_fork
(gdb) continue
(gdb) finish
(gdb) print $s0     # riscv64；x86_64 换成 print $rbp
```

**riscv64 侧 `sepc`/用户 `sp` 相关问题，优先用 QEMU 的中断跟踪日志而不是 GDB 单步。** 这类 bug 的本质是"两个并发执行流之间共享了本该独立的状态"，是一个跟*时间*相关的问题——GDB 单步会大幅改变两个执行流实际交替的时机，反而让 bug 不重现或者换个样子重现。优先用：

```bash
qemu-system-riscv64 -d int -D guest.log ...
```

跑起来之后翻 `guest.log`，核对某次 `ecall`/`sret` 前后 `epc`/`stval` 的值是否符合预期，比对相邻几次中断分别属于哪个进程的地址范围。

**x86_64 侧 TSS/GDT 相关问题**（本 Lab 沿用 Lab7 建好的 TSS，一般不需要重新排查，但如果改动了 `trap.c`）：

```
(gdb) info registers
(gdb) print/x $tr
```

## 自动验收测试

```bash
bash ../../scripts/test-lab.sh ARCH=x86_64 LAB=lab09-shell-userspace
bash ../../scripts/test-lab.sh ARCH=riscv64 LAB=lab09-shell-userspace
```

对应 `tests/expect-x86_64.txt`/`tests/expect-riscv64.txt`——按顺序检查一批子串是否依次出现在串口输出里（不要求整行完全匹配）。两份期望文件的核心断言序列相同：`init loaded`→`echo lab9 shell up`→`ls` 的 9 个条目→`cat /motd.txt` 的三行内容→`grep lab < /motd.txt` 过滤后的两行→`cat /motd.txt | grep lab` 同样的两行（验证管道跟直接重定向结果一致）→`echo initrc done`→`init: initrc finished`（脚本模式跑完的锚点行）。

## 常见坑与排查

以下几个都是本 Lab 开发过程中**实测踩到、定位、修复过的真实 bug，不是猜测或者理论上可能发生的情况**：

- **（两边通用，`proc.c`/`proc.h` `sys_fork()`）子进程丢失调用约定 callee-saved 寄存器，GCC 编译的用户程序（本 Lab 第一次出现）才会真正暴露，手写汇编用户程序永远不会。** `sys_fork()` 最初只显式设置 `rip`/`rsp`/`cs`/`ss`/`rflags`（x86_64）或 `sepc`/`sp`/`sstatus`/`a0`（riscv64）几个跟"恢复执行位置"直接相关的字段，其余寄存器停留在 `proc_alloc_skeleton()` 的 `memset` 零值。Lab7/8 手写汇编用户程序从不依赖"fork 之后某个寄存器还保留着 fork 之前的值"这个假设，所以从未暴露这个缺口。本 Lab 第一次让 GCC 编译的 C 代码（`init.c`/`sh.c`）跑起来——从调用者的角度看，`fork()` 只是一次普通函数调用，C 调用约定保证跨函数调用 callee-saved 寄存器的值会被保留，`init.c`/`sh.c` 编译产物里确实真实依赖了这个保证。riscv64 侧这个 bug **经历了两阶段修复**：第一次只把捕获范围扩展到 `ra`/`t0-t6`/`a0-a7`（16 字段，跟 `trap_entry.S` 从 Lab4 起就保存的集合一致），当时的修复说明写下"`init.c`/`sh.c` 在 fork 调用点前后的代码足够简单，编译产物没有触发这个缺口（这是实测确认过的，不是靠论证假设）"——**这个判断被下一轮更彻底的测试证明是错的**：在 `trap.c` 某处故障诊断行设 GDB 断点，配合 `objdump` 反汇编 `user/init` 的编译产物，发现 GCC 给 `init.c` 的 `main()` 用 `s0` 做帧指针（出现 `sw a5, -20(s0)` 这样的指令），而 `s0` 完全不在第一次修复捕获的范围内，子进程读到的 `s0` 是 0，访问 `0 + (-20)` 这个偏移，无符号表示成 `0xffffffffffffffec`，真实触发一次页错误。最终修复把 riscv64 轻量帧扩展到完整 ABI callee-saved 集合（`s0-s11`+`gp`+`tp`，32 字段/256 字节，是第一次修复范围的两倍），代价是每次 trap（不只是 fork 路径）多付 14 组 `sd`/`ld`。**这个教训本身值得记录，不只是记录修复本身**："当前测试用例没有暴露"和"缺口不存在"是两件不能划等号的事，第一次修复的判断没有被悄悄改写覆盖，而是被完整保留、标注为错误、附上推翻它的证据——这份记录本身就是本 Lab 想传达的方法论。排查方法：在子进程刚恢复执行的位置对比父子进程同名寄存器的值是否符合"应该被 fork 保留"的预期；怀疑帧指针相关问题时，`objdump -d` 反汇编用户程序，找 `sw`/`sd` 到 `s0`/`fp` 相对偏移的指令，确认它们是否落在 fork 保存范围内。
- **（riscv64，`proc.h`/`trap.c`/`trap_entry.S`）`sepc`/用户 `sp` 曾经是全局变量，本 Lab 引入的阻塞系统调用会让它们被另一个进程覆写。** Lab7/8 时期这两个值可以安全地是全局变量（`g_current->tf->sepc` 模式、`trap_saved_user_sp`），因为当时没有任何系统调用会在执行期间调用 `yield()`——一次系统调用从进入到返回之间，`g_current` 指向的永远是同一个进程，全局变量和"当前进程自己的状态"完全等价。本 Lab 引入 `sys_wait`（阻塞等待子进程退出）、`pipe_read`/`pipe_write`（阻塞等待对端）、`console_read`（阻塞等待输入）之后，这些系统调用内部会真正调用 `yield()` 让另一个进程运行——如果 `sepc`/用户 `sp` 还是全局变量，另一个被调度进来的进程自己的每一次 trap 都会覆写同一份全局存储，原来那个进程被重新调度回来、真正 `sret` 的时候读到的是别的进程留下的值。修复：把这两个值从全局变量挪进每次 trap 自己的栈帧槽位（`trap_entry.S` 定义 `TF_SEPC`/`TF_SP` 偏移，`trap.c` 用 `FRAME_SEPC`/`FRAME_SP` 两个 C 侧常量镜像同一组偏移——这是又一处"没有单一数据源，需要人肉保持两边同步"的手工契约，跟 Lab6 起 `ecall_handler` 里 `frame[N]` 下标必须跟 `trap_entry.S` 保存顺序同步是同一类简化）。排查方法：优先用 `qemu-system-riscv64 -d int -D guest.log` 而不是 GDB 单步（见"GDB/QEMU Monitor 调试方法"一节的说明——这类 bug 的本质是时序问题，单步会改变实际的交替时机）。
- **（两边通用，`sh.c` `run_cmd()`）管道场景漏掉任意一次 `close()`，表现成"看起来是对方程序卡住了"的挂起，但根因在自己这边。** 单级管道需要六次 `close()` 调用（左子进程 2 次、右子进程 2 次、父进程 2 次），每一次都需要单独对照 `pipe.c` 的 `nread_open`/`nwrite_open` 引用计数语义论证"为什么这里必须关闭"——比如父进程如果忘记关闭自己手里的管道写端，`nwrite_open` 永远不会降到 0，右边子进程（读端）在读到当前缓冲区内容之后，会因为写端"看起来还有人可能继续写"而继续阻塞等待，不会正确地收到 EOF，看起来像是右边程序卡死不退出，但真正的问题在父进程的 `close()` 漏了一次。排查方法：给 `pipe_close()` 加临时调试输出，打印每次调用后的 `nread_open`/`nwrite_open` 数值，核对是否在预期的时刻降到 0；或者用 GDB 在挂起时打印所有进程的状态（`RUNNABLE`/`SLEEPING`/`ZOMBIE`），确认到底是哪个进程真正卡在等待，再回溯它在等什么。
- **（两边通用，`exec.c`）如果不小心把校验逻辑和加载逻辑交织在一次 program header 遍历里，会破坏"提交线"设计，让 exec 失败变得不干净。** 一种自然但错误的实现方式是遍历一次 program header，边检查边直接分配物理页、直接映射——如果检查在第三个 segment 才失败，前两个 segment 已经真实修改了调用者的地址空间和物理页占用，此时既不能干净地返回 -1（调用者的地址空间已经被破坏了一部分），也没有完整加载出一个可运行的新程序。这不是本 Lab 实测踩到的 bug（两遍遍历的设计是从一开始就确定的），但值得作为"如果打算简化实现"的显式提醒：两遍遍历（先全部校验、累加页数，再跨过提交线加载）不是可以省略的冗余步骤，是"exec 失败时调用者能继续干净运行"这个行为的必要条件——shell 打错命令名之后还能正常打印提示继续接受输入，直接依赖这个设计。

## 挑战任务

- 给 `sh.c` 增加真正的多级管道支持（`cmd1 | cmd2 | cmd3`），需要把 `parse()` 从"最多识别一次 `|`"改成识别任意多次，`run_cmd()` 从"固定两个子进程"改成按管道段数动态 `fork()`，每一段的输入/输出各自换成前一个/后一个管道端——体会为什么真实 shell 的管道实现往往用一个动态数组或链表记录"这一串命令、这一串管道"，而不是硬编码两段。
- 把 `struct file` 从"嵌在每个 `struct proc` 里"改造成一个全局共享、带引用计数的 file 表（`dup()`/`fork()` 复制的是表项引用，不是整个 `struct file` 内容），体会真实 Unix `open()` 之后两个不同进程通过 `dup2`/继承共享同一个文件表项时，文件读写偏移量为什么是共享的——本 Lab 当前的实现里，`dup()`/`fork()` 复制出的 `struct file` 副本各自独立，同一个底层 inode 被两个 fd 各自维护自己的读写偏移量，跟真实 POSIX 语义不同。
- 给 `pipe_read()`/`pipe_write()` 实现真正的睡眠/唤醒，取代当前"忙等 `yield()`"的轮询实现，体会为什么真实内核需要一个显式的阻塞队列/唤醒机制——这个话题会在 Lab10 讨论并发时更系统地展开，本 Lab 可以先做一个局部的、只服务于管道这一个场景的最小版本。
- 给内核栈加一个真正的溢出保护页（当前 `PROC_KSTACK_PAGES` 提到 2 只是course主动选择"给更多余量"，不是真正的保护机制）：在内核栈下方留一个特意不映射的页，栈溢出时触发一次可诊断的缺页而不是静默覆盖别的内存。
- 给 `pipe.c` 补上真正的每管道自旋锁，替换当前"靠系统调用期间中断关闭"提供的事实原子性——想象一下如果 Lab10 引入多核之后，这份代码不加锁会在哪个具体的检查-修改序列上产生竞态,先把这个分析写清楚,再动手加锁,是理解 Lab10 要解决的问题的一个提前预习。
- 实现 `copy_from_user()`/`access_ok()`，让内核在解引用任何用户提供的指针（`exec()` 的 `path`/`argv`，`write()`/`read()` 的缓冲区）之前，先显式校验这个地址是否落在调用者地址空间的合法范围内——当前实现隐式信任所有用户传入的指针,一个刻意构造的、指向内核地址空间的指针会被直接解引用,这是一个真实的安全问题,不只是教学简化。

## 参考

- Xv6 (MIT 6.828 教学操作系统)，`exec.c`/`pipe.c`/`sh.c` 的整体结构（本 Lab ELF 加载器的两阶段设计、管道的环形缓冲区实现思路直接受其启发，但本 Lab 的 ELF 加载器只支持 `PT_LOAD` 段、shell 只支持单级管道，简化程度比 xv6 更高）
- System V Application Binary Interface, AMD64 Architecture Processor Supplement（`.dynamic`/`p_flags` 位序、64 位 ELF 结构体字段布局的权威定义）
- Executable and Linkable Format (ELF) Specification, Version 1.2（ELF 格式本身的权威定义，program header/section header 的语义）
- The RISC-V Instruction Set Manual, Volume II: Privileged Architecture，"sepc"一节（`sepc` 作为 per-hart CSR 而非 per-process 存储的权威定义，本 Lab riscv64 侧 TF_SEPC/TF_SP 重构的根因依据，跟 Lab7 riscv64 两个真实 bug 是同一类根因的延续）
- POSIX.1-2017，`dup()`/`pipe()` 章节（`dup()` 必须返回最小空闲 fd 这一行为的规范来源，本 Lab `sh.c` 的重定向实现直接依赖它）

## 下一步

进入 [Lab10：SMP、锁、高级主题、收尾对比](../lab10-smp-locks/README.md)。

本 Lab 的管道实现靠"系统调用期间中断关闭"白得了原子性,进程之间的调度仍然是单核轮转——Lab10 要拆掉这两个简化：x86_64 通过 ACPI MADT + APIC 启动其它核心,riscv64 通过 SBI HSM 扩展启动其它 hart,第一次让"多个执行流真的同时在跑"变成事实而不是靠时间片模拟出来的假象。一旦这件事成立,本 Lab `pipe.c` 顶部注释里提前预告的"一旦引入多核就需要真正的每管道自旋锁"就不再是假设性的警告,而会变成一个立即需要动手解决的真实问题。

Lab10 还会补上本课程一路留到最后的两块内容：真正的睡眠/唤醒机制（取代 Lab8 起块设备驱动的忙等轮询、本 Lab 管道的忙等 `yield()`），以及内存序的讨论（多核之后,"写操作对其它核可见的顺序"第一次成为一个必须显式面对的问题）。最后一节会做一次贯穿全课程的 x86_64 与 riscv64 架构对照复盘——回顾从 Lab1 的启动协议到本 Lab 的系统调用 ABI,两个架构在哪些地方选择了截然不同的机制解决同一个问题,又在哪些地方（比如本 Lab `sys_fork`/`sys_exec` 签名的最终趋同）殊途同归。
