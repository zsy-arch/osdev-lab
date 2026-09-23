# 调试指南

写内核时你没有 `printf` 调试之外的舒适区——一次错误的页表写入可能让整个系统三重故障重启，而不是抛一个带行号的异常。这份文档教你怎么用 QEMU + GDB 把"黑盒重启"变成"能单步调的可观察系统"。

## 分层调试策略

从便宜到贵，按顺序尝试：

1. **串口打印**：最便宜，本课程所有 Lab 默认走这条路。缺点是不能看寄存器/内存实时状态，只能看你主动打印的东西。
2. **QEMU Monitor**：不用改内核代码，随时能看 CPU 寄存器、物理内存、中断状态。
3. **QEMU + GDB remote**：能下断点、单步、看调用栈，需要内核编译时带调试符号（`-g`），这是本课程主线调试方式。
4. **QEMU `-d` 系列 trace 选项**：打印 CPU 执行的每一条指令、每一次异常、每一次中断，信息量最大但也最吵，用于排查"到底是哪一条指令炸了"这种极端情况。

## QEMU Monitor

QEMU Monitor 是 QEMU 自带的调试控制台，本课程的 `scripts/run-qemu.sh` 默认没有开启（为了让 `-serial stdio` 独占终端），需要显式加 `-monitor` 参数或用 telnet 连接。

在 `run-qemu.sh` 里传 `MONITOR=1` 会切换为：

```bash
MONITOR=1 bash scripts/run-qemu.sh ARCH=x86_64 LAB=lab03
```

这会把 monitor 挂在 `telnet:127.0.0.1:4444`，串口仍然走 stdio。另开一个终端：

```bash
telnet 127.0.0.1 4444
```

常用 Monitor 命令：

```
info registers      # 看当前 CPU 寄存器
info mem             # 看页表映射（需要 guest 已经开启分页）
info tlb             # 看 TLB 内容（x86）
x/10i $pc            # 从当前 PC 开始反汇编 10 条指令
xp /16xb 0x1000      # 以十六进制查看物理地址 0x1000 开始的 16 字节
system_reset         # 软重启 guest
```

`info mem`/`info tlb` 在 Lab4（虚拟内存）调试页表映射错误时特别有用——比起在 GDB 里手动算页表项地址，直接问 QEMU"现在的映射是什么"更快。

## GDB + QEMU remote（主线调试方式）

原理：QEMU 加 `-s -S` 参数，`-S` 让虚拟机启动后立刻挂起不执行，`-s` 是 `-gdb tcp::1234` 的简写，开一个 GDB remote stub 监听 1234 端口。GDB 连上去之后，你对"客户机 CPU"的控制和调试本地进程几乎一样：`break`、`continue`、`step`、`print`、`bt` 全部可用。

### x86_64

```bash
# 终端 1：启动 QEMU 并挂起等待调试器
qemu-system-x86_64 -cdrom build/x86_64/os.iso -serial stdio -display none -s -S

# 终端 2：连接调试器
gdb build/x86_64/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

macOS 原生工具链下，如果你装的是 `x86_64-elf-gdb`：

```bash
x86_64-elf-gdb build/x86_64/kernel.elf
(gdb) target remote localhost:1234
```

用系统自带 `lldb` 也能连 QEMU 的 gdbstub（协议兼容），但命令语法不同（`gdb-remote localhost:1234`），本课程文档统一用 GDB 命令，如果你偏好 lldb，自行按 [LLDB 命令映射表](https://lldb.llvm.org/use/map.html) 转换。

### riscv64

```bash
# 终端 1
qemu-system-riscv64 -machine virt -bios default -kernel build/riscv64/kernel.elf \
  -serial mon:stdio -display none -s -S

# 终端 2
riscv64-elf-gdb build/riscv64/kernel.elf     # macOS Homebrew
# 或
gdb-multiarch build/riscv64/kernel.elf        # Linux/Docker
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

`-bios default` 让 QEMU 自动加载内置的 OpenSBI 固件；调试时你会先停在 OpenSBI 的地址（因为 `-S` 是从 CPU 复位状态就挂起的，比你的内核入口更早），先 `continue` 一次让 OpenSBI 跑完初始化再进你的内核断点，或者直接在你的内核入口地址（比如 `_start` 或 `kernel_main`）下断点，`continue` 会自动跑过 OpenSBI 停在那里。

macOS 上没有 `gdb-multiarch` 这个包名，用 `riscv64-elf-gdb`（Homebrew）功能等价。Docker 镜像和 Linux 原生按本课程约定统一提供 `gdb-multiarch`（同时支持两种架构，切换用 `set architecture`），文档里两种命令都会给出。

脚本化版本：

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab05
bash scripts/debug-gdb.sh ARCH=riscv64 LAB=lab05
```

这个脚本会自动起 QEMU（后台）、等端口就绪、拉起对应架构的 GDB 并 `target remote`，还会 source 一份 `.gdbinit` 片段，预置几个常用宏（比如打印当前页表、打印 trap frame）。

### 常用 GDB 会话技巧

```
(gdb) info registers          # 全部通用寄存器
(gdb) p/x $rip                # x86：当前指令指针（riscv 对应 $pc）
(gdb) p/x $cr3                # x86：当前页表基址（riscv 对应 $satp，但 gdb 未必认识这个别名，用 info registers 找）
(gdb) x/20i $pc                # 反汇编查看当前位置附近指令
(gdb) bt                       # 调用栈回溯（需要 -fno-omit-frame-pointer 编译）
(gdb) watch *(uint64_t*)0x1000 # 硬件观察点，某内存地址被写入时中断
```

**调用栈回溯在裸机内核里经常"看起来是对的但其实是错的"**：GDB 的 `bt` 依赖帧指针链或 DWARF 调试信息，如果你的编译选项里有 `-fomit-frame-pointer`（某些优化级别默认开启）又没有生成足够的 DWARF 信息，`bt` 会给出无意义的结果。本课程所有 Lab 的 `CFLAGS` 默认包含 `-g -fno-omit-frame-pointer -O0`，保证 `bt` 可信；如果你自己改了优化级别，注意这个坑。

## `-d` 系列 trace 选项（重武器）

当你怀疑"某条具体指令导致了三重故障"，又不知道是哪一条时：

```bash
qemu-system-x86_64 ... -d int,cpu_reset -D qemu.log
```

`-d int` 记录每一次中断/异常（包括触发它的原因），`-d cpu_reset` 记录 CPU 复位事件，`-D qemu.log` 把这些不写到终端，写到文件里（否则会和串口输出混在一起没法看）。

**这里有一个必须知道的陷阱：`-no-reboot` 会让 `-d cpu_reset` 查不到三重故障。** 三重故障在 QEMU 里是一次"复位请求"，而 `-no-reboot` 的语义是"收到复位请求就停机，不要重启"——于是 CPU 静静地停住，复位日志里一条都不会多，和健康内核的日志完全一样。本课程的标准运行命令**默认带 `-no-reboot`**（上面那行的 `...` 里就有），所以直接往后面加 `-d cpu_reset` 查到的"干净"是**假阴性**。

两种真正有效的判据：**去掉** `-no-reboot` 然后数启动横幅重复了几次（健康 = 1 次；另外注意健康启动本来就有 2 次 `CPU Reset`，判据是"多于 2 次"），或者**保留** `-no-reboot` 改用 `-d int` 并 grep `check_exception old: 0x8`。完整的判据、基线数据和实测对照表见 [`docs/verification-methodology.md`](verification-methodology.md)。

顺带一个容易误判的点：`-d int` 日志里的 `Servicing hardware INT=0x0e` / `INT=0x08` **不是**你的内核异常，是固件实模式 BIOS 调用，健康日志里条数一样多。真正的内核异常只出现在带 `v=..` / `cpl=..` 字段的行里。

更细粒度的 `-d in_asm`（记录每条被翻译执行的客户机指令）信息量极大，几秒钟能生成几十 MB 日志，只在其它方法都失败、你要做"最后一条正确执行的指令是什么"这种考古时才用。完整选项列表：`qemu-system-x86_64 -d help`。

## 图形窗口（可选）

本课程默认 `-display none`，如果某个挑战任务想看 VGA 图形输出（比如 Lab2 的挑战任务提到 VGA 文本模式），去掉 `-display none` 即可弹出 QEMU 窗口。Docker 容器内默认没有配置 X11 转发，如果你在容器里跑图形模式，需要额外挂载 X11 socket 或用 VNC（`-display vnc=:0` 然后用 VNC 客户端连接），这超出本课程范围，按需自行查阅 Docker + X11 转发相关资料。

## 排查决策树

```
QEMU 直接闪退 / 报 "could not load kernel"
  → 检查 ELF 是否是正确架构（file build/.../kernel.elf）
  → 检查链接脚本入口地址和 QEMU/固件期望的入口是否匹配

QEMU 起来了但没有任何串口输出
  → 用 QEMU Monitor `info registers` 看 PC 是否卡在某个固定地址（可能是死循环）
  → 检查串口驱动的端口/地址是否配对（x86 0x3F8 vs riscv MMIO 0x10000000，见 arch-compare.md）
  → 用 -d int 看是否在到达你的打印代码之前就已经异常

打印了一部分然后卡死或重启
  → GDB attach，在最后一行成功打印之后的代码路径设断点，单步走
  → 怀疑栈溢出：检查栈指针是否越过了链接脚本里分配的栈区域
  → 怀疑页表错误（Lab4 之后）：QEMU Monitor `info mem` 核对映射是否符合预期

系统调用/用户态切换后卡死（Lab6 之后）
  → 确认 trap frame 保存/恢复的寄存器数量和顺序完全对称
  → 用 GDB 在 trap 入口和返回用户态之前分别打印寄存器，比较是否符合预期
  → 检查特权级切换相关寄存器（x86 的 MSR、riscv 的 sstatus.SPP）是否设置正确

串口输出"完全正常"——期望的每一行都打全了
  → 这也可能是崩溃：内核在打完最后一行之后死掉，串口上看不出区别
  → 去掉 -no-reboot 重跑，数启动横幅出现了几次（>1 次 = 重启循环）
  → 判据细节和实测对照表见 verification-methodology.md
```
