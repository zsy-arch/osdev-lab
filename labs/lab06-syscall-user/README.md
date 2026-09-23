# Lab6：系统调用与用户态

## 学习目标

- 理解"特权级"这个概念在两种架构上分别是怎么落地的：x86_64 靠 CS 段选择子的低 2 位（CPL）+ SYSCALL/SYSRET 这一对专用指令；riscv64 靠 `sstatus.SPP`/`sstatus.SPIE`/`sstatus.SUM` 这几个 CSR 位 + `ecall`/`sret`。两边"用户态能做什么、不能做什么"的具体规则几乎不重叠，但都要解决同一个问题：怎么可控地从低特权级换到高特权级、再换回去。
- 亲手驱动一次"内核态构造好上下文，第一次跳进用户态"——这跟本课程之前所有 Lab 的"CPU 从复位开始就在内核自己手里"完全不同，第一次需要显式地把控制权交给一段权限更低的代码，并且保证交出去之后，如果那段代码想拿回更高权限，只能通过内核指定的唯一入口（SYSCALL/ecall），不能绕过。
- 通过实现 `sys_write`/`sys_exit` 这两个最小系统调用，理解"系统调用"本质上就是一次受控的特权级切换 + 参数传递 + 分发，硬件负责"怎么切换"，内核负责"切换之后做什么"，两者是分层的。
- 再一次通过本 Lab 开发过程中踩到的真实 bug（见"常见坑与排查"），巩固"编译通过、QEMU 里跑起来不崩溃"和"行为真的符合设计"是两件不同的事——本 Lab 四个真实 bug 里，两个是"表面正常但触发路径极其隐蔽"的类型（页表覆盖、CSR 位缺失），一个是"编译期就会报错但报错信息不直接指向根因"（riscv 汇编器对链接期表达式的限制），一个是本课程基础设施（`kalloc.c`）里潜伏了三个 Lab 都没触发、Lab6 第一次触发的设计缺口。

## 前置 Lab

依赖 [Lab5：中断、异常、trap、时钟](../lab05-interrupt-trap/README.md)——本 Lab 复用 Lab5 建好的页表框架、IDT/`stvec` trap 入口、定时器基础设施，全部原样调用，不重新实现。

`boot.S`、`console_putc.c`、`panic_arch.c`、`memmap.c`、`pagetable.c`（两个架构）、x86_64 的 `grub.cfg`、riscv64 的 `sbi.c`/`sbi.h` 这几个文件和 Lab5 完全一样，直接带过来，本 README 不重复讲解。x86_64 的 `linker.ld` 和 Lab5（继承自 Lab4）完全一样；riscv64 的 `linker.ld` 也和 Lab5 完全一样（包括 Lab5 修过的 `.sdata`/`.sbss` 匹配）。

## 核心概念

**特权级切换的本质：CPU 承诺"只有指定入口能提升权限"，软件在这个约束下自己保证正确性。** 无论是 x86_64 的 SYSCALL 还是 riscv64 的 ecall，硬件提供的保证都是同一件事——用户态代码执行这条指令时，CPU 会原子地（不会被中断打断到一半）跳到内核预先注册好的固定地址，并把特权级提升到内核态；用户态代码*不能*通过任何其它手段让 CPU 提升到内核特权级（比如直接写 CS 段选择子或者 CSR 是被禁止的操作，会触发异常，不会真的提权）。硬件只保证"入口在哪、怎么跳"，不检查"传进来的参数是否合法"——`sys_write` 拿到的用户指针可能指向内核不该暴露的地址，可能压根没映射，这一层校验完全是软件（内核）自己的责任，本 Lab 的实现刻意保持最简（只检查指针本身能不能被读，不做更严格的地址空间归属校验），这是有意的教学简化，不是真实内核的完整实现。

**x86_64：SYSCALL/SYSRET 是一对"跳过 IDT"的专用指令，靠 MSR 而不是内存里的表结构配置。** 跟 Lab5 里"中断/异常都走 IDT 查表"完全不同，SYSCALL 执行时不查任何内存里的表，入口地址直接来自 `LSTAR` 这个 MSR（Model-Specific Register，通过 `rdmsr`/`wrmsr` 指令访问，不是普通内存地址）。`STAR` MSR 编码了四个段选择子（内核 CS/SS、用户 CS/SS）的相对关系，且这四者在 GDT 里的排列顺序有硬性约束（详见下面"分步实现步骤"），`SYSRET` 靠这个约束反推用户态段选择子，不需要额外指定。第一次从内核跳进用户态不能用 `SYSRET`——`SYSRET` 恢复 RIP/RSP 靠的是 RCX/R11 这两个寄存器，但它们只在*真正执行过一次 SYSCALL*之后才会被硬件自动写入正确的值，本 Lab 场景下从来没有发生过 SYSCALL，RCX/R11 里没有任何有意义的值，所以第一次切换必须借用 IDT 路径本来就有的 `iretq`，手工在栈上构造一份完整的返回帧。

**riscv64：`ecall`/`sret` 和 Lab5 的 trap 入口是同一套机制,不是新增的独立路径。** riscv-privileged 规范里 `ecall` 本质上就是一种同步异常（`scause` 有专属的 code，S-mode 处理 U-mode 发起的 ecall 时 code 是 8），跟 Lab5 的定时器中断、Lab4 的 page fault 共享同一个 `stvec` 入口——这跟 x86_64 SYSCALL"跳过 IDT、走独立入口"是本 Lab 两条架构路线最大的结构性差异。但正因为共享同一入口，Lab5 原有的"trap 只会从 S-mode 发起，sp 已经是合法内核栈"这个前提被打破了：ecall 第一次让 trap 可能从 U-mode 发起，`trap_entry.S` 必须在触碰任何栈内容之前，先靠 `sstatus.SPP` 位判断这次 trap 从哪个特权级来，再决定要不要把 sp 换成内核栈——这一步做错的后果不是"编译错误"或者"立刻崩溃"，是用户栈上的内容被内核当成自己的栈使用，表现为随机的、难以复现的崩溃。`sret` 相比 x86_64 的 `sysretq` 有一个关键的自由度：它读的 `sepc`/`sstatus.SPP`/`sstatus.SPIE` 全部是软件可写的 CSR，不依赖"之前必须发生过一次 ecall"这个硬件前提,所以 riscv64 第一次和第 N 次特权级下降用的是*同一条*指令,不需要像 x86_64 那样区分"第一次用 iretq、之后用 sysretq"。

**两边都需要一个新的、之前从未出现过的 CSR/寄存器位来"允许 S-mode 访问带用户标记的页"，这不是可选项。** x86_64 如果开启了 CR4.SMAP（本课程没有启用，所以不受影响），S-mode 默认不能解引用 U 位页；riscv64 则不管开不开关都受 `sstatus.SUM` 位约束，且这个位的复位值是 0（禁止），QEMU/OpenSBI 都不会替内核代劳设置它——如果内核需要直接解引用用户传进来的指针（本 Lab `sys_write` 就需要），riscv64 版本必须显式把 SUM 置 1，否则会在页表项完全正确的情况下仍然触发 page fault，这是本 Lab 的一个真实 bug（见"常见坑与排查"），且是 riscv64 独有的坎，x86_64 侧因为本课程从未开启 SMAP，没有对应的坑。

## x86_64 与 riscv64 对照表

| 维度 | x86_64 | riscv64 |
|---|---|---|
| 触发指令 | `syscall`（用户态执行） | `ecall`（用户态执行） |
| 返回指令 | 第一次用 `iretq`，之后用 `sysretq` | 全部用 `sret`（无需区分第一次） |
| 内核入口配置方式 | `LSTAR`/`STAR`/`SFMASK` 三个 MSR（`rdmsr`/`wrmsr`） | 复用 Lab5 已经配置好的 `stvec`（Direct 模式） |
| 入口是否与其它 trap 共享 | 不共享，独立入口 `syscall_entry`（跳过 IDT） | 共享同一个 `supervisor_trap_entry`，靠 `scause`/`sstatus.SPP` 分发/判断来源 |
| 用户态发起时的栈切换责任 | 硬件*不会*自动切栈，`syscall_entry` 手动用全局变量暂存用户 RSP | 硬件*不会*自动切栈，`supervisor_trap_entry` 手动用全局变量暂存用户 sp,且必须在判断来源特权级*之前*不能碰栈 |
| 返回地址/PC 的保存方式 | 硬件自动写入 RCX（`syscall` 执行时的下一条指令地址） | 硬件自动写入 `sepc`，但*不会*自动 +4（跟 x86_64 的 RCX 不同，需要软件手动 `sepc += 4` 跳过 ecall 本身） |
| 系统调用号/参数传递寄存器 | RAX=调用号,RDI/RSI/RDX=参数（本 Lab 用到的子集） | a7=调用号,a0/a1=参数（复用 SBI 调用约定的思路,但这是本课程系统调用自定义的约定,不是 riscv 官方规范强制) |
| S-mode 访问用户页的额外开关 | CR4.SMAP（本课程未启用,不受影响） | `sstatus.SUM`（复位值 0,必须显式置 1,本 Lab 真实 bug 之一） |
| 用户态是否可被中断打断 | RFLAGS.IF（本 Lab `enter_user_mode` 手动置 1） | `sstatus.SPIE`（本 Lab `enter_user_mode` 手动置 1） |
| 用户/内核地址空间关系 | 共用同一份页表（同一个 CR3），本 Lab 刻意简化 | 共用同一份页表（同一个 `satp`），本 Lab 刻意简化 |

两边在"用户程序打印一行字然后退出"这个可观察行为上完全对等，但硬件机制几乎没有相似之处——这正是本 Lab 想传达的教学要点：系统调用需要解决的核心问题（受控提权、参数传递、安全返回）是通用的，但落到具体架构上，从"用什么指令触发"到"入口怎么配置"再到"怎么安全地共享/不共享栈"，几乎是从零开始的两套逻辑。

## 代码目录与关键文件

```
labs/lab06-syscall-user/
├── Makefile                     # 构建入口，两边都新增用户程序的独立构建产物
├── README.md                    # 本文件
├── solution/
│   ├── x86_64/
│   │   ├── boot.S                # 和 Lab5 完全一样
│   │   ├── linker.ld             # 和 Lab5 完全一样
│   │   ├── kernel_main.c         # 新增：分配/映射用户程序页+用户栈页，跳进用户态
│   │   ├── trap.c                # 在 Lab5 基础上新增：GDT（6 项）+ sys_write/sys_exit + syscall_dispatch + syscall_init（MSR 编程）
│   │   ├── trap_entry.S          # 在 Lab5 基础上新增：syscall_entry（独立入口）+ enter_user_mode
│   │   ├── user_prog.S           # 新增：嵌入式用户程序源码（SYSCALL 两次：write, exit）
│   │   ├── user_prog.ld          # 新增：用户程序独立链接脚本，固定加载地址
│   │   ├── user_blob.S           # 新增：把编译好的用户程序二进制用 .incbin 嵌进内核镜像
│   │   ├── syscall.h             # 新增：SYS_WRITE/SYS_EXIT 调用号定义（内核和用户程序共享）
│   │   ├── console_putc.c        # 和 Lab5 完全一样
│   │   ├── panic_arch.c          # 和 Lab5 完全一样
│   │   ├── memmap.c              # 和 Lab5 完全一样
│   │   ├── pagetable.c           # 和 Lab5 完全一样
│   │   └── grub.cfg              # 和 Lab5 完全一样
│   └── riscv64/
│       ├── boot.S                # 和 Lab5 完全一样
│       ├── linker.ld             # 和 Lab5 完全一样
│       ├── kernel_main.c         # 新增：分配/映射用户程序页+用户栈页，跳进用户态
│       ├── trap.c                # 在 Lab5 基础上新增：read_sepc/write_sepc + sys_write/sys_exit + syscall_dispatch + ecall_handler + supervisor_trap_handler 里的 ecall 分支
│       ├── trap_entry.S          # 在 Lab5 基础上新增：supervisor_trap_entry 的 U-mode 来源分支 + enter_user_mode
│       ├── user_prog.S           # 新增：嵌入式用户程序源码（ecall 两次：write, exit）
│       ├── user_prog.ld          # 新增：用户程序独立链接脚本，固定加载地址
│       ├── user_blob.S           # 新增：把编译好的用户程序二进制用 .incbin 嵌进内核镜像
│       ├── syscall.h             # 新增：SYS_WRITE/SYS_EXIT 调用号定义（内核和用户程序共享）
│       ├── sbi.c/sbi.h            # 和 Lab5 完全一样
│       ├── console_putc.c        # 和 Lab5 完全一样
│       ├── panic_arch.c          # 和 Lab5 完全一样
│       ├── memmap.c              # 和 Lab5 完全一样
│       └── pagetable.c           # 和 Lab5 完全一样
├── starter/                      # 结构和 solution 一一对应，教学文件带 TODO
│   ├── x86_64/
│   └── riscv64/
└── tests/
    ├── expect-x86_64.txt
    └── expect-riscv64.txt
```

`syscall.h`/`user_prog.ld`/`user_blob.S` 三个文件虽然是本 Lab 新增，但 starter 版本里也是直接带过来、不做 TODO：`syscall.h` 只是两个数字常量，没有推导逻辑；`user_prog.ld` 的加载地址是固定选定的，不需要学生设计；`user_blob.S` 只是一个机械的 `.incbin` 包装，没有教学点——这三者的"新增"体现在*内容*上是本 Lab 才第一次出现，不体现在*需要学生填空*上。

`kalloc.c`/`kalloc.h`（`src/common/`）本 Lab 新增了 `kalloc_set_phys_to_virt_offset()` 这个函数（详见"常见坑与排查"）——这是本 Lab 唯一一处需要修改 Lab5 之前从未变过的公共基础设施代码，不属于任何架构专属目录，两边 `kernel_main.c` 都需要调用它。

## 分步实现步骤

### x86_64 路线

1. **trap.c：GDT（Global Descriptor Table）扩展到 6 项。** Lab5 之前从未真正用到分段机制（`mcmodel=kernel` 编译，段基址全部是 0，分段形同虚设），但 SYSCALL/SYSRET 硬性要求 GDT 里有合法的段选择子可用，且四个关键选择子（内核 CS、内核 SS、用户 CS32、用户 CS64+SS）在数组里的**相对位置**必须符合 `STAR` MSR 的编码约定：`SYSCALL` 用 `STAR[47:32]` 算出内核 CS（这个值本身）和内核 SS（这个值+8）；`SYSRET`（64位模式）用 `STAR[63:48]` 算出用户 CS32（这个值+16，一个从不会真正使用、纯粹占位的 32 位兼容段）和用户 SS（这个值+8）以及用户 CS64（这个值+16）——**必须在用户真正的 64 位代码段之前预留一个从不使用的 32 位段占位**，这不是随意的选择，是 `SYSRET` 硬件编码规则本身要求的相对偏移。GDT 数组需要：null 描述符、内核 CS（64 位，DPL=0）、内核 SS（DPL=0）、用户 CS32（占位，不会真正跳进 32 位模式）、用户 SS（DPL=3）、用户 CS64（DPL=3）,共 6 项。
2. **trap.c：`gdt_init()`。** 填好 6 项描述符的 access byte/flags（内核段 DPL=0，用户段 DPL=3——DPL 是 CPU 判断当前特权级、`SYSRET`/`iretq` 切换后 CPL 由谁决定的关键字段），构造 GDT pointer，执行 `lgdt`。
3. **trap.c：`sys_write(const char *buf, size_t len)`/`sys_exit(int code)`。** `sys_write` 直接把用户传进来的指针交给 `console_puts`/逐字符 `console_putc`——本 Lab 教学取向不做更严格的地址空间归属校验，只依赖这个指针指向的页确实被映射且带 U 位（映射本身在 `kernel_main.c` 完成）。`sys_exit` 打印退出码，不需要真的终止/回收任何资源（本 Lab 没有进程概念）。
4. **trap.c：`syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3)`。** 按 `syscall_num`（对应 RAX）分发到 `sys_write`/`sys_exit`，参数对应 RDI/RSI/RDX。
5. **trap.c：`syscall_init()`。** 编程三个 MSR：`STAR`（按步骤 1 的编码规则填四个选择子）、`LSTAR`（写入 `syscall_entry` 的地址）、`SFMASK`（SYSCALL 执行时哪些 RFLAGS 位要被硬件自动清零——至少要清 IF，避免 SYSCALL 刚进内核态、栈还没切换好的这一小段窗口被中断打断）,再置位 `EFER.SCE`（System Call Extensions，**这是一个独立于 `EFER.LME` 的位，只影响 SYSCALL/SYSRET 是否可用，不影响长模式本身**——本课程从 Lab1 就已经置位过 `EFER.LME`，这里第一次需要碰 `EFER` 的另一个位）。
6. **trap_entry.S：`syscall_entry`。** SYSCALL 硬件不会自动切栈（跟 IDT 路径的 `iretq`/中断门不同），进入时 RSP 仍然是用户栈——第一步必须先把用户 RSP 存进一个全局变量（`syscall_saved_user_rsp`，不能压栈，这一刻还没有可信的内核栈），再切到 `__stack_top`（复用 Lab1 就有的同一个内核栈）。硬件已经自动把返回地址存进 RCX、原 RFLAGS 存进 R11——这两个寄存器在调用 `syscall_dispatch` 前后必须原样保留（`syscall_dispatch` 是普通 C 函数，可能踩这两个寄存器），需要显式保存/恢复。传参时要注意**参数搬运顺序**：RAX（调用号）先搬到 RDI，再把原 RDI/RSI/RDX 依次搬到 RSI/RDX/RCX——**必须从后往前搬（或者用不会自我覆盖的顺序），否则会覆盖还没搬走的源寄存器**。返回时把 `sys_write`/`sys_exit` 走完之后 RAX 里的返回值原样传回用户态（本 Lab 目前的调用约定不强制要求返回值有意义，但按标准约定填好），恢复 RCX/R11，换回用户 RSP，执行 `sysretq`。
7. **trap_entry.S：`enter_user_mode(uintptr_t entry, uintptr_t stack)`。** 第一次跳进用户态**不能用 `sysretq`**（理由见"核心概念"），必须手工在栈上按 `iretq` 期望的格式构造五个值：SS（用户栈段选择子）、RSP（用户栈顶）、RFLAGS（`0x202`，IF=1 允许被中断打断，bit 1 是 x86 架构规定必须恒为 1 的保留位）、CS（用户代码段选择子，注意 CS 的选择子值里低 2 位就是 DPL=3，`iretq` 靠这两位决定切换后的 CPL）、RIP（用户程序入口）——**这五个值入栈顺序和 `iretq` 弹出顺序相反**，先压 SS，最后压 RIP，`iretq` 执行时按"RIP、CS、RFLAGS、RSP、SS"的顺序弹出。
8. **kernel_main.c：** 在 Lab5"页表+中断+定时器都齐备"之后，依次：`pagetable_activate()` → `kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE)`（**必须在这一步之后、下面第一次 `kalloc_page()` 之前完成**，理由见"常见坑与排查"）→ `gdt_init()` → `idt_init()`/`syscall_init()`/`pit_init()`/`sti` → 分配一页物理内存，`memcpy` 嵌入的用户程序字节进去，`pagetable_map` 到 `USER_PROG_VADDR`（带 `PTE_FLAG_USER`）→ 分配另一页给用户栈,清零,`pagetable_map` 到 `USER_STACK_VADDR - PAGE_SIZE`（带 `PTE_FLAG_USER`,注意 `USER_STACK_VADDR` 本身定义成栈顶,不是栈页起始地址,这两者差一个 `PAGE_SIZE`,是本 Lab 的第一个真实 bug,见"常见坑与排查"）→ `enter_user_mode(USER_PROG_VADDR, USER_STACK_VADDR)`。
9. **user_prog.S：** `_user_start` 依次执行两次 SYSCALL——RAX=SYS_WRITE, RDI=消息地址, RSI=消息长度,`syscall`;RAX=SYS_EXIT, RDI=退出码,`syscall`。`sys_exit` 返回后用户程序落进自己的死循环（`hlt` 在用户态是合法指令,不会像 riscv64 的 `wfi` 那样触发 illegal instruction,详见对照表riscv64 那一侧的坑）。

### riscv64 路线

1. **trap.c：`read_sepc()`/`write_sepc()`。** riscv64 处理 ecall **不像** x86_64 的 RCX 会被硬件自动设置成"下一条指令"——`sepc` 硬件写入的是 `ecall` **这条指令自己**的地址,软件必须在返回前手动 `write_sepc(read_sepc() + 4)` 跳过它（riscv64 指令定长 4 字节,不需要像 x86 那样处理变长指令的麻烦）,否则 `sret` 之后会在用户态无限重复执行同一条 `ecall`。
2. **trap.c：`sys_write`/`sys_exit`。** 跟 x86_64 版本逻辑一致,但读用户指针这一步**必须先确保 `sstatus.SUM` 已经置 1**（见步骤 5/6 的 `enter_user_mode`),否则会在 PTE 完全正确的情况下仍然触发 page fault,这是本 Lab riscv64 侧的真实 bug,见"常见坑与排查"。
3. **trap.c：`syscall_dispatch`。** 按调用号（a7)分发,参数对应 a0/a1——**这是本课程系统调用自定义的约定**,不是 riscv SBI 规范强制要求的,只是借用了同一个思路方便对照。
4. **trap.c：`ecall_handler(uint64_t *frame)`。** 从 `frame[15]`（对应 `trap_entry.S` 里 a7 存入栈帧的偏移量,字节偏移÷8=15)读调用号,`frame[8]`/`frame[9]`（a0/a1)读参数,调用 `syscall_dispatch`,把返回值写回 `frame[8]`（跟 x86_64 把返回值放回 RAX 是同一个思路,只是 riscv64 这里靠约定的栈帧下标,不是寄存器名)。**这些下标必须和 `trap_entry.S` 里 `sd a7, 120(sp)` 这类保存顺序手工保持同步**,这是本 Lab riscv64 侧唯一一处"没有单一数据源、需要人肉对齐"的地方,和 x86_64 侧 `USER_PROG_VADDR` 需要跟 `user_prog.ld` 手工同步是同一类简化。
5. **trap.c：`supervisor_trap_handler()` 新增 ecall 分支。** 在原有中断判断（Lab5）、page fault 判断（Lab4）之外,插入对 `scause == SCAUSE_ECALL_FROM_U`（值为 8)的判断——命中时调用 `write_sepc(read_sepc() + 4)`（步骤 1)再调用 `ecall_handler(frame)`。
6. **trap_entry.S：`supervisor_trap_entry` 新增 U-mode 来源分支。** 在触碰任何栈内容*之前*,先 `csrr` 读 `sstatus.SPP`（bit 8)——SPP=0（来自 U-mode,本 Lab 新增路径)时,当前 sp 是用户栈指针,必须先存进全局变量（`trap_saved_user_sp`,不能压栈,这一刻还没有可信的栈可以压)再换上内核栈（`__stack_top`);SPP=1（来自 S-mode,Lab4/5 原有路径)时不做任何切换,直接沿用现有 sp。**两条分支之后必须汇合到同一段"保存 16 个寄存器→call→恢复 16 个寄存器"的代码**,保证 S-mode 来源时的指令序列跟 Lab5 完全一致。出口时按 t1（入口时保存的 SPP 判断结果,本身也要跟着 t0-t6 一起存进栈帧、call 后从栈帧恢复,因为 t1 是 caller-saved,C 函数可能踩它)决定要不要把 sp 换回 `trap_saved_user_sp`。
7. **trap_entry.S：`enter_user_mode(entry, stack)`。** 写 `sepc`=entry；读-改-写 `sstatus`：SPP（bit 8）清零（`sret` 靠这一位决定回到哪个特权级）、SPIE（bit 5）置 1（`sret` 时会把这一位的值写进 `sstatus.SIE`,决定回到用户态之后全局中断使能状态,不是 SPP 决定的）、**SUM（bit 18）置 1**（"permit Supervisor User Memory access"，riscv-privileged 规范定义,复位值 0,是 x86_64 SMAP 的等价机制但复位状态相反——不置 1 会导致 S-mode 完全无法解引用带 U 位的页,即便 PTE 本身完全正确,这是本 Lab riscv64 侧最容易漏掉的一步,见"常见坑与排查"）；再 `mv sp, a1`（`sret` 不会自动设置任何通用寄存器,包括 sp,这跟 x86_64 `iretq` 会自动弹出 RSP 不同,必须手动设置）；最后 `sret`。
8. **kernel_main.c：** 跟 x86_64 版本步骤一致,只是把 `gdt_init()`/`syscall_init()`/`sti` 换成 riscv64 侧已有的 `trap_init()`/`timer_enable()`/`sbi_set_timer()`（Lab5 遗留),`kalloc_set_phys_to_virt_offset(KERNEL_VIRT_BASE)` 同样必须在 `pagetable_activate()` 之后、第一次 `kalloc_page()` 之前完成。
9. **user_prog.S：** `_user_start` 依次执行两次 ecall——a7=SYS_WRITE, a0=消息地址, a1=消息长度,`ecall`；a7=SYS_EXIT, a0=退出码,`ecall`。**消息长度不能写成 `li a1, msg_len`（`msg_len = . - msg` 这种链接期表达式)**,riscv GNU 汇编器的 `li` 伪指令要求操作数是*汇编期*可求值的常量,遇到链接期才能确定的表达式会报 `illegal operands`——必须换成 `la a0, msg; la a1, msg_end; sub a1, a1, a0` 这种运行期计算长度的写法,这是本 Lab riscv64 侧一个编译期就会报错、但报错信息不直接指向根因的坑。`sys_exit` 返回后用户程序落进自己的死循环,**不能用 `wfi`**（U-mode 执行 `wfi` 会因为 `mstatus.TW`（Timeout Wait,复位值 0,本课程从未主动设置,但 QEMU 对 rv64gc 的默认实现里这一位造成的效果等同于禁止 U-mode 直接执行 `wfi`)触发 illegal instruction,详见"常见坑与排查"）,必须换成 `j 1b` 忙等。

## QEMU 运行命令

```bash
cd labs/lab06-syscall-user
make ARCH=x86_64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=x86_64 LAB=lab06-syscall-user VARIANT=solution TIMEOUT=8
```

x86_64 预期输出（本 README 撰写时在 QEMU 里实测确认）：

```
Hello OS from x86_64 (Lab6: syscall & user mode)
memmap: 2 available region(s) from Multiboot2 mmap tag, 32467 page(s) free
kernel image: phys [0x100000, 0x10d000)
switched to page table, low identity map gone
timer armed, syscall entry armed, building user program
user program mapped, entering user mode
hello from user mode (x86_64, SYSCALL/SYSRET)
user program exited with code 0
```

```bash
make ARCH=riscv64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab06-syscall-user VARIANT=solution TIMEOUT=8
```

riscv64 预期输出（本 README 撰写时在 QEMU 里实测确认，OpenSBI 启动横幅省略）：

```
memmap: 1 region(s) from DTB /memory, 32241 page(s) free (kernel image excluded)
Hello OS from riscv64 (Lab6: syscall & user mode)
kernel image: phys [0x80200000, 0x8020f000)
switched to page table, low identity map gone
timer armed, ecall entry armed, building user program
user program mapped, entering user mode
hello from user mode (riscv64, ecall/sret)
user program exited with code 0
```

两边的用户程序在打印一次 `hello from user mode`、退出一次之后都会落进各自的死循环（内核主循环理论上还会继续 `hlt`/`wfi` 等下一次定时器中断，但本 Lab 没有调度器，用户态一旦拿到 CPU 就不会再交还，`run-qemu.sh` 靠 `TIMEOUT` 强制终止是设计上的正常行为，`terminating on signal 15` 那一行不代表出错）。

## GDB/QEMU Monitor 调试方法

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab06-syscall-user VARIANT=solution
```

```gdb
target remote :1234
file solution/x86_64/build/kernel.elf
break syscall_entry
commands
  print/x $rax
  print/x $rdi
  print/x $rsi
  continue
end
break enter_user_mode
continue
```

排查"用户态到底有没有真的跳过去/系统调用参数有没有传对"最有用的做法：在 `syscall_entry`/`ecall_handler` 入口打断点，核对 RAX/a7（调用号）和 RDI-RDX/a0-a1（参数）跟用户程序源码里字面写的值是否一致；在 `enter_user_mode` 打断点，`stepi` 单步走到 `iretq`/`sret` 那一条，确认执行完之后 `info registers` 里 CS/`$satp` 反映的当前特权级确实降到了 3/U-mode。

riscv64 一侧排查 page fault 相关的坑（SUM 位、页表映射覆盖），QEMU 自带的 MMU 调试日志比 GDB 更直接：

```bash
qemu-system-riscv64 ... -d guest_errors -D guest.log
```

一旦看到 `Illegal access` 或者反复出现的 `sret`/`ecall` 循环但地址范围不对，基本可以确定是 `sstatus.SUM` 没设或者页表映射被覆盖，不需要先怀疑系统调用分发逻辑本身写错了。

## 自动验收测试

```bash
cd labs/lab06-syscall-user
bash ../../scripts/test-lab.sh ARCH=x86_64 LAB=lab06-syscall-user
bash ../../scripts/test-lab.sh ARCH=riscv64 LAB=lab06-syscall-user
```

`tests/expect-*.txt` 用逐行子串匹配（不要求整行完全一致，也不要求逐行相邻，只要求按顺序作为子串出现），所以 starter 版本实现完成后即使空闲页数、内核镜像结束地址这些和 solution 的具体数值不完全一样，也不影响测试通过——只要关键行的固定文本部分一致、顺序一致。

## 常见坑与排查

以下五个都是本 Lab 开发过程中**实测踩到、定位、修复过的真实 bug，不是猜测或者理论上可能发生的情况**（再往下还有一个刻意*不*修的已知缺陷，单独一节）：

- **x86_64：`USER_STACK_VADDR` 定义成"栈页起始地址+一页"而不是"栈页顶端地址"，导致两次 `pagetable_map` 覆盖同一个页表项。** 用户栈页紧跟在用户程序页正上方（`USER_PROG_VADDR + PAGE_SIZE` 开始），栈顶应该是这段范围*再加一页*（`USER_PROG_VADDR + 0x2000`），映射时用"栈顶 - PAGE_SIZE"当栈页的起始虚拟地址。之前的实现直接把 `USER_STACK_VADDR` 写成 `USER_PROG_VADDR + 0x1000`，映射时用 `USER_STACK_VADDR - PAGE_SIZE` 算出来正好等于 `USER_PROG_VADDR` 本身——第二次 `pagetable_map` 调用把第一次（用户程序页）的页表项直接覆盖掉，两个不同的物理页被映射到了*同一个*虚拟地址上。实测症状：用户程序跳过去执行入口点所在的那一页，CPU 实际取到的物理内容其实是清零后从未写过东西的栈页（内容全零），尝试从全零字节译码取指直接触发 #PF。排查方法：GDB 在 `enter_user_mode` 之后立刻 `x/10i $entry_addr`（或者用 `pagetable_lookup` 打印两次映射各自算出的虚拟地址是否真的不同），核对预期的入口地址处是否真的能读到用户程序字节，而不是一段全零内容。
- **riscv64：`enter_user_mode` 没有置位 `sstatus.SUM`，导致内核态解引用用户指针触发 page fault，即便 PTE 本身完全正确。** `sys_write` 需要从 S-mode 直接读取用户程序传进来的消息指针（`a0`，用户虚拟地址），这个地址对应的 PTE 上 U 位、R 位都已经被 `kernel_main.c` 正确设置——但 riscv-privileged 规范额外规定了一层独立的检查：S-mode 默认不允许访问任何带 U 位的页，无论页表项本身权限位是什么，这层检查由 `sstatus.SUM`（"permit Supervisor User Memory access"）控制，复位值是 0（禁止），且 QEMU/OpenSBI 都不会替内核代劳设置。忘记这一位的实测症状：`sys_write` 内部对 `a0` 解引用直接触发 load page fault（`scause`=13，`stval`=用户消息的虚拟地址），故障发生在一个"页表项完全正确"的地址上,容易误判成映射本身出了问题,但页表检查和 SUM 检查是两层独立的门,页表那层过了不代表 SUM 那层也过了。排查方法：GDB 在触发 fault 之前 `print/x $sstatus`，核对 bit 18 是否真的是 1；或者直接在 `enter_user_mode` 里逐条单步验证 `sstatus` 读-改-写之后的最终值。
- **riscv64：`user_prog.S` 里用 `li a1, msg_len`（`msg_len = . - msg`）计算消息长度，riscv GNU 汇编器报 `illegal operands`。** `li` 伪指令展开成具体的 `lui`/`addi` 组合时，要求操作数是*汇编期*就能求出确定数值的常量表达式；`. - msg` 是"当前地址减去某个标号"，这是一个只有*链接期*（所有 section 布局都确定之后）才能求值的表达式，`li` 遇到这种表达式直接在汇编阶段报错，不会拖到链接期才暴露。这跟同样的"符号减法"在 `.dword`/`.quad` 这类数据指令里是完全合法的用法（能退化成一条链接期重定位）形成对比——`li` 展开出来的是若干条独立指令，指令操作数不是"一个字段等着重定位填值"，而是要在汇编阶段就编码进指令编码本身，两者对"是否允许链接期才确定的值"的支持粒度不同。修复方式：换成 `la a0, msg; la a1, msg_end; sub a1, a1, a0`，把长度计算挪到*运行期*用两个标号的地址相减得到，不依赖汇编器在汇编阶段就求出这个值。
- **riscv64：用户程序退出后的死循环写成 `wfi`，U-mode 执行时触发 illegal instruction。** `wfi` 语义上是"让 hart 进入低功耗等待状态直到下一次中断"，riscv-privileged 规范允许它在 U-mode 执行（不像大多数特权指令那样直接禁止），但**是否真的允许，还要看 `mstatus.TW`（Timeout Wait）这一位**——`mstatus.TW=1` 时，任何非 M-mode 执行 `wfi` 都会被当成非法指令处理（这是规范特意设计的一个开关，给 hypervisor/固件一个"不希望被打扰"的选项）。本课程从未主动设置过 `mstatus.TW`，但 QEMU 对 `rv64gc` 的默认实现在这个场景下造成的效果等同于禁止 U-mode 直接执行 `wfi`——实测在用户程序执行到这一条时立刻触发 illegal instruction 异常（`scause`=2），此时 `sepc` 指向的正是那一条 `wfi`。排查方法：GDB 断点在 illegal instruction 触发时 `x/1i $sepc`，确认落在 `wfi` 那一行；或者直接查 riscv-privileged 规范"Wait for Interrupt"一节里关于 `mstatus.TW` 的行为描述。修复方式：换成一条忙等分支 `j 1b`（跳回自身标号，纯粹空转，不依赖任何特权指令），教学取向不为这一行额外去配置 `mstatus.TW`。
- **（基础设施，非架构专属）`kalloc.c` 从 Lab3 建立以来一直假设"物理地址本身就能直接当指针解引用"，Lab6 是第一个在页表切换*之后*还调用 `kalloc_page()` 的 Lab，第一次触发这个假设不成立的场景。** 空闲页链表的元数据（下一个 run 的地址、这个 run 有多少页）直接存在空闲物理页自己的头几个字节里，`kalloc_add_region`/`kalloc_pages`/`kfree_pages` 三处都需要把"物理地址"强转成指针去读写这几个字节——这在 Lab3-5 里从未出问题，因为它们全部的 `kalloc_page()` 调用都发生在 `pagetable_activate()` *之前*（低地址身份映射还在，物理地址凑巧也是合法的可解引用地址）。本 Lab 第一次需要在切换到只有高半自映射的正式页表*之后*，现场分配用户程序页和用户栈页——这时候物理地址不再是合法的可解引用虚拟地址，实测触发：`kalloc_pages()` 内部读 `run->num_pages` 直接 page fault，故障地址（CR2/`stval`）恰好落在 kalloc 空闲池头部那个 run 的物理地址上，不是随便什么地址。修复方式是往 `kalloc.h`/`kalloc.c` 新增 `kalloc_set_phys_to_virt_offset(uint64_t offset)`，调用方在 `pagetable_activate()` 之后、第一次 `kalloc_page()`/`kalloc_pages()` 之前补一次调用（两边 `kernel_main.c` 都传 `KERNEL_VIRT_BASE`）——之后 `kalloc.c` 内部所有对链表节点的解引用都会自动加上这个偏移量，`kalloc_page()`/`kalloc_pages()` 对外的返回值约定不变（仍然是物理地址），链表里存储的地址值本身（用于排序/邻居合并判断）也不变，只有"要不要加偏移量才能解引用"这一层内部逻辑发生了变化。这是本课程基础设施代码第一次因为后续 Lab 的新场景而需要回头修改，不是本 Lab 架构专属目录里的问题。

## 已知缺陷：x86_64 本 Lab 结束时其实已经三重故障了，而串口日志看不出来

上面五个 bug 都修好了。这一节讲的是一个**实测确认存在、但本 Lab 刻意不修**的缺陷——它同时也是本课程到目前为止关于"怎么才算验证过"最重要的一课，比缺陷本身更值得记住。

**现象。** 用"QEMU 运行命令"一节给的命令跑 x86_64 solution，串口输出干净地停在 `user program exited with code 0`，然后进程一直挂着直到 `TIMEOUT` 把它杀掉（退出码 124）——这正是本课程一直当作"通过"的形态，反复跑多少次都一样。但给**同一个二进制**加上 `-d int -D /tmp/log.txt` 再看，日志里每次都确定性地出现这样一串（下面是实测日志原文，不是示意）：

```
0: v=20 e=0000 i=0 cpl=3 IP=002b:0000000000400027 pc=0000000000400027 SP=0023:0000000000402000
1: v=0e e=0000 i=0 cpl=3 IP=002b:0000000000400027 pc=0000000000400027 CR2=0000000000000004
2: v=08 e=0000 i=0 cpl=3 IP=002b:0000000000400027 pc=0000000000400027
check_exception old: 0x8 new 0xd
```

读法：用户程序 `sys_exit` 之后在 ring3 的 `jmp 1b` 死循环里空转（`cpl=3`，`IP=0x400027`，可以用 `objdump -d user_prog.elf` 核对这就是那条死循环），第一次 PIT 时钟落进来 → `v=20`（时钟，IDT 查表分发本身是正常的）→ 紧接着 `v=0e`（#PF，`CR2=0x4`）→ `v=08`（#DF）→ 最后 `check_exception old: 0x8 new 0xd`（处理 #DF 期间又来一个 #GP）就是**三重故障**。这一行是整个日志的最后一行：内核在本 Lab"看起来正常结束"之后，其实已经死了。

**为什么串口看不出来。** 三重故障之后 CPU 不会再执行内核的任何一条指令，而串口上最后一行 `user program exited with code 0` 是三重故障*之前*就打完的——之后无论内核是活着空转还是已经死了，串口都不会再有任何字节。于是"内核三重故障、彻底停住"和"内核活着、在 `hlt` 里等中断"这两件事**产生逐字节相同的串口输出**，无法区分。这个 bug 之所以在本 Lab 自己收尾时没被抓到，就是因为当时的验证方法只检查串口输出的形状，而这个失效模式恰好在串口上不可观测。

**`-no-reboot` 这个参数会把证据也一起藏起来，这点尤其反直觉。** 本课程的运行命令一直带 `-no-reboot`（见"QEMU 运行命令"一节）。三重故障在 QEMU 里是当作一次"复位请求"处理的，而 `-no-reboot` 的语义是"收到复位请求就*停机*，不要真的重启"——于是 QEMU 静静地把 CPU 停在那里，`-d cpu_reset` 日志里**一条复位记录都不会多**（实测就是启动阶段固有的那 2 次，和一个完全健康的内核跑出来的日志一模一样）。换句话说：带着 `-no-reboot` 用 `-d cpu_reset` 去查这个 bug，会得到"干净"的假阴性结论。把 `-no-reboot` 去掉再跑同一个二进制，真相立刻显形——实测 15 秒内 `CPU Reset` 出现 **72 次**、启动横幅 `Hello OS from x86_64 ...` 重复打印 **35 次**，是一个彻底的重启循环（每次重启都重新跑一遍内核、重新三重故障）。作为对照，Lab7 装上 TSS 之后同样去掉 `-no-reboot` 跑，`CPU Reset` 就是 2 次、横幅 1 次。

**根因。** x86_64 从 Lab1 到 Lab6 **从来没有加载过 TSS**（`grep -rn "ltr\|TSS"` 翻遍前面所有 Lab 是零命中，已确认）——TR 里是 GRUB/复位遗留下来的值，实际上不是一个有效的任务段。IDT 的中断门在 CPL=3 时触发，需要硬件从 TSS 里取 RSP0 来切到内核栈、往上压中断栈帧；TSS 无效意味着没有合法的 RSP0，栈帧被写到了 0 附近的地址（这就是 CR2=0x4 的来源），于是 #PF → #DF → 三重故障级联。注意这不是"本 Lab 漏了一行配置"那种疏忽：在本 Lab 之前，CPL 一直是 0，中断门从不需要切栈，TSS 不存在也完全正常；**本 Lab 第一次让 CPL=3 存在了非零长度的时间窗口，这个缺口才第一次可被触发**。

**为什么不在本 Lab 修。** 修它需要引入 64 位 TSS（`struct tss64`、`tss_init()`、把 GDT 从 6 项扩到 8 项、`ltr`），而"每个进程一个内核栈、进程切换时更新 RSP0"是 [Lab7](../lab07-process-scheduler/README.md) 的核心内容之一——提前搬到本 Lab 会让本 Lab 的主题（特权级切换本身）被稀释。本 Lab 的取向是：用户程序 `sys_exit` 之后就不再指望内核做任何有意义的事，所以这个缺陷不影响本 Lab 要教的东西。Lab7 装上 TSS 之后，用下面"要带走的方法论"提到的那两种*有效*查法在双架构上各查一遍，都是干净的——同一个时钟中断照样在 `cpl=3` 落下来（`-d int` 日志里能看到 `v=20`），但这次后面什么都没有，因为 TSS 里有合法的 RSP0 了。

**riscv64 侧没有对应的坑**，因为 riscv 的 trap 入口切栈是*软件*做的——`trap_entry.S` 自己读 `sstatus.SPP` 判断来源特权级再决定要不要换 sp，没有"硬件从某张表里取内核栈指针"这一层，所以不存在一张必须提前配好的表。这是本课程两条架构路线又一个结构性差异，和"SYSCALL 绕过 IDT vs `ecall` 共享 `stvec`"属于同一类。

**要带走的方法论。** 任何 Lab 只要设计上允许中断在 CPL=3 / U-mode 期间落下来，就不能拿"串口输出干净"当作"没崩"的证据。这套判据（两种实测有效的查法、"正常启动本来就有 2 次 `CPU Reset`"这条基线、以及 `-d int` 日志里那些**不是**内核异常的红鲱鱼）已经整理成一份独立文档，供本课程所有 Lab 引用：[`docs/verification-methodology.md`](../../docs/verification-methodology.md)。那份文档里还有一张本 Lab 与 Lab7 双架构的实测对照表——**本 Lab 这个 x86_64 二进制现在是本课程唯一的"已知坏"靶子**，任何新的"证明没崩"的方法，先拿它试，报不出来就说明方法不对。

## 挑战任务

- 亲手观察上面这个"看不见的三重故障"，按顺序做三次、每次只改一个变量，体会"验证方法本身也可能有盲区"：(a) 用本 Lab 原本的命令加 `-d cpu_reset` 跑 x86_64 solution，数 `CPU Reset`——会得到 2 次，看起来完全健康；(b) 把 `-no-reboot` 去掉重跑，数 `CPU Reset` 和启动横幅重复次数——会得到几十次，同一个二进制、同一份代码，结论完全反了；(c) 加回 `-no-reboot` 但换成 `-d int`，grep `check_exception old: 0x8`，看故障级联的每一步。然后对 riscv64 solution 重复 (b)，它应该老老实实是 2 次——本 Lab 这个坑是 x86_64 独有的。
- 给 `sys_write` 加一层最基础的地址校验（比如"指针落在 `USER_PROG_VADDR` 到某个上限之间"），故意从用户程序传一个指向内核自己数据的指针进去，观察校验加上前后的行为差异，体会为什么真实内核需要更完整的地址空间归属检查（而不是本 Lab 这种"完全不校验，信任 PTE 权限位就够了"的简化）。
- 再实现一个 `sys_getpid`（哪怕现在还没有进程概念，可以先固定返回 0），体会"加一个新系统调用"需要同步改动的所有位置——调用号定义（`syscall.h`）、内核侧分发（`syscall_dispatch`）、用户侧调用约定（`user_prog.S`），这三处目前完全靠人肉保持同步，思考真实内核会怎么用工具（比如 Linux 的 syscall table 生成脚本）自动化这一步。
- x86_64 侧尝试去掉 `enter_user_mode` 里手工构造的 `iretq` 帧，改成"先真的从用户态发起一次哑元 SYSCALL 把 RCX/R11 填对，再用 SYSRET 回去"这种迂回路径，体会为什么本 Lab 选择直接用 `iretq`（提示：这条路径要求用户态代码在还没被内核信任建立好之前就先执行一段代码，思考这在启动顺序上是否可行）。
- riscv64 侧阅读 RISC-V Privileged Architecture 手册里关于 `mstatus.TW`/`mstatus.TVM`/`mstatus.MXR` 这几个和"S-mode 对下层特权级施加限制"相关的位，对照本 Lab 已经碰到的 `SUM`，梳理还有哪些位是本课程尚未触发但未来 Lab 可能会踩到的坑。

## 参考

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 2B，`SYSCALL`/`SYSRET` 指令参考页（编码规则、`STAR`/`LSTAR`/`SFMASK` MSR 布局的权威定义）
- Intel SDM, Volume 3A, Chapter 5 "Protection"（段选择子 DPL/CPL 与特权级切换的基础机制，GDT 排列约束的来源）
- The RISC-V Instruction Set Manual, Volume II: Privileged Architecture，"Supervisor-mode Environment Call and Breakpoint"（`ecall`/`sret` 行为定义）、"sstatus"一节（SPP/SPIE/SUM 位的权威定义）、"Wait for Interrupt"一节（`wfi`/`mstatus.TW` 的权威定义）
- GNU Binutils 文档，`as` 手册"Expressions"一节（汇编期常量表达式与链接期表达式的区分，本 Lab riscv64 第三个真实 bug 的依据）
- OSDev Wiki，"SYSCALL"页面（社区整理的 SYSCALL/SYSRET 常见坑，本课程从零推导后确认与其一致）
- [`docs/verification-methodology.md`](../../docs/verification-methodology.md)（本 Lab"已知缺陷"一节引出的通用判据：怎么证明内核真的没崩）

## 下一步

进入 [Lab7：进程与调度](../lab07-process-scheduler/README.md)——本 Lab 只做到"一个用户程序跳进去、退出、卡在死循环"，Lab7 会在本 Lab 搭好的用户态基础设施上引入"多个执行流"和"定时器中断时把 CPU 从用户态抢回来"这两个此刻还刻意回避的机制，让内核主循环第一次能在用户程序运行期间真正拿回控制权。
