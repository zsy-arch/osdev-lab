# Lab0：环境、工具链、QEMU、构建系统

## 学习目标

- 搭好本课程后续全部实验需要的工具链（Docker 或原生二选一）。
- 理解 `make run`/`make test` 背后到底发生了什么，而不是把它当成一个黑盒命令。
- 跑通第一个 QEMU 会话，确认串口 I/O 通路是好的——这是后面所有 Lab 判断"我的代码到底有没有在跑"的唯一渠道。

## 前置 Lab

无。这是第一课。

## 核心概念

**为什么内核开发不能用"平时写应用代码"的方式验证代码对不对**：应用代码跑在一个已经启动好的操作系统上面，你写错了，操作系统会用信号（比如 SIGSEGV）、错误码、崩溃报告把问题摆在你面前。内核代码本身就是"操作系统"，没有更底层的东西替你兜底，写错了 CPU 可能直接卡死、静默重启、或者做出完全无法预测的事情（比如把不该写的物理内存改了）。QEMU 模拟器在这里的作用是给你一个"可以随时按重置键、不会真的把硬件搞坏"的沙盒。

**交叉编译（cross compilation）**：你的开发机（比如运行 macOS 的 Apple Silicon）和你要生成代码的目标（x86_64 或 riscv64 裸机）是两种不同的指令集，普通编译器只会生成"当前机器能跑"的代码。交叉编译器是"跑在 A 架构上、生成 B 架构代码"的编译器，这是本课程 Lab0 要装的核心工具。

**为什么本课程默认 `-serial stdio -display none`**：没有图形界面的内核想要输出信息，最简单可靠的方式是往串口（serial port）写字节。QEMU 可以把模拟出来的虚拟串口重新映射到你终端的标准输入输出，这样你写内核代码时用的"打印"手段，和你平时 `printf` 调试应用代码时看到的效果一样直观，只是背后走的硬件路径完全不同。

## x86_64 与 riscv64 对照表

| 主题 | x86_64 | riscv64 |
|---|---|---|
| 交叉编译器 | `x86_64-elf-gcc`（或系统 gcc + `-ffreestanding`） | `riscv64-elf-gcc`（没有系统原生工具链可用，必须交叉编译） |
| QEMU 二进制 | `qemu-system-x86_64` | `qemu-system-riscv64` |
| 需要额外引导工具 | 是（`grub-mkrescue` 制作 ISO，见 Lab1） | 否（QEMU `-bios default` 自动加载 OpenSBI） |
| 调试器 | `x86_64-elf-gdb` 或 `gdb-multiarch` | `riscv64-elf-gdb` 或 `gdb-multiarch` |

本 Lab 不涉及内核代码本身的架构差异（那是 Lab1 开始的内容），这里的对照纯粹是"工具链层面"的差异。

## 代码目录与关键文件

Lab0 没有内核代码，starter/solution 都只是占位说明；真正的"产出"是你机器上跑通的环境。

```
labs/lab00-environment/
  README.md          本文件
  starter/            占位说明（没有 TODO 代码，因为这个 Lab 的任务是装环境不是写代码）
  solution/           一份 shell 脚本示例，展示"环境装对之后"能跑通的最小验证
  tests/expect.txt    验收标准说明（本 Lab 的"测试"是 scripts/check-env.sh 而非 QEMU 输出比对）
```

## 分步实现步骤

1. 阅读 [`docs/environment.md`](../../docs/environment.md)，选择 Docker 或原生工具链路线。
2. 按对应路线的步骤安装。
3. 运行环境检查：
   ```bash
   bash ../../scripts/check-env.sh
   ```
   直到看到"环境检查通过"且没有 `[MISS]` 标记的必需项（`[SKIP]` 的可选项，比如 Rust，可以先忽略）。
4. 跑一次最小 QEMU 烟雾测试，确认交叉编译器 + 链接器 + QEMU 这条链路完整可用（这一步还没有我们自己的内核，只是验证工具链装对了）：
   ```bash
   bash solution/smoke-test.sh
   ```
   两个架构都看到 `[PASS]` 才算过关；这个脚本会实际交叉编译一段最小汇编、链接成 ELF、丢进 QEMU 跑、再检查串口输出，跟真正的 Lab1 走的是同一条链路（只是代码极简）。
5. 进入 [`labs/lab01-boot-hello/README.md`](../lab01-boot-hello/README.md) 开始写你的第一行内核代码。

## QEMU 运行命令

Lab0 阶段还没有自己的内核镜像，这里给的是"验证 QEMU 本身工作正常"的命令，用 QEMU 自带的固件做一次最小烟雾测试：

```bash
# x86_64：不给 -kernel，只是确认 QEMU 能起来又能正常退出
# (macOS 默认没有 timeout 命令，装了 coreutils 就用 gtimeout；也可以手动 Ctrl-C 退出)
timeout 3 qemu-system-x86_64 -display none -serial stdio -no-reboot || true

# riscv64：加载 OpenSBI 固件，能看到 OpenSBI 自己的 banner 输出就说明固件加载正常
timeout 3 qemu-system-riscv64 -machine virt -bios default -display none -serial mon:stdio || true
```

> 如果你的系统没有 `timeout`（macOS 默认没有），把 `timeout 3` 换成 `gtimeout 3`（`brew install coreutils` 后可用），或者直接去掉 `timeout 3` 手动用 Ctrl-C 结束。

riscv64 那条命令你应该能看到一段 OpenSBI 的版本信息横幅，这说明固件层是通的（后面 Lab1 我们的内核代码就是在这段横幅之后被加载执行的）。x86_64 那条命令没有 `-kernel`，QEMU 会因为没有可引导设备而报错或停在固件层，这是预期行为，本课程 Lab0 只关心"QEMU 进程本身能不能正常启动"，不关心它有没有东西可引导。

## GDB/QEMU Monitor 调试方法

Lab0 不涉及调试自己的代码，跳过。等 Lab1 有了第一份可以设断点的代码，再开始用 [`docs/debugging.md`](../../docs/debugging.md) 里的方法。

## 自动验收测试

```bash
bash tests/run.sh
```

这个测试脚本本质是重新跑一次 `scripts/check-env.sh`，退出码 0 表示环境合格。它不是 QEMU 串口输出比对（本课程从 Lab1 开始才用那种方式验收），因为 Lab0 的"验收标准"是环境本身，不是内核行为。

## 常见坑与排查

- **macOS 上 `pip install` 或改动 system Python 环境导致其它工具异常**：本课程涉及的少量 Python 脚本（如果有）应该在虚拟环境里跑，不要动系统 Python/pip。
- **Docker Desktop 默认的磁盘空间/内存限额太小**：构建镜像 + 跑多个 QEMU 实例，建议 Docker Desktop 至少分配 4GB 内存、20GB 磁盘。在 Docker Desktop 设置里调整。
- **详见** [`docs/faq.md`](../../docs/faq.md) 的"环境相关"一节，覆盖了绝大多数第一次装环境会遇到的问题。

## 挑战任务

- 同时装好 Docker 和原生工具链两条路线，对比两者跑 `make test ARCH=x86_64` 的耗时差异，理解容器化带来的开销具体在哪个环节（提示：用 `time` 命令分别测两种模式）。
- 如果你打算做 Rust 分支（见 [`docs/rust-track.md`](../../docs/rust-track.md)），提前把 `rustup target add x86_64-unknown-none riscv64gc-unknown-none-elf` 跑一遍，确认 `check-env.sh` 里 Rust 那两行从 `[SKIP]` 变成 `[OK]`。

## 参考

- OSDev Wiki: [Setting Up a Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler)、[QEMU](https://wiki.osdev.org/QEMU)
- xv6-riscv: 整个仓库的 [`Makefile`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/Makefile) 是研究"一个教学内核如何组织构建系统"的好例子，尤其是它怎么自动探测 `riscv64-unknown-elf-` 前缀
