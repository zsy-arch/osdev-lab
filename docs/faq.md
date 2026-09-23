# 常见问题

## 环境相关

**Q: macOS 上 `gcc` 编译出来的东西能直接当内核用吗？**

不能，而且比"目标架构不对"更严重。macOS 的 `/usr/bin/gcc` 其实是 Apple Clang 的别名（用 `gcc --version` 能看到实际打的是 `Apple clang version ...`），它背后调用的 `as`（汇编器）和 `ld`（链接器）都是 Apple/LLVM 自己的实现，不是 GNU binutils。本课程的 `.S` 文件用 GNU AT&T 汇编语法（`.section`、`.code32`、`.global` 等指令），链接脚本用 GNU `ld` 的 `-T` 语法，这些 Apple 的 `as`/`ld` 完全不认识——不是"目标架构不匹配导致运行不了"，而是**编译/链接阶段直接报语法错误**（实测报 `unknown directive`、`unknown options: -T` 这类错误），连产物都生成不出来。加 `-ffreestanding -m64` 之类的选项也无法绕过，因为问题不在于目标三元组，而在于汇编器/链接器认识的指令集合是两套不同的东西。

解决方式是用 `docs/environment.md` 里的 `x86_64-elf-gcc`/`riscv64-elf-gcc`（Homebrew 直接可装）。它们各自依赖并自动带入 `x86_64-elf-binutils`/`riscv64-elf-binutils`，这两个包里的 `x86_64-elf-as`/`x86_64-elf-ld` 才是真正的 GNU 工具，能正确处理本课程的汇编文件和链接脚本。**Linux 上不存在这个问题**：Linux 系统自带的 `as`/`ld` 本身就是 GNU binutils，所以 Linux/Docker 环境下 x86_64 侧可以直接用系统 `gcc`，不强制要求装 `x86_64-elf-gcc`。

**Q: `make setup` 卡在下载 Docker 镜像不动？**

大概率是网络问题，不是脚本问题。可以：
```bash
docker pull ubuntu:22.04     # 单独测试能不能拉基础镜像
```
如果连基础镜像都拉不动，检查代理设置（`~/.docker/config.json` 或环境变量 `HTTP_PROXY`/`HTTPS_PROXY`）。国内网络环境下，可以在 `docker/Dockerfile` 顶部按注释提示替换 apt 源为国内镜像（阿里云/清华源），不改变其它任何行为。

**Q: WSL2 里 QEMU 特别慢？**

检查是否启用了 KVM 加速：
```bash
ls -la /dev/kvm
```
如果不存在，WSL2 的虚拟化嵌套没开。在 Windows 的"启用或关闭 Windows 功能"里确认"适用于 Linux 的 Windows 子系统"和"虚拟机平台"都勾选，且 BIOS 里虚拟化技术（Intel VT-x/AMD-V）已开启。没有 KVM 加速时 QEMU 会退化为纯软件模拟（TCG），慢但仍然能用，本课程所有 Lab 都不依赖 KVM 加速，只是速度体验会差一些。

**Q: 用 `riscv64-linux-gnu-gcc` 而不是 `riscv64-elf-gcc`/`riscv64-unknown-elf-gcc` 编译裸机代码，会有什么坑？**

`riscv64-linux-gnu-gcc` 默认假设目标是 Linux 用户态程序（会链接 glibc 相关的启动代码），裸机场景下必须加 `-nostdlib -nostartfiles -static` 之类的选项手动排除这些假设，本课程的 `Makefile` 已经按 ARCH 自动配置好这些选项，你不需要手动处理，但如果你在 Lab 之外自己写测试代码，要留意这一点。

## 构建相关

**Q: 编译报 `undefined reference to __stack_chk_fail` 之类的符号？**

编译器默认可能开启了栈保护（stack protector）或其它依赖运行时支持库的特性，裸机环境没有这些库。检查 `CFLAGS` 是否包含 `-fno-stack-protector`；本课程 `Makefile` 默认已经加了，如果你新建了自己的编译规则要记得带上。

**Q: 链接报找不到某个 section 或者内核加载后地址不对？**

大概率是链接脚本（`.ld` 文件）里的 `. = ADDR` 或 `AT>` 之类的定位没设对，或者你的代码里假设的加载地址和链接脚本不一致。见 Lab2 的"常见坑"一节，那是专门讲链接脚本的课。

## QEMU/调试相关

见 [`debugging.md`](debugging.md) 末尾的排查决策树，覆盖了"QEMU 闪退""没有输出""打印一半卡死""系统调用后卡死"四类最常见情况的排查步骤。这里补充几个没在那份文档里的点：

**Q: `-serial stdio` 之后，Ctrl+C 退不出 QEMU？**

`-serial stdio` 会让终端进入一种模式，普通 Ctrl+C 可能被 QEMU 截获当成串口输入发给 guest。用 QEMU 自己的快捷键 `Ctrl+A X`（先按住 Ctrl+A 松开，再按 X）退出，或者另开一个终端 `pkill qemu-system-x86_64`。本课程脚本 `scripts/run-qemu.sh` 内部对超时测试场景用的是 `timeout` 命令包一层，正常情况下测试脚本会自动结束进程，不需要你手动杀。

**Q: GDB `target remote localhost:1234` 连接被拒绝？**

确认 QEMU 是不是带了 `-s`（等价于 `-gdb tcp::1234`）参数启动，以及有没有别的进程占用了 1234 端口（`lsof -i :1234` 查一下）。如果端口冲突，QEMU 会静默选择失败或者用别的行为，取决于版本，最保险的做法是先 `pkill qemu` 清一遍再重试。

**Q: 为什么有的 Lab 断点下在函数名上没反应？**

如果该函数被编译器 inline 了，函数名对应的符号可能不存在或者地址和你想的不一样。本课程默认 `-O0`（不优化）避免这个问题；如果你调高了优化级别去测试性能相关话题，记得断点可能需要改用地址而不是函数名。

## 课程内容相关

**Q: 为什么不直接用现成的 xv6 代码，非要重新实现一遍？**

xv6 是给 MIT 6.S081 课堂用的，默认假设你已经在配套讲座里听过大量背景知识，代码本身注释稀疏、教学脚手架（比如详细的中文对照说明、QEMU 自动化测试）几乎没有。本课程的目标是"自学也能跟下去"，所以按相同的核心思路重新设计了一套更小、注释更完整、双架构对照、带自动测试的实现，方便你在读懂之后回头对照原版 xv6/rCore 代码，理解生产级实现和教学最小实现之间省略了什么。

**Q: 一定要两个架构都做吗？只做 x86_64（或只做 riscv64）可以吗？**

可以只做一个架构完成课程主线，每个 Lab 的 starter/solution 都是按架构独立组织的，`make run ARCH=x86_64` 不会强制你也跑 riscv64。但强烈建议至少把 Lab4（虚拟内存）和 Lab6（系统调用）两个架构都做一遍——这两个 Lab 的架构差异最大，只看一个架构很容易把"这个架构的实现细节"错误地理解成"操作系统原理本身"，双架构对照是本课程设计里最有教学价值的部分。

**Q: 我卡在某个 Lab 好几天了，直接抄 solution 行不行？**

短期内可以先看 solution 理解思路，但建议之后关掉它自己重写一遍——OS 开发的很多坑（栈对齐、页表项算错一位、trap frame 顺序不对）只有自己踩过一次才会真正记住，单纯读懂 solution 的解释不会带来同等的debugging 直觉。如果卡住的原因是某个前置概念没搞懂（而不是纯粹的代码细节），回头检查是不是前置 Lab 的某个环节理解有误。
