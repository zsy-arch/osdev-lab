# Rust 分支（可选附录）

本课程主线是 C11 + GNU 汇编。如果你已经走完 C 版本（至少到 Lab4，理解了内存管理和页表的裸机实现细节），想看 Rust 在这些场景下能提供什么保证、又在哪里必须"逃生"到 unsafe，这份附录给出迁移路线。

**这不是强制路径。** 不建议把 Rust 分支当成第一遍学习方式——Rust 的所有权系统会在你还没理解"为什么裸机代码这么写"之前，就用编译器错误替你挡掉一些坑，你会失去亲手踩坑、理解"这个不变量为什么重要"的过程。先用 C 走一遍全课程主线，再回来看 Rust 版本"避免了什么类型的错误、又在哪里避不开"，收益最大。

## 为什么裸机 Rust 和应用层 Rust 差异很大

日常写 Rust 应用代码时，你几乎不需要接触 `unsafe`。裸机内核开发反过来：几乎每一处触碰硬件的代码（读写特定物理地址、解释固件传来的原始内存、执行特权指令）都要标 `unsafe`，因为这些操作的"安全性"依赖的是硬件手册里的约定，不是 Rust 编译器能验证的类型不变量。Rust 在这里的价值不是"消除 unsafe"，而是"把 unsafe 的边界收窄到最小、显式标出来，剩下 95% 的代码——数据结构、算法、状态机——仍然享受完整的借用检查"。

## 环境准备

```bash
rustup target add x86_64-unknown-none
rustup target add riscv64gc-unknown-none-elf
rustup component add llvm-tools-preview
cargo install cargo-binutils      # 提供 cargo objcopy 等
```

`x86_64-unknown-none` / `riscv64gc-unknown-none-elf` 是"裸机目标"（no_std，没有操作系统假设），对应我们交叉编译器里的 `-ffreestanding`。

## 每个 Lab 的 Rust 迁移要点

以下按 Lab 编号列出迁移时的关键差异点，不重复 C 版本已经讲过的操作系统原理，只讲"这一步在 Rust 里怎么表达、和 C 版本比省了什么/多了什么"。

### Lab1-2：Boot 与内核入口

- 用 `#![no_std]` + `#![no_main]`，自己定义 `_start`（`#[no_mangle] extern "C" fn _start() -> !`），等价于 C 版本手写的汇编入口跳转到 C 函数。
- `panic_handler` 必须自己实现（`#[panic_handler]`），这是 Rust no_std 强制要求的，比 C 版本"忘记写 panic 处理也能编译，只是链接时才报错"更早发现问题。
- BSS 清零：如果你完全手写汇编入口再跳 Rust，行为和 C 版本一致，需要手动清零。如果用 `build.rs` 生成的启动代码或某些 crate（比 riscv 生态的 `riscv-rt`），可能已经帮你处理，注意别重复清零导致覆盖已初始化的 `.data`。
- 参考 crate：[`bootloader`](https://crates.io/crates/bootloader)（x86_64，配合 Limine 或自带引导）、[`riscv-rt`](https://crates.io/crates/riscv-rt)（riscv64 启动运行时）。**建议第一次迁移时不用这些 crate，自己手写启动路径**，理解完整过程后再考虑引入它们简化代码——这正是 *Writing an OS in Rust* 教程本身的做法（前几章手写，后面逐步引入生态 crate）。

### Lab3：物理内存管理

- 空闲链表分配器本身是纯数据结构逻辑，用 Rust 写和 C 几乎一样，主要差异是指针操作全部要包一层 `unsafe`。
- 可以练习实现 Rust 的 `GlobalAlloc` trait，这样后续 Lab 里就能用 `Vec`/`Box` 等标准库数据结构（仍然是 no_std，但配合 `alloc` crate），比 C 版本手动管理链表/数组更省心。*Writing an OS in Rust* 的 "Allocator" 系列章节完整讲了这一步，本课程不重复展开，直接参考。

### Lab4：虚拟内存与页表

- 页表项可以用 Rust 的位域库（比如 `bitflags` crate）表达权限位，编译期保证你不会拼错某个 flag 组合，比 C 里手写 `#define PTE_P 0x1` 之类的宏更不容易犯拼写错误。
- 页表遍历函数如果设计成返回 `Option<&mut PageTableEntry>` 而不是裸指针，能让"页表项不存在"这个分支在类型层面强制处理，不会像 C 版本一样"忘记检查空指针"编译也不报错。
- 这是体会 Rust 价值最明显的一课：页表相关的 bug（比如页表项类型混用、忘记检查权限位）在 C 里是运行时才炸，用得当的 Rust 类型设计能把一部分变成编译期错误。

### Lab5-6：中断与系统调用

- 中断处理函数的调用约定（x86 的 `x86-interrupt` calling convention）Rust 曾经有实验性支持（`#[naked]` 或 `extern "x86-interrupt"`），具体可用性随 Rust 版本变化较大，实际项目里更常见的做法是用汇编写最外层 trap 入口（保存/恢复寄存器），再调用一个普通 Rust 函数处理逻辑——这和 C 版本的做法完全一致，汇编这一层无论哪种语言都逃不掉。
- 系统调用的参数传递、特权级切换汇编部分，Rust 版本和 C 版本几乎是逐行对应的内联汇编（`core::arch::asm!` 宏 vs GNU 汇编内联语法），这里语言差异最小，工作量差异也最小。

### Lab7 之后（进程调度、文件系统、shell）

这些 Lab 涉及大量"面向操作系统语义的数据结构和算法"（进程状态机、inode 管理、ELF 解析），是 Rust 类型系统（枚举 + match 表达状态机、`Result` 表达可能失败的操作）相对 C 的裸 `switch`/返回错误码收益最大的地方，但也是本课程篇幅最长、最需要你已经在 C 版本里吃透原理的部分。这部分暂不提供逐行迁移指南，建议吃透 C solution 之后，把它当作一个独立的"用 Rust 重新设计这套语义"的练习，收益比照抄迁移大得多。

## 参考资源

- [*Writing an OS in Rust*](https://os.phil-opp.com/)：本附录的主要参考，x86_64 为主，覆盖 Freestanding、VGA 文本模式、Interrupts、Paging、Heap Allocator 等章节，和本课程 Lab1-4 直接对应。
- [`riscv-rt`](https://github.com/rust-embedded/riscv-rt) 和 [rCore-Tutorial](https://github.com/rcore-os/rCore-Tutorial-v3)：riscv64 Rust 裸机开发的实际生产参考，rCore-Tutorial 本身就是完整的 Rust 教学内核，读到这一步你已经具备直接读懂它源码的能力。
- [The Embedded Rust Book](https://docs.rust-embedded.org/book/)：更广义的"no_std + 硬件交互"参考，不专门讲 OS 内核但讲透了 no_std 生态的基础工具。
