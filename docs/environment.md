# 环境配置

两条路：**Docker（推荐）** 或 **原生工具链**。Docker 保证你和课程作者用的是完全一样的工具版本，出问题时排查起来不会怀疑是版本差异。原生工具链更快（没有容器开销），但三个平台（Linux/macOS/WSL2）装法都不一样，且版本漂移会导致偶发的、难排查的差异。

如果你是第一次做，用 Docker。等你确认整套流程能跑通了，再考虑要不要换成原生工具链提速。

## 方式一：Docker（推荐）

只需要装好 Docker Desktop（macOS/Windows）或 Docker Engine（Linux）。

```bash
cd osdev-lab
make setup          # 首次会构建镜像，之后是快速检查
make run ARCH=x86_64
```

`make` 会在需要跑构建/运行/调试类目标时自动把当前目录挂载进容器执行。你不需要手动 `docker run`。

如果你想直接进容器手动敲命令：

```bash
docker build -t osdev-lab:latest -f docker/Dockerfile .
docker run --rm -it -v "$PWD:/workspace" -w /workspace osdev-lab:latest bash
```

**GUI 需求**：本课程默认所有 QEMU 都用 `-display none -serial stdio`（串口输出到终端），不需要图形界面，因此 Docker 容器不需要额外配置 X11/VNC。如果你想用 QEMU 图形窗口调试（比如后面看 VGA 输出），需要额外配置，见 [`debugging.md`](debugging.md) 的"图形窗口"一节。

## 方式二：原生工具链

### Linux（以 Ubuntu 22.04+ / Debian 12+ 为例）

```bash
sudo apt update
sudo apt install -y \
  build-essential clang lld nasm \
  qemu-system-x86 qemu-system-misc \
  grub-pc-bin grub-common xorriso mtools \
  gdb-multiarch \
  gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu
```

Ubuntu/Debian 官方仓库里没有独立的 `riscv64-unknown-elf-gcc`，`gcc-riscv64-linux-gnu` 对本课程用的裸机代码够用（我们不依赖 glibc，只用交叉编译器和 binutils）。如果你想要更"纯粹"的裸机交叉工具链，可以从 [SiFive 预编译版](https://github.com/sifive/freedom-tools/releases) 或自己用 `crosstool-ng` 构建 `riscv64-unknown-elf-`。

x86_64 侧，**Linux 原生环境**下本课程默认用系统自带的 `gcc`/`clang` + `-ffreestanding` 编译，不需要额外的 `x86_64-elf-gcc` 交叉编译器——因为 Linux 上系统自带的 `as`/`ld` 就是真正的 GNU binutils，原生就认识本课程用的 GNU AT&T 汇编语法和链接脚本 `-T` 选项。**这一条对 macOS 不成立**，见下一小节。

### macOS（Apple Silicon 或 Intel，Homebrew）

**macOS 上必须用交叉工具链，没有"直接用系统 gcc 凑合"的选项**，原因比"默认目标架构不对"更底层：macOS 的 `/usr/bin/gcc` 实际是 Apple Clang 的别名，它调用的 `as`（汇编器）和 `ld`（链接器）都是 Apple/LLVM 自己的实现，**不是** GNU binutils。本课程的 `.S` 文件用 GNU AT&T 汇编语法（`.section`、`.code32`、`$imm` 立即数前缀等指令级语法），链接时依赖 GNU `ld` 的 `-T <script>` 加载自定义链接脚本——这两者 Apple 的 `as`/`ld` 都不支持，即使你手动指定 `-m64`/`-ffreestanding` 也无法绕过，因为问题不在"目标架构"，而在"汇编器/链接器认识的指令和参数集合"完全是两套。实测：在 macOS 上直接拿 `/usr/bin/gcc` 汇编本课程的 `.S` 文件会报 `unknown directive`/`unexpected token`，`/usr/bin/ld -T` 会报 `unknown options: -T`。

解决方式是用下面的交叉工具链——它们不只是换了目标三元组（target triple），而是**打包了自己的 GNU binutils**（`x86_64-elf-as`、`x86_64-elf-ld` 等），从根上解决语法不兼容问题：

```bash
brew install x86_64-elf-gcc x86_64-elf-grub i686-elf-grub riscv64-elf-gcc riscv64-elf-gdb
brew install qemu xorriso mtools gdb coreutils
```

已确认（2026-09）以下工具在 Homebrew 官方 core 中可直接安装，无需额外 tap：

| 工具 | 用途 |
|---|---|
| `x86_64-elf-gcc` | x86_64 裸机交叉编译器（GCC 16.x），依赖并自动带入 `x86_64-elf-binutils`（提供 `x86_64-elf-as`/`x86_64-elf-ld`，真正的 GNU 汇编器/链接器） |
| `i686-elf-grub` | **实际用来制作可启动 ISO 的那个 GRUB**（命令名是 `i686-elf-grub-mkrescue`）。见下面单独的踩坑说明，不要装 `x86_64-elf-grub` 就以为够了。 |
| `x86_64-elf-grub` | 只是为了装 `grub-file`（校验 Multiboot2 header 用），**不要**用它的 `x86_64-elf-grub-mkrescue` 做 ISO——见下方说明。 |
| `riscv64-elf-gcc` | riscv64 裸机交叉编译器，同理依赖 `riscv64-elf-binutils` |
| `riscv64-elf-gdb` | riscv64 专用 GDB（也可以用下面的 `gdb` + `gdb-multiarch` 思路，但 macOS 上没有 `gdb-multiarch` 这个包名，用架构专属的 gdb 更省事） |
| `qemu` | 提供 `qemu-system-x86_64` 与 `qemu-system-riscv64` |
| `xorriso`、`mtools` | `grub-mkrescue` 的依赖，生成 ISO 用 |
| `coreutils` | 提供 `gtimeout`，`make test` 靠它给 QEMU 施加超时（见下面单独的踩坑说明）。macOS 默认没有 `timeout` 这个命令。 |

**踩坑记录：`x86_64-elf-grub-mkrescue` 做出来的 ISO 在 QEMU 默认的 SeaBIOS 下完全启动不了。** 现象是 `qemu-system-x86_64 -cdrom os.iso` 之后串口上什么都不会打印，等多久都一样，看起来像是内核代码没跑起来——但其实内核代码根本没被加载。真正原因是 Homebrew 的 `x86_64-elf-grub` 这个 formula 只编译了 GRUB 的 `x86_64-efi` 平台目标，完全没打包 `i386-pc`（也就是 legacy BIOS）平台需要的 `eltorito.img`/`boot_hybrid.img`。用它的 `grub-mkrescue` 生成的 ISO，El Torito 引导目录里只有一条 UEFI 记录（用 `xorriso -indev os.iso -report_el_torito plain` 可以直接看到），SeaBIOS 完全读不出来，会报 `Boot failed: Could not read from CDROM (code 0009)` 然后掉进 iPXE 网络引导——这段错误只会打在 QEMU 的模拟显示器上，`-serial`/`-display none` 的组合永远看不到，得用 `-vnc` 接显示器截图才能看见，非常容易误判成自己内核代码的问题。

解决方式是另装 `i686-elf-grub`（命名习惯：`i686-elf-grub` 对应经典 32 位 `i386-pc`/legacy BIOS 平台，`x86_64-elf-grub` 只对应 `x86_64-efi`），做 ISO 时用 **`i686-elf-grub-mkrescue`**，不要用 `x86_64-elf-grub-mkrescue`。`grub.cfg`/`isodir` 目录结构完全一样，只是换一个 mkrescue 二进制。本课程的 `src/common/toolchain.mk` 在探测 `GRUB_MKRESCUE` 时已经把 `i686-elf-grub-mkrescue` 排在最前面，你不需要手动指定。

**踩坑记录：没装 `coreutils` 时 `make test` 会真的卡死，不是变慢。** `scripts/run-qemu.sh` 用 `-serial stdio` 启动 QEMU 并把内核的串口输出接到当前终端，这个进程本身不会因为"内核打印完了"就自动退出——它得靠外面再包一层 `timeout <秒数> qemu-system-...` 才会在规定时间内被杀掉。GNU coreutils 的 `timeout` 命令在 Linux 上几乎总是自带，但 macOS 默认一个都没有（系统自带的不叫这个名字）。如果这时候内核代码本身还有别的问题导致没打印出期望字符串，`make test` 会真的卡在那儿不返回——不是等待变长，是永远不会结束，很容易被误判成"QEMU 卡住了"或者"内核死循环了"，其实只是缺一层超时包装。装了 `coreutils` 之后这个命令会以 `gtimeout` 的名字出现在 `PATH` 里（避免跟系统内置命令撞名），`run-qemu.sh` 已经优先探测 `timeout`、找不到再退到 `gtimeout`，两个都没有会直接报错退出而不是静默地不做超时。

`x86_64-elf-gdb` 也可以装（`brew install x86_64-elf-gdb`），但一般装一个 `riscv64-elf-gdb` 或系统 `gdb`（若支持 multiarch）就够两个架构调试用；具体见 [`debugging.md`](debugging.md)。

Apple Silicon 上的 Homebrew 默认前缀是 `/opt/homebrew`，Intel Mac 是 `/usr/local`；工具都会进 `PATH`，正常情况下装完直接能用。用 `which x86_64-elf-gcc` 确认。

**macOS 上没有 Limine/GRUB 官方 formula 覆盖到全部我们需要的功能时**，Lab1 文档里给的 Limine 分支是更轻量的备选（Limine 本身是可下载的预编译 bootloader + 头文件，不需要单独装编译器）。

### WSL2（Windows）

在 WSL2 里的 Ubuntu 发行版，直接按上面"Linux"小节操作即可，WSL2 内核对 QEMU/KVM 支持良好。两个要注意的点：

1. **图形/串口输出**：本课程默认 `-display none -serial stdio`，直接在 WSL2 终端里能看到输出，不需要装 X server。
2. **网络代理**：如果你的机器有企业代理，`apt install` 或 Docker 拉镜像可能需要额外配置代理环境变量（`http_proxy`/`https_proxy`），这个和 WSL2 本身无关，是网络环境问题。

WSL1 不支持本课程（缺少完整的系统调用兼容性和 KVM 加速），请确认 `wsl -l -v` 显示的版本是 2。

### Rust（可选，仅 Rust 分支需要）

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add x86_64-unknown-none
rustup target add riscv64gc-unknown-none-elf
rustup component add llvm-tools-preview
```

## 验证安装

跑一次环境检查脚本，它会打印每个工具的版本，缺失的会用醒目的方式标出来（不会静默跳过）：

```bash
bash scripts/check-env.sh
```

期望看到类似：

```
[OK]   qemu-system-x86_64   -> QEMU emulator version 11.0.1
[OK]   qemu-system-riscv64  -> QEMU emulator version 11.0.1
[OK]   x86_64-elf-gcc       -> gcc (Homebrew GCC 16.2.0) 16.2.0
[OK]   riscv64-elf-gcc      -> riscv64-elf-gcc (Homebrew GCC 16.2.0) 16.2.0
[OK]   gdb                  -> GNU gdb (GDB) 15.x
[MISS] rustc                -> not found (仅 Rust 分支需要，可忽略)
```

`[MISS]` 且你确定不需要该工具（比如不打算做 Rust 分支）可以忽略；如果是主线必需工具缺失，脚本会给出对应平台的安装建议。

## 环境变量与工具链前缀

`Makefile` 通过 `ARCH` 变量决定用哪套工具链前缀，规则：

| ARCH | Linux / Docker | macOS |
|---|---|---|
| `x86_64` | 系统 `gcc`/`clang` + `-ffreestanding -m64`（系统 `as`/`ld` 就是 GNU binutils，直接能用） | 必须用 `x86_64-elf-gcc`（连带其 `x86_64-elf-as`/`x86_64-elf-ld`），系统 `gcc`/`ld` **不能**汇编/链接本课程的 `.S`/`.ld` 文件 |
| `riscv64` | `riscv64-linux-gnu-gcc`（或 `riscv64-elf-gcc`，如果你另外装了） | 必须用 `riscv64-elf-gcc`（连带其 `riscv64-elf-as`/`riscv64-elf-ld`） |

`Makefile` 里每个 Lab 的构建规则会按下面的顺序自动探测可用编译器：

- **x86_64**：`x86_64-elf-gcc` → 系统 `gcc`（用 `gcc -dumpmachine` 判断输出是否形如 `*-linux-*`，即 Linux 上的原生 gcc；如果输出形如 `*-apple-darwin*`，说明是 macOS 的 Apple Clang，即使叫 `gcc` 也不会被选用，直接报错并提示装 `x86_64-elf-gcc`）→ 系统 `clang`（同样先判断 target）
- **riscv64**：`riscv64-elf-gcc` → `riscv64-unknown-elf-gcc` → `riscv64-linux-gnu-gcc`（在 macOS 上這三个都不存在时会直接报错，不会静默回退到任何本机工具，因为 riscv64 没有"本机工具链"这个选项）

这个探测逻辑封装在 [`src/common/toolchain.mk`](../src/common/toolchain.mk) 里（Lab1 开始引入，Lab0 阶段还不需要关心细节），每个 Lab 的 `Makefile` 都 `include` 它。

如果自动探测选错了工具链，可以显式覆盖：

```bash
make run ARCH=x86_64 CROSS_X86_64=x86_64-elf-
make run ARCH=riscv64 CROSS_RISCV64=riscv64-linux-gnu-
```
