# Lab1：Boot 与裸机输出

## 学习目标

- 亲手写出让 CPU 从"固件/引导器交出控制权那一刻"跑到"你的第一行 C 代码"之间的全部代码——这中间没有任何东西替你兜底。
- 理解 x86_64 为什么要经过实模式→保护模式→长模式三级跳，而 riscv64 为什么只需要一次 M 模式→S 模式切换。
- 亲手写通串口驱动，建立后续所有 Lab 判断"内核到底有没有在跑"的唯一可靠渠道。
- 做到两边架构都能启动、初始化好最基本的执行环境（栈、必要的页表/特权级配置），然后串口打印一行字，最后进入一个安全的停机循环。

## 前置 Lab

[Lab0：环境、工具链、QEMU、构建系统](../lab00-environment/README.md)。如果 `bash scripts/check-env.sh` 还没有干净通过，先回去解决。

## 核心概念

**固件（firmware）和引导器（bootloader）的职责边界**：固件是硬件上电后第一个跑起来的代码，负责最基础的硬件初始化（比如让 CPU 进入一个已知的、可预测的状态）；引导器负责把内核镜像从存储介质加载到内存，并按某种约定（协议）把控制权交给内核。这两者经常被合并理解成"开机的东西"，但它们的职责完全不同，也是本课程 x86_64 和 riscv64 路线差异最大的地方：x86_64 用 GRUB 这种独立的引导器程序，读文件系统、解析配置、加载内核，是一个相对复杂的软件；riscv64 用 OpenSBI 这种"固件本身兼职做了引导器的活"，没有文件系统概念，直接跳到一个约定好的物理地址。

**特权级（privilege level）**：CPU 不是从一开始就有"内核态/用户态"的区别执行代码——这个区别是 CPU 硬件设计出来的一种保护机制，需要显式配置才会生效。x86_64 上电时其实处于比"内核态"更没有保护、更原始的实模式，一路配置到长模式才具备本课程需要的执行环境；riscv64 上电直接在 M 模式（最高特权级，通常只留给固件），OpenSBI 完成必要初始化后把控制权降级交给 S 模式（本课程内核运行的特权级）。Lab6 会详细展开"内核态/用户态"这一层特权级切换，这里先建立"特权级不是与生俱来的，是硬件按约定一步步配置出来的"这个认识。

**为什么两边都要在 C 代码跑之前手写一段汇编**：C 编译器生成的代码默认假设一些环境已经存在——比如栈指针是有效的、`.bss` 段已经清零（Lab2 会展开这一点）。在你自己的汇编代码把这些前提条件搭好之前，任何 C 函数都不能安全执行，哪怕只是最简单的一次函数调用（`call` 指令本身就要往栈上写返回地址）。这也是为什么本课程的入口点必须是 `.S` 汇编文件而不是 C 文件。

**为什么本 Lab 选 Multiboot2 + GRUB 而不是手写 x86 引导扇区**：手写一个 512 字节的 MBR 引导扇区（经典的"自己动手写 OS"入门方式）需要你先搞定实模式下的磁盘读取（BIOS `int 0x13` 中断）、20 位地址线之类的历史细节，这些内容和"操作系统内核如何工作"关系不大，纯粹是给你增加历史包袱。Multiboot2 是一个标准化协议，GRUB 帮你处理了磁盘读取、ELF 解析这些脏活，你的内核收到的是一个已经规整好的启动状态（比如按 Multiboot2 规范传来的信息结构指针），可以把精力直接放在"CPU 特权级怎么一路切到长模式"这个真正值得学的部分。本 Lab 末尾的 [Limine 附录](#附录limine-引导器方案) 给出另一种引导器的对照，如果你对引导协议这一层本身感兴趣可以对比着看。

## x86_64 与 riscv64 对照表

| 主题 | x86_64 | riscv64 |
|---|---|---|
| 固件/引导 | BIOS（QEMU 默认用 SeaBIOS）→ GRUB 读取 ISO 里的 Multiboot2 内核并加载 | OpenSBI（M 模式固件，QEMU `-bios default` 自动加载）直接跳转到内核 |
| 内核加载地址 | 由链接脚本决定（本 Lab 选 `0x100000`，1MiB 处，避开实模式遗留的低地址区域），GRUB 按 ELF 加载 | 固定物理地址 `0x80200000`，OpenSBI 硬编码跳转到这里，不读取 ELF 的入口字段 |
| 启动时特权级路径 | 实模式 → 保护模式（32 位，GRUB 已经帮你切好，内核接手时已经在这里）→ 长模式（64 位，内核自己切） | M 模式 → S 模式（OpenSBI 完成，内核接手时已经在 S 模式） |
| 进入 64 位模式需要 | 是，且步骤多：开 PAE、填一份最小页表、设 `EFER.LME`、开分页、`lgdt`+`ljmp` 远跳 | 否，riscv64 的 hart 从复位开始就是完整的 64 位执行环境，没有"32 位过渡阶段"这一说 |
| 入口时寄存器约定 | 无特定寄存器约定，Multiboot2 信息结构地址在 `%ebx`（本 Lab 暂不使用，Lab3 会用到里面的内存映射信息） | `a0` = hartid（当前核心编号），`a1` = 设备树（DTB）指针（本 Lab 暂不使用，Lab3 会解析它） |
| 串口地址 | I/O 端口 `0x3F8`（COM1），用 `in`/`out` 指令访问，是独立的 I/O 地址空间 | 内存映射 I/O（MMIO）`0x10000000`（QEMU virt 机器固定地址），用普通的内存读写指令访问 |
| 停机方式 | `cli`（关中断）+ `hlt`（暂停直到下一次中断）循环 | `wfi`（等待中断）循环，riscv64 默认中断是关闭的，不需要额外指令关中断 |

**串口访问方式的差异是本 Lab 最值得记住的一点**：x86 把"外设"和"内存"看作两个独立的地址空间，访问外设要用专门的 `in`/`out` 指令（独立 I/O，Port-Mapped I/O）；riscv（以及大多数现代架构，包括 ARM）把外设寄存器直接映射到内存地址空间的某一段，用普通的指针读写就能访问（内存映射 I/O，Memory-Mapped I/O）。这不是简化，是两种真实存在、至今仍在使用的硬件设计哲学，你会在 `solution/x86_64/console_putc.c` 和 `solution/riscv64/console_putc.c` 里看到这个差异直接体现在代码写法上——一个用内联汇编包装的 `inb`/`outb`，一个用 `volatile` 指针。

完整版对照表见 [`docs/arch-compare.md`](../../docs/arch-compare.md)。

## 代码目录与关键文件

```
labs/lab01-boot-hello/
  README.md                  本文件
  Makefile                    ARCH=x86_64|riscv64 VARIANT=solution|starter 通用构建入口
  starter/
    x86_64/                   带 TODO 的骨架，缺 boot.S 里长模式切换的关键几步
    riscv64/                  带 TODO 的骨架，缺 boot.S 里的栈设置和 kernel_main 调用
  solution/
    x86_64/
      boot.S                  Multiboot2 头 + 32 位入口 + 长模式切换 + 跳 C
      linker.ld                控制内核加载地址（0x100000）和段布局
      console_putc.c            实现 console.h 声明的 console_putc()：COM1 端口 I/O
      panic_arch.c               实现 panic.h 声明的 panic_halt()：cli+hlt 循环
      kernel_main.c              内核 C 入口，打印一行字
      grub.cfg                   GRUB 配置，声明用 multiboot2 协议加载 kernel.elf
    riscv64/
      boot.S                    极简入口：设栈指针，call kernel_main，wfi 循环
      linker.ld                  控制内核加载地址（0x80200000，OpenSBI 约定）
      console_putc.c              实现 console_putc()：UART MMIO 读写
      panic_arch.c                实现 panic_halt()：wfi 循环
      kernel_main.c                内核 C 入口，打印一行字
  tests/
    expect-x86_64.txt           x86_64 期望的串口输出
    expect-riscv64.txt          riscv64 期望的串口输出
```

`console_putc()`/`panic_halt()` 是两个架构必须各自实现的"契约函数"，签名定义在 [`src/common/include/console.h`](../../src/common/include/console.h) 和 [`src/common/include/panic.h`](../../src/common/include/panic.h)；`console_puts`/`console_puts_line` 等更高层的函数是架构无关的公共代码（[`src/common/console.c`](../../src/common/console.c)），两个架构共用同一份实现，只在最底层的"怎么把一个字节写出去"这一点上分叉。这个"公共逻辑 + 架构分叉的最小接口"结构会贯穿本课程后面所有 Lab。

## 分步实现步骤

### x86_64 路线

1. **写 Multiboot2 头**（`boot.S` 的 `.multiboot` 段）：按规范填 `magic`（固定值 `0xE85250D6`）、`architecture`（0 表示 i386/x86_64 保护模式）、`header_length`、`checksum`（这四个字段的和必须模 2^32 等于 0，所以 checksum 是前三者的和取负），再加一个类型 0、大小 8 的结束标记。这个头必须落在 ELF 文件开头 32768 字节以内，GRUB 才能找到它。
2. **32 位入口 `_start`**：这时 CPU 还在 GRUB 留下的 32 位保护模式，先设好 `%esp`（栈指针），因为接下来要开始用栈。
3. **搭一份最小页表**：长模式要求分页必须开启。本 Lab 用 PAE 模式下的 2MiB 大页，只搭 3 级（PML4→PDPT→PD，省掉最底层的 PT），把物理地址 `[0, 1GiB)` 恒等映射（虚拟地址等于物理地址），够内核现阶段用了。完整的 4 级页表和非恒等映射是 Lab4 的内容。
4. **按顺序切换到长模式**（顺序不能打乱，具体原因见 `boot.S` 里对应位置的注释）：
   - 开 `CR4.PAE`
   - 把 `CR3` 指向刚搭好的 PML4（**必须在开分页之前做好**，否则开分页那一刻 CR3 是无效值，CPU 立刻三重故障重启）
   - 通过 `rdmsr`/`wrmsr` 设置 `EFER.LME`（Long Mode Enable）
   - 开 `CR0.PG`（正式开启分页，CPU 进入"IA-32e 兼容子模式"，但还没有真正按 64 位解码指令）
   - `lgdt` 加载一份包含 64 位代码段描述符的全局描述符表（GDT）
   - `ljmp` 远跳到那个 64 位段——只有这条指令执行完，CPU 才真正开始按 64 位指令流解码
5. **64 位入口 `_start64`**：清一遍段寄存器（长模式下段基本不起作用，但残留的旧值可能触发意外行为），设好 64 位栈指针，`call kernel_main`。
6. **`kernel_main` 返回后**：正常不应该返回，如果返回了，`hlt` 循环兜底，不要让 CPU 掉进随机指令流。
7. 补全 `console_putc.c`（COM1 端口轮询发送）和 `panic_arch.c`（`cli`+`hlt`）。
8. 写 `linker.ld`，加载地址定 `0x100000`（1MiB），这是 x86 传统的"安全"内核加载地址（避开了实模式时代 640KB 以下的各种保留区域）。
9. 写 `grub.cfg`，声明 `multiboot2 /boot/kernel.elf`。

### riscv64 路线

1. **入口 `_start`**：riscv64 到这一步已经是完整的 64 位 S 模式执行环境，不需要任何模式切换，直接设好栈指针（`la sp, stack_top`）就能安全调用 C 函数了——这是和 x86_64 路线最直观的对比：x86 花了 6 步做的事，riscv 这里 0 步，因为硬件设计时就没有历史包袱要兼容。
2. **`call kernel_main`**：调用约定和普通函数调用完全一样。
3. **`kernel_main` 返回后**：`wfi`（等待中断）循环兜底。
4. 补全 `console_putc.c`（UART MMIO 轮询写入）和 `panic_arch.c`（`wfi` 循环）。
5. 写 `linker.ld`，加载地址定 `0x80200000`——这个地址不是你能自由选的，是 OpenSBI 的固定跳转约定，改了这个地址内核就不会被执行到（详见下面"常见坑与排查"）。

## QEMU 运行命令

```bash
# x86_64：先构建出 ISO，再用 -cdrom 引导
make ARCH=x86_64 LAB=lab01-boot-hello run

# riscv64：直接用 -kernel 加载 ELF，OpenSBI 负责跳转
make ARCH=riscv64 LAB=lab01-boot-hello run
```

`make run` 背后调的是 [`scripts/run-qemu.sh`](../../scripts/run-qemu.sh)，等价于手动执行：

```bash
# x86_64
qemu-system-x86_64 -serial stdio -display none -no-reboot -no-shutdown \
  -cdrom labs/lab01-boot-hello/solution/x86_64/build/os.iso

# riscv64
qemu-system-riscv64 -machine virt -bios default -display none -no-reboot \
  -kernel labs/lab01-boot-hello/solution/riscv64/build/kernel.elf -serial mon:stdio
```

两边都应该看到（riscv64 前面会多一大段 OpenSBI 自己的版本横幅，这是正常的，说明固件层先跑完了才轮到你的内核）：

```
Hello OS from x86_64      # 或
Hello OS from riscv64
```

打印完之后进程不会自己退出（`hlt`/`wfi` 循环让 CPU 停在那里等待中断，`-no-reboot`/`-no-shutdown` 防止意外重启），手动 `Ctrl-A X`（QEMU 默认快捷键，退出并终止模拟器）或 `Ctrl-C` 结束。

## GDB/QEMU Monitor 调试方法

本 Lab 的代码量还小，大概率不需要上 GDB，但如果 x86_64 的长模式切换卡住了（最常见的失败模式：三重故障，表现为 QEMU 静默重启或直接退出），GDB 能帮你确认卡在哪一步：

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab01-boot-hello
```

在 GDB 里，长模式切换前的代码还是 32 位，切换完是 64 位，GDB 会自动跟随（用 `x86_64-elf-gdb` 或支持 multiarch 的 `gdb`/`gdb-multiarch`，见 [`docs/debugging.md`](../../docs/debugging.md)）：

```
(gdb) break _start
(gdb) continue
(gdb) stepi                 # 单步一条指令，重点看 mov %cr0/%cr3、rdmsr/wrmsr、lgdt、ljmp 这几条
(gdb) p/x $cr0
(gdb) p/x $cr3
(gdb) p/x $cr4
```

riscv64 这边如果 OpenSBI 横幅打完了但内核没打印任何东西，最可能是入口地址没对上（见下面"常见坑"），用 QEMU Monitor 直接看 PC 停在哪：

```bash
MONITOR=1 bash scripts/run-qemu.sh ARCH=riscv64 LAB=lab01-boot-hello
# 另开一个终端
telnet 127.0.0.1 4444
(qemu) info registers        # 看 pc 的值，和你 kernel.elf 里 _start 的地址对比
```

## 自动验收测试

```bash
make test ARCH=x86_64 LAB=lab01-boot-hello
make test ARCH=riscv64 LAB=lab01-boot-hello
```

背后是 [`scripts/test-lab.sh`](../../scripts/test-lab.sh)：构建 solution，跑进 QEMU（15 秒超时兜底，防止内核卡死导致测试永远不返回），抓取串口输出，和 `tests/expect-x86_64.txt` / `tests/expect-riscv64.txt` 做逐字比对。两边都应该看到 `[PASS]`。

## 常见坑与排查

- **x86_64：QEMU 一启动就重启或直接退出（三重故障）**：几乎总是长模式切换的 6 步顺序出了问题，最常见的是 `CR3` 加载晚于（或没有）`CR0.PG` 开启——开分页那一刻如果 `CR3` 指向的物理地址是垂圾数据，CPU 找不到有效页表，会立刻三重故障。检查 `boot.S` 里 `mov pml4_addr, %cr3` 是否确实在 `or $0x80000000, %eax; mov %eax, %cr0` **之前**执行。
- **x86_64：ISO 构建出来但 QEMU 完全没有任何串口输出，等多久都一样**：这是本课程在 macOS 上踩过的真实坑——如果你用 Homebrew 的 `x86_64-elf-grub` 而不是 `i686-elf-grub` 做 ISO，生成的 ISO 只有 UEFI 引导记录，QEMU 默认的 SeaBIOS（走 legacy BIOS 路径）完全读不出来，会静默转向网络引导，串口上什么都不会打印。这个错误信息只出现在 QEMU 的模拟显示器上，本课程默认 `-display none` 看不到。详细原因和修复方式见 [`docs/environment.md`](../../docs/environment.md) 里专门的踩坑记录，一句话版本：`brew install i686-elf-grub`，本课程的构建脚本已经自动优先选它，不需要手动指定。
- **riscv64：OpenSBI 横幅打完了，之后再没有任何输出**：先确认 `kernel.elf` 的加载地址是不是 `0x80200000`（`readelf -h kernel.elf` 看 `Entry point address`，或者 `nm kernel.elf | grep _start`）。如果你的内核有多个 `.c`/`.S` 文件参与链接，还要确认链接脚本里用 `KEEP(*(.text.boot))` 把入口代码强制排在最前面——多个目标文件的 `.text` 段合并顺序在链接脚本里用普通通配符（`*(.text)`）是不保证跟你的文件顺序一致的，**OpenSBI 是硬编码跳转到 `0x80200000`，完全不读 ELF 的入口字段**，如果这个地址实际落在了别的函数中间（比如某个字符串处理函数的中间一条指令），CPU 会从一条随机指令开始"执行"，通常表现为卡死或者做出完全不可预测的行为，不会有任何报错提示你。本课程的 `solution/riscv64/boot.S`/`linker.ld` 已经用 `.text.boot` 专属段名 + `KEEP()` 规避了这个问题，写 starter 版本时注意保留这个模式。
- **两边都适用：忘记先跑 `make clean`**：如果你改了 `ARCH` 或 `VARIANT` 反复测试，`build/` 目录下的旧产物有时会让人误以为新代码没生效，卡住调试思路时先 `make clean` 排除这个可能性。

更系统的排查流程见 [`docs/debugging.md`](../../docs/debugging.md) 的"排查决策树"一节。

## 挑战任务

- **x86_64**：把 `panic_halt()` 触发时的 `hlt` 循环改成先往串口打印一句"panicked"再停机（提示：这需要 `panic_arch.c` 能访问 `console_putc`，思考一下这个依赖方向是否合理——`panic.c` 现在的分层设计是"架构无关的 panic 逻辑调用架构相关的 `panic_halt`"，如果反过来要求 `panic_halt` 感知"打印"这件事，这层分工还成立吗？Lab2 会重新设计 `panic()` 的完整形态，这里只是让你提前感受这个设计张力）。
- **riscv64**：读一下 `a0`（hartid）和 `a1`（DTB 指针）现在分别是什么值——在 `_start` 里把它们保存到你能在 GDB 里查看的位置（比如某个全局变量，注意这一步需要 `.bss` 已经能安全写入），用 GDB 确认 hartid 是 0（QEMU 默认单核启动时的编号），DTB 指针是一个看起来合理的物理地址。这两个值会在 Lab3（DTB 解析拿可用内存范围）和 Lab10（多核启动，hartid 用来区分不同核心）真正用上。
- **两边通用**：故意把 `linker.ld` 里的加载地址改成一个错误的值，重新构建，观察失败的具体表现（x86_64 大概率是 GRUB 报错或者三重故障；riscv64 大概率是 OpenSBI 跳转到你内核之外的地方，行为不可预测），然后改回来。这个练习的目的不是"制造 bug"，是亲眼见证"加载地址"这个链接脚本里一行不起眼的配置，实际上是整个启动链条能否闭合的关键前提。

## 附录：Limine 引导器方案（x86_64，可选对照）

本课程主线用 Multiboot2 + GRUB，但业界另一个常见选择是 [Limine](https://github.com/limine-bootloader/limine)——一个更现代、更轻量的引导器，不需要你单独安装一整套 GRUB 工具链（Limine 以预编译二进制 + 头文件的形式分发，`git clone` 下来就能用）。它和 Multiboot2 最大的区别是协议设计：Limine 协议基于一组"请求（request）"结构体，内核在自己的数据段里声明"我需要什么信息"（比如内存映射、帧缓冲区），Limine 加载时会填好对应的响应结构体，而不是像 Multiboot2 一样把所有信息打包成一份链表结构一次性丢给你。

这不是本 Lab 要求实现的第二份代码——ROADMAP 里对 Limine 分支的定位是"文档对照，二者任选其一继续后续 Lab"，如果你选择继续用 Limine 而不是 GRUB，后续 Lab（尤其是 Lab3 解析内存映射的部分）需要你自己把"读取 Multiboot2 mmap tag"替换成"读取 Limine 的 memmap request 响应"，两者信息等价，只是数据结构形状不同，OSDev Wiki 的 [Limine Bare Bones](https://wiki.osdev.org/Limine_Bare_Bones) 教程有完整的对应代码可以参考。

如果你只是想直观感受一下差异，Limine 的最小启动流程大致是：

1. 下载 Limine 二进制发行版（不需要编译，也没有 macOS Homebrew formula 的坑）。
2. 内核里用 `#include <limine.h>`（Limine 官方提供的头文件），声明请求结构体，比如：
   ```c
   static volatile struct limine_bootloader_info_request bootloader_info_request = {
       .id = LIMINE_BOOTLOADER_INFO_REQUEST,
       .revision = 0
   };
   ```
3. 链接脚本和 GRUB 路线不同，不需要 Multiboot2 头，但需要按 Limine 文档要求的方式排布可执行文件（较新版本的 Limine 支持直接识别标准 ELF，不强制要求特殊段）。
4. 用 `limine-deploy` 或 Limine 提供的脚本把内核和 Limine 自身的引导文件一起打进一张磁盘镜像或 ISO。

是否要在本课程之外自己动手做一份 Limine 版本的 Lab1，取决于你对"引导协议本身"这个话题的兴趣程度——理解操作系统内核怎么工作，Multiboot2 主线已经完全足够。

## 参考

- OSDev Wiki: [Multiboot](https://wiki.osdev.org/Multiboot)、[Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)、[Bare Bones](https://wiki.osdev.org/Bare_Bones)
- OSDev Wiki: [RISC-V Bare Bones](https://wiki.osdev.org/RISC-V_Bare_Bones)、[SBI](https://wiki.osdev.org/RISC-V_Supervisor_Binary_Interface)
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot2.html)（官方规范，Multiboot2 头字段的权威定义）
- [RISC-V SBI Specification](https://github.com/riscv-non-isa/riscv-sbi-doc)（OpenSBI 的行为依据）
- Rust 分支迁移要点见 [`docs/rust-track.md`](../../docs/rust-track.md) 的"Lab1-2：Boot 与内核入口"一节

## 下一步

进入 [Lab2：内核入口、链接脚本、BSS、栈、panic](../lab02-kernel-entry/README.md)，把本 Lab "手写汇编搭好最小环境"这件事系统化：写一份能控制内核完整内存布局的链接脚本，在跳进 `kernel_main` 之前正确清零 `.bss`，实现一个真正完整的 `panic()`（带文件名和行号）。
