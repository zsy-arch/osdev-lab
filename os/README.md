# osdev-lab OS

一个跑在 QEMU 里、支持 x86_64 与 riscv64 两种架构的教学向操作系统。从物理
内存管理到多进程 shell，整条链路都是从零手写的：分页、中断与系统调用、
进程调度（fork/exec/wait）、块设备驱动、只读文件系统、ELF 加载器、管道，
再往上是一个极简用户态 libc 和六个用户程序（`init`/`sh`/`ls`/`cat`/
`echo`/`grep`），组成一个可以交互的 shell 环境。

## 特性

- **双架构内核**：x86_64（Multiboot2 引导，可生成 GRUB ISO 或直接
  `-kernel` 启动）与 riscv64（OpenSBI 引导），各自独立实现，不共用二进制。
- **内存管理**：物理页分配器（`kalloc`）+ 四级/三级页表，内核与用户地址
  空间分离，每个进程拥有独立的用户态映射。
- **中断与系统调用**：x86_64 走 IDT + `syscall`/`sysret`，riscv64 走
  `ecall`/`sret`；两边都有可编程定时器驱动抢占式调度。
- **进程管理**：`fork`/`exec`/`wait`/`exit`，基于时间片轮转的调度器。
- **文件系统**：只读的块设备文件系统（超级块 + inode + 直接块），x86_64
  经 ATA PIO 读盘，riscv64 经 virtio-blk 读盘，逻辑层完全共享（`fs.c`/
  `fs.h`/`blk.h` 在两个架构目录下逐字节相同）。
- **ELF 加载器 + 管道**：用户程序以磁盘上的 ELF 文件形式存在，运行时由
  内核解析装载；管道（`pipe`/`dup`）让 shell 具备 `a | b` 重定向能力。
- **交互式 shell**：`/init` 启动后先跑 `/initrc` 里的一组命令，再进入
  可交互的 `sh`。

## 目录结构

```
os/
├── x86_64/         x86_64 内核实现（boot.S、pagetable、trap、proc、fs、ide ...）
├── riscv64/        riscv64 内核实现（boot.S、pagetable、trap、proc、fs、virtio ...）
├── common/         两个架构共用的基础设施：console、panic、string、kalloc
├── user/           用户态源码（六个程序 + 极简 libc + 每架构的 crt0/系统调用桩），
│                   两个架构共用同一份源码
├── elf.h           ELF 加载器与内核共用的常量定义
├── fs_format.h     磁盘文件系统格式定义（mkfs 和内核都依赖这一份）
├── syscall.h       系统调用号——内核与用户程序之间的 ABI 契约
├── mkfs/           宿主工具：把文件打包成本系统的磁盘镜像格式
├── fsroot/         打包进磁盘镜像的种子文件（motd、初始化脚本等）
├── scripts/        QEMU 启动脚本、自动化测试脚本
├── tests/          串口输出的期望结果（按架构区分）
└── Makefile        构建入口
```

## 快速开始

需要的工具链见下方“依赖”一节。装好之后：

```bash
# 编译并在 QEMU 里跑起来（串口输出直接打到当前终端，Ctrl-C 退出）
make run ARCH=x86_64
make run ARCH=riscv64

# 只编译，不运行
make build ARCH=x86_64

# 编译、跑一次、跟已知的预期串口输出自动比对
make test ARCH=x86_64
make test-all              # 两个架构各跑一次

# 清理构建产物
make clean
```

启动后会看到类似这样的输出（riscv64，节选）：

```
Hello OS from riscv64 (Lab9: shell and userspace)
switched to page table, low identity map gone
timer armed, ecall entry armed, mounting filesystem
virtio: block device at slot 0 (0x10001000), version 2
fs: mounted, magic ok, 512 blocks, 32 inodes, data from block 18
filesystem ready, loading /init
init loaded (pid 1), entering scheduler
$ echo lab9 shell up
lab9 shell up
$ ls
exact.txt  initrc  motd.txt  init  sh  ls  cat  echo  grep
$ cat /motd.txt | grep lab
osdev-lab9: userspace is alive
this file lives on a virtual disk, in a lab filesystem
```

`/initrc` 跑完之后会留在交互式 shell 里，可以自己敲命令（`ls`、
`cat <file>`、`echo ...`、`grep <word> <file>`，以及 `|` 管道和 `<`
输入重定向）。

## 依赖

- 一个能编译裸机代码的交叉编译器：
  - x86_64：`x86_64-elf-gcc`（macOS 唯一选项）或 Linux 上的系统
    `gcc`/`clang`（其 `as`/`ld` 就是 GNU binutils，配合
    `-ffreestanding` 直接可用）。
  - riscv64：`riscv64-elf-gcc` / `riscv64-unknown-elf-gcc` /
    `riscv64-linux-gnu-gcc` 三者之一。
- QEMU：`qemu-system-x86_64` 与 `qemu-system-riscv64`。
- 制作 x86_64 引导 ISO 需要 `grub-mkrescue` + `xorriso` + `mtools`
  （macOS 上是 `i686-elf-grub`，不是 `x86_64-elf-grub`——后者只打包了
  EFI 目标，做出来的 ISO 传统 BIOS 引导不了）。
- `timeout`（Linux 自带）或 `gtimeout`（macOS：`brew install coreutils`）
  ——只有跑 `make test` 才需要，用于给 QEMU 的串口会话加超时保护。

工具链探测顺序、找不到时的具体报错和安装命令都写在 `Makefile` 顶部的
`CROSS_X86_64`/`CROSS_RISCV64` 相关段落里；找不到自动探测的工具时可以
显式指定，例如：

```bash
make build ARCH=x86_64 CROSS_X86_64=x86_64-elf-
make build ARCH=riscv64 CROSS_RISCV64=riscv64-linux-gnu-
```

## 调试

```bash
make debug ARCH=riscv64
# 另开一个终端：
riscv64-elf-gdb riscv64/build/kernel.elf -ex 'target remote :1234'
```

`MONITOR=1` 可以额外打开 QEMU monitor（`telnet 127.0.0.1 4444`），用来
查看设备树/中断状态等底层信息。

## 架构说明

两个架构的内核源码各自独立（`x86_64/`、`riscv64/`），但共享同一套
架构无关的逻辑：文件系统（`fs.c`/`fs.h`/`blk.h`）、ELF 加载器
（`exec.c`）、管道（`pipe.c`/`pipe.h`）在两个目录下逐字节相同——它们
描述的是与硬件无关的行为，架构差异已经被各自的 `blk.h`/`pagetable.c`
挡在下面一层。用户态源码（`user/`）则完全共用一份，只有底层的程序入口
（`crt0_<arch>.S`）和系统调用桩（`usys_<arch>.S`）按架构区分，因为这两处
直接对应 CPU 的调用约定和陷入指令。

磁盘镜像的内容由 `mkfs`（一个跑在宿主机上的原生小工具）在构建期生成：
把 `fsroot/` 下的种子文件和编译好的六个用户程序 ELF 一起打包成本系统的
文件系统格式，作为 `fs.img` 提供给 QEMU 挂载。内核本身只实现只读的读取
路径，不实现写入——创建文件系统格式的工作交给宿主工具，这是很多教学
内核（包括 xv6）的常规分工。
