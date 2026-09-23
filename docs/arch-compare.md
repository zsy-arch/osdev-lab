# x86_64 与 riscv64 架构对照

这是全课程的架构差异索引。每个 Lab 文档里会有一份"本 Lab 专属"的精简对照表；这份文档是完整版，覆盖全部 11 个 Lab 涉及的差异点，供随时查阅。

## 总表

| 主题 | x86_64 | riscv64 | 涉及 Lab |
|---|---|---|---|
| 固件/引导 | BIOS 或 UEFI → GRUB/Limine → Multiboot2 协议交给内核 | OpenSBI（M 模式固件）→ 直接跳到内核入口，走 SBI 调用约定 | Lab1 |
| 启动时特权级 | 实模式 → 保护模式（Protected Mode）→ 长模式（Long Mode），Bootloader 已经切到 32 位保护模式，内核自己切 64 位长模式 | M 模式 → S 模式（OpenSBI 完成大部分初始化后把控制权交给 S 模式内核） | Lab1 |
| 特权级模型 | Ring 0（内核）～ Ring 3（用户），共 4 级，本课程只用 Ring 0 和 Ring 3 | M（Machine）/ S（Supervisor）/ U（User）3 级，本课程内核跑在 S，用户程序跑在 U，M 交给 OpenSBI | Lab1, Lab6 |
| 页表结构 | 4 级页表 PML4（本课程主线），CR3 寄存器指向顶级页表；PML5（5 级，支持超大地址空间）作为扩展讨论 | Sv39（3 级，本课程主线），satp 寄存器指向顶级页表；Sv48（4 级）作为扩展讨论 | Lab4 |
| 页表项权限位 | Present / Read-Write / User / NX（不可执行） | Valid / Read / Write / Execute / User，权限位直接编码在 PTE 里更直观 | Lab4 |
| TLB 刷新 | `invlpg <addr>`（单页）或重新加载 CR3（全刷） | `sfence.vma <addr>, <asid>`（可单页可全刷，看操作数） | Lab4 |
| 缺页异常地址来源 | `CR2` 寄存器 | `stval` 寄存器（riscv 把这个寄存器复用给多种异常的附加信息） | Lab4 |
| 中断/异常分发 | IDT（中断描述符表，256 项）+ GDT（全局描述符表，定义段和 TSS）+ PIC（老式）/ APIC（现代，本课程主线） | `stvec` 寄存器指向单一 trap 入口，内核软件自己用 `scause` 判断类型再分发 + PLIC（平台级中断控制器，管外部中断）+ CLINT（核心本地中断，管定时器/软件中断） | Lab5 |
| 中断向量风格 | 硬件按中断号跳转到 IDT 里对应的独立入口 | 所有异常/中断统一跳到 `stvec` 一个地址，软件读 `scause` 分支 | Lab5 |
| 时钟源 | PIT（8253/8254，老式，本课程起步用）；APIC Timer（现代，扩展讨论） | CLINT 里的 `mtime`/`mtimecmp`（riscv 特权架构规范定义） | Lab5 |
| 系统调用指令 | `syscall`（现代，配合 `sysret` 返回，本课程主线）；`int 0x80`（传统方式，作为对照讨论） | `ecall`（唯一方式，riscv 没有传统/现代之分） | Lab6 |
| 系统调用号/参数传递 | `syscall`：调用号在 `rax`，参数在 `rdi,rsi,rdx,r10,r8,r9`（注意第 4 个参数是 `r10` 不是 `rcx`，因为 `syscall` 会破坏 `rcx`） | `ecall`：调用号在 `a7`，参数在 `a0-a5`，返回值在 `a0` | Lab6 |
| 用户态切换相关寄存器 | `MSR`（Model-Specific Register）配置 `syscall` 入口地址（`IA32_LSTAR`）等 | 无需 MSR 概念，`sepc`/`sstatus` 等 CSR（Control and Status Register）直接控制 | Lab6 |
| 串口地址（QEMU 默认） | I/O 端口 `0x3F8`（COM1，走 `in`/`out` 指令） | 内存映射 MMIO `0x10000000`（QEMU virt 机器的 UART16550，直接读写内存地址） | Lab1, Lab5 |
| 多核启动 | ACPI MADT 表列出可用 CPU，通过 APIC 发送 INIT/SIPI 中断唤醒 AP（Application Processor） | SBI HSM（Hart State Management）扩展调用唤醒其他 hart | Lab10 |
| 原子指令基础 | `lock` 前缀 + `cmpxchg`/`xadd` 等 | `amoswap`/`amoadd` 等原子内存操作（AMO）指令，或 `lr`/`sc`（load-reserved / store-conditional）对 | Lab10 |

## 关键差异的直觉解释

### 为什么 x86 启动流程比 riscv 复杂

x86 背着 40 多年的历史包袱：CPU 上电默认进 16 位实模式（兼容 1978 年的 8086），要一路手动切到 32 位保护模式再到 64 位长模式，每一步都要配置好对应的描述符表。riscv 是现代设计，规范里直接规定了 hart 上电后的行为，且把"这台机器有什么外设、内存多大"这类事交给标准化的 OpenSBI + Device Tree，内核不需要像 x86 那样解析一堆历史遗留的枚举方式（PCI 总线扫描、ACPI 表）就能拿到基本信息。

这不代表 riscv "更好"，只代表它设计得晚，能避开兼容性负担。等你在 Lab10 处理 x86 的 ACPI/APIC 多核启动时，你会直接感受到这种历史包袱的重量。

### 为什么 riscv 的 trap 处理感觉"更统一"

x86 的 IDT 给每一种中断/异常独立的入口地址，硬件跳转时已经帮你区分好了"这是第 14 号缺页异常还是第 32 号时钟中断"。riscv 只有一个 `stvec` 入口，所有 trap 都先走到这里，内核自己读 `scause` 判断类型再分发。这是设计哲学的取舍：x86 把分发逻辑放硬件里（换来更少的软件开销，但硬件更复杂），riscv 把分发逻辑放软件里（换来更简单的硬件，但每次 trap 多一次软件判断）。

### 为什么系统调用参数传递里 x86 的 `r10` 显得突兀

Linux/x86_64 的函数调用约定（System V ABI）第四个整型参数本该用 `rcx`，但 `syscall` 指令执行时会把返回地址存进 `rcx`（这是硬件行为，改不了），所以系统调用约定专门把第四参数换成 `r10`，避开冲突。这是"通用函数调用约定"和"系统调用专用指令的硬件副作用"打架之后留下的历史疤痕，riscv 的 `ecall` 没有这个问题，因为它不覆写任何通用寄存器，返回地址由软件在 `ecall` 前用 `sepc` 相关机制处理。

### 页表权限位设计哲学差异

x86 的 PTE 权限位是"否定式"的（比如 NX 位是 1 表示不可执行，默认可执行），这也是历史遗留——早期 x86 页表根本没有执行权限位，NX 是后来加的扩展位，为了兼容只能用"1 = 禁止"这种反直觉编码。riscv 的 PTE 权限位是"肯定式"的（R/W/X 位是 1 才表示可读/可写/可执行），从零设计，更直观，写代码时更不容易搞反。

## 逐 Lab 精简对照表位置索引

完整对照表在每个 Lab 的 README 里，本文档只做全局索引。如果你正在做 Lab4，直接看 [`labs/lab04-virtual-memory/README.md`](../labs/lab04-virtual-memory/README.md) 里的对照表就够了，不需要每次都翻这份总表。
