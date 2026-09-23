# osdev-lab

从零开始写一个操作系统内核：boot、内存管理、页表、中断、系统调用、进程调度、文件系统、用户态 shell，**x86_64 与 riscv64 双架构对照**实现。

面向读者：会写 C、读得懂一点汇编、用惯 Linux 命令行，但从没写过 OS 的人。看完并做完全部 Lab，你会对"内核到底做了什么"有一个可以自己动手验证的答案，而不是停留在教科书插图层面。

## 这套 Lab 是什么

- 11 个 Sub Lab（Lab0 ~ Lab10），循序渐进，每个 Lab 单独可编译、可在 QEMU 里跑、可自动判分。
- 每个核心 Lab 都同时给出 **x86_64** 和 **riscv64** 两套实现，并列对照，不是"讲一个架构，另一个说'类似'"。
- 主线语言 C11 + GNU 汇编（AT&T 语法）。Rust 作为可选分支，见 [`docs/rust-track.md`](docs/rust-track.md)。
- 每个 Lab 目录下有三件套：`starter/`（带 TODO 的骨架，你来填）、`solution/`（可直接跑通的参考实现）、`tests/`（自动验收脚本）。
- 用 QEMU 模拟硬件，不需要真机、不需要 U 盘刻录、不会把你的电脑变砖。

## 学习路径

```
Lab0  环境与工具链           —— 把工具链装对，跑通第一个 QEMU 窗口
Lab1  Boot 与裸机输出         —— 串口打印 "Hello OS"，理解引导流程
Lab2  内核入口与链接脚本      —— BSS 清零、栈、panic、符号表
Lab3  物理内存管理           —— 从固件拿到内存布局，做一个页分配器
Lab4  虚拟内存与页表         —— 4 级/3 级页表，映射、权限位、缺页
Lab5  中断异常与时钟         —— trap 入口、时钟中断、内核态断点
Lab6  用户态与系统调用       —— 特权级切换，第一次从 Ring3/U 模式调用内核
Lab7  进程与调度             —— fork/exec/wait/exit，上下文切换
Lab8  文件系统               —— 块设备 + 简化 inode 文件系统
Lab9  Shell 与用户程序       —— ELF 加载、mini libc、命令行
Lab10 SMP 与进阶话题         —— 多核启动、锁、两个架构的收尾对比
```

详细的目标、难度曲线、可选路线（要不要做 Lab10、要不要走 Rust 分支）见 [`ROADMAP.md`](ROADMAP.md)。

## 30 秒开始

需要 Docker（推荐，环境完全一致）或者按 [`docs/environment.md`](docs/environment.md) 装原生工具链。

```bash
git clone <this-repo> osdev-lab   # 或者你已经在本地了
cd osdev-lab

make setup              # 构建/拉取开发环境（Docker 镜像，或检查本机工具链）
make run ARCH=x86_64    # 跑 solution 里当前最新 Lab 的 x86_64 版本
make run ARCH=riscv64   # 跑 riscv64 版本
```

看到串口打印 `Hello OS from x86_64` / `Hello OS from riscv64` 就说明环境是通的。之后进入 [`labs/lab00-environment/README.md`](labs/lab00-environment/README.md) 开始第一课。

## 常用命令

```bash
make setup                        # 环境检查 / 构建 Docker 镜像
make run ARCH=x86_64 LAB=lab03    # 运行指定 Lab 的 solution
make debug ARCH=riscv64 LAB=lab05 # 用 QEMU -S -s + gdb-multiarch 调试
make test ARCH=x86_64             # 跑当前 Lab 的自动验收测试
make test-all                     # 跑全部 Lab 在两个架构上的测试（CI 用）
make clean                        # 清理构建产物
```

单个 Lab 内部也可以独立操作：

```bash
cd labs/lab03-physical-memory
make run ARCH=x86_64      # 只跑这个 Lab
make test ARCH=riscv64    # 只测这个 Lab
```

`make test` 的判据是串口输出，而串口有一个确定性的盲区：**内核打完最后一行之后崩掉，串口上和正常空转逐字节相同**。本课程在 Lab6 x86_64 上真实踩到过一次。什么时候必须额外补一次检查、用什么判据、以及一个反直觉的陷阱（标准运行参数里的 `-no-reboot` 会让 `-d cpu_reset` 查不出三重故障）见 [`docs/verification-methodology.md`](docs/verification-methodology.md)。

## 目录结构

```
osdev-lab/
  README.md            本文件
  ROADMAP.md            Sub Lab 列表、里程碑、难度曲线
  LICENSE               MIT
  Makefile              顶层入口：setup / run / debug / test / clean
  docker/Dockerfile      固定版本工具链镜像
  .devcontainer/         VS Code Dev Container 配置
  docs/                  环境配置、架构对照表、调试指南、验证方法论、FAQ、Rust 分支说明
  labs/labXX-*/          每个 Sub Lab：README + starter + solution + tests
  src/
    common/              两个架构共享的代码（数据结构、字符串函数、打印等）
    x86_64/               x86_64 专属实现，按小节累积
    riscv64/              riscv64 专属实现，按小节累积
  scripts/               check-env.sh / run-qemu.sh / test-lab.sh / debug-gdb.sh
```

`src/` 里的代码是"累积式"的：Lab3 的 solution 依赖 Lab2 已经搭好的内核入口，Lab4 依赖 Lab3 的物理内存分配器，一直往上叠。每完成一个 Lab，对应的实现会合并进 `src/`，作为下一个 Lab 的起点——这和 xv6/rCore 的教学结构是一致的。如果你想跳到某个 Lab 直接看效果，也可以只进那个 Lab 目录下的 `solution/` 单独编译运行，它是自包含的。

## 参考与借鉴

实现思路上参考了下面这些项目，但代码是重新设计的教学最小实现，不是搬运：

- [xv6-riscv](https://github.com/mit-pdos/xv6-riscv)（MIT 6.S081 教学内核）
- [rCore-Tutorial](https://github.com/rcore-os/rCore-Tutorial-v3)
- [OSDev Wiki](https://wiki.osdev.org/)
- [Writing an OS in Rust](https://os.phil-opp.com/)（Rust 分支参考）

每个 Lab 文档末尾都标出对应参考项目的具体文件，方便你交叉阅读原版实现。

## 常见问题

macOS 上没有 `x86_64-elf-gcc` / `riscv64-elf-gcc`？没装 QEMU？GDB 连不上？看 [`docs/faq.md`](docs/faq.md) 和 [`docs/environment.md`](docs/environment.md)。

## 许可证

MIT，见 [`LICENSE`](LICENSE)。
