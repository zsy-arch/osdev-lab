# Lab2：内核入口、链接脚本、BSS、栈、panic

## 学习目标

- 理解链接脚本（linker script）不只是"告诉链接器把文件放哪"，而是内核对自己内存布局的唯一权威声明——`.bss` 在哪、多大，栈在哪、多大，这些问题的答案都应该来自链接脚本，不是散落在某个 `.S` 文件里的魔法数字。
- 亲手证明一件大多数教程一笔带过的事：ELF 的 `.bss` 段本身不保证被清零，"没有显式初始化的静态变量等于 0"这个 C 语言保证，在裸机环境下必须由内核自己的代码去满足。
- 把 Lab1 里 `.S` 文件自己 `.skip` 出来的栈空间，改成由链接脚本定义的符号（`__stack_bottom`/`__stack_top`），建立"内存布局归链接脚本管，指令逻辑归汇编代码管"的分工习惯——这个习惯会在 Lab10 引入每核心独立栈时再次用到。
- 第一次真正调用 `panic()`：验证它能正确打印触发位置的文件名、行号和消息，然后安全停机。这条诊断路径会贯穿本课程剩下的所有 Lab。

## 前置 Lab

[Lab1：Boot 与裸机输出](../lab01-boot-hello/README.md)。本 Lab 直接在 Lab1 的长模式切换（x86_64）/ S 模式入口（riscv64）代码基础上修改，如果 Lab1 还没跑通，先回去解决。

## 核心概念

**`.bss` 段清零不是加载器欠你的**：ELF 规范里 `.bss` 对应的 section 类型是 `SHT_NOBITS`——意思是"这段数据在文件里不占字节，只是声明了运行时需要这么多空间"。规范本身没有规定加载器必须把这段空间清零，GRUB、QEMU 的 ELF 加载器这么做都只是实现上的惯例，不是你能依赖的契约。Lab1 的代码能正常工作，纯粹是因为它没有任何依赖"静态变量初值是 0"这个保证的代码路径——一旦有（本 Lab 就故意加了一个），不自己清零就是一个安静的、极难调试的 bug：变量的值取决于内存里的历史残留，可能在你的机器上恰好是 0（因为 QEMU 一般给新分配的内存清零），换一台机器、换一个 QEMU 版本就不一定。

**链接脚本定义的符号是"地址标记"，不是变量**：`linker.ld` 里 `__bss_start = .;` 这一行不是在声明一个占内存的对象，它只是把链接时的当前地址值绑定到这个符号名上。链接完成后，`__bss_start` 就是一个具体的常量地址，在 `.S` 文件里可以直接当立即数用（`mov $__bss_start, %edi`）；如果需要在 C 代码里用，要声明成 `extern char __bss_start[];`（数组类型，取地址就是取符号本身的地址，不是"读取符号处的内容"）。本 Lab 只在汇编里用到这几个符号，C 代码不需要引用它们。

**为什么栈要挪到链接脚本，不留在 `boot.S` 的 `.skip`**：Lab1 的写法（`.section .bss; .skip 16384; stack_top:`）能用，但栈的大小、对齐方式这些"内存布局决策"和"怎么把这块内存的地址装进 `%esp`"这两件事被绑在了同一个文件里，架构专属的汇编文件因此背负了本不该属于它的职责。本 Lab 把栈的空间声明挪进 `linker.ld`（和 `.bss` 一样，只是换了个位置、给了独立的符号名），`boot.S` 只负责"用" `__stack_top` 这个地址，不管它具体落在内存的什么位置。这个分工在当前阶段只是代码整洁问题，但从 Lab10（对称多处理，每个 CPU 核心需要独立的栈）开始，会变成一个必须解决的实际问题——链接脚本能用循环或数组符号一次性声明 N 份栈空间，比在汇编里手写 N 段 `.skip` 更不容易出错。

**段权限合并是一个真实存在、容易被忽视的坑**：链接器在生成最终可执行文件时，会把连续的、没有对齐边界隔开的输出 section 合并进同一个 PT_LOAD 段（program header 里描述的"一整块需要被加载器映射的内存区域"），合并后的权限位是所有被合并 section 权限的按位或。`.text` 需要可执行（E），`.bss` 需要可写（W）——如果两者之间没有显式的分页对齐边界，链接器可能把它们合并进同一个段，得到的结果是同时可写又可执行（RWE/RWX）。这不是危言耸听的理论场景，本 Lab 在设计阶段真实踩到过这个坑，具体经过见下面"踩坑记录"一节。RWX 段是安全领域里一类具体攻击手法（比如往可写内存里注入代码再让 CPU 当指令执行）的必要前提，NX（No-Execute）、W^X（Write XOR Execute，要求任何一段内存不能同时可写和可执行）这类保护机制存在的意义就是消灭这种段。写内核链接脚本时应该养成习惯：任何权限需求不同的输出 section 之间都显式加一条 `. = ALIGN(4096);`，不依赖"链接器这次恰好没合并"的运气。

**`panic()` 为什么要设计成宏而不是普通函数**：[`src/common/include/panic.h`](../../src/common/include/panic.h) 里 `panic(msg)` 展开成 `kernel_panic(__FILE__, __LINE__, (msg))`——`__FILE__`/`__LINE__` 是预处理器在**调用点**展开的，如果把这两个参数的填充放进 `kernel_panic()` 函数体内部，得到的永远是 `panic.c` 自己的文件名和行号，不是真正触发 panic 的那一行代码。宏在这里不是"图省事"，是唯一能让"报错信息指向真正出错位置"这件事成立的机制。

## x86_64 与 riscv64 对照表

| 主题 | x86_64 | riscv64 |
|---|---|---|
| 跳进 C 代码前必须做的新增步骤 | 设 `%esp`（复用链接脚本符号）→ 清零 BSS | 清零 BSS →设 `sp`（复用链接脚本符号） |
| BSS 清零循环怎么写 | 32 位模式下用 `%edi`/`%ecx` 走字节循环，`movb $0, (%edi)` | 用 `t0`/`t1` 走字节循环，`sb zero, 0(t0)`（`zero` 是 riscv 里硬编码为常量 0 的寄存器，`x0`） |
| 栈对齐要求 | 16 字节对齐（System V AMD64 ABI 要求 `call` 指令执行时栈已经 16 字节对齐） | 16 字节对齐（riscv 调用约定同样要求，虽然 riscv 整数寄存器宽度按 8 字节自然对齐就够，但 ABI 显式要求 16） |
| 链接脚本里两次用到 `ALIGN` 的原因 | 一次是 `__bss_start` 前的 `ALIGN(4096)`（段权限分离），一次是 `__stack_bottom` 前的 `ALIGN(16)`（栈对齐要求） | 同左，两个架构的原因完全一致，这一点没有分叉 |
| 本 Lab 新增代码量 | `boot.S` 多出约 10 行（清零循环），`linker.ld` 多出 6 行符号 | `boot.S` 多出约 6 行（清零循环，比 x86_64 短是因为不需要在 32/64 位模式各设一次栈），`linker.ld` 多出 6 行符号 |

**这次两边的差异比 Lab1 小得多**：Lab1 的核心教学点（模式切换步骤数量的巨大差异）在这一层已经讲完了，本 Lab 的新内容——BSS 清零、栈符号——在两边是同一件事，只是指令集的具体写法不同（`mov`/`inc`/`dec` 对 `la`/`addi`/`bge`），不涉及任何架构级的设计哲学分叉。这本身也是一个值得记住的信号：不是所有 Lab 都会在架构对照表里挖出深刻差异，有些内容就是"两边一样重要，语法不同而已"，[`docs/arch-compare.md`](../../docs/arch-compare.md) 的全局索引也没有专门为本 Lab 开一行，就是这个原因。

## 代码目录与关键文件

```
labs/lab02-kernel-entry/
  README.md                  本文件
  Makefile                    ARCH=x86_64|riscv64 VARIANT=solution|starter 通用构建入口
  starter/
    x86_64/                   带 TODO 的骨架：缺 BSS 清零循环、__stack_top 的使用、kernel_main 里的验证与 panic 调用
    riscv64/                  带 TODO 的骨架：缺 BSS 清零循环、__stack_top 的使用、kernel_main 里的验证与 panic 调用
  solution/
    x86_64/
      boot.S                  Multiboot2 头 + 页表 + 清零 BSS + 长模式切换 + 跳 C（长模式切换部分与 Lab1 相同）
      linker.ld                新增 __bss_start/__bss_end/__stack_bottom/__stack_top 符号
      console_putc.c            与 Lab1 完全相同，不是本 Lab 教学内容
      panic_arch.c               与 Lab1 完全相同，不是本 Lab 教学内容
      kernel_main.c              验证 BSS 清零 + 触发一次 panic()
      grub.cfg                   与 Lab1 相同，menuentry 名字换成 lab02
    riscv64/
      boot.S                    清零 BSS + 设栈 + call kernel_main（比 x86_64 短得多，原因见核心概念一节）
      linker.ld                  新增的四个符号，位置同 x86_64
      console_putc.c              与 Lab1 完全相同
      panic_arch.c                与 Lab1 完全相同
      kernel_main.c                验证 BSS 清零 + 触发一次 panic()
  tests/
    expect-x86_64.txt           x86_64 期望的串口输出（含 panic 的文件名/行号格式）
    expect-riscv64.txt           riscv64 期望的串口输出
```

## 分步实现步骤

### 两边通用：链接脚本改动

在 `.rodata`/`.data` 之后、`.bss` 之前插入：

```
. = ALIGN(4096);
__bss_start = .;
.bss : {
    *(.bss)
    *(.bss.*)
    *(COMMON)
}
__bss_end = .;

. = ALIGN(16);
__stack_bottom = .;
. += 16384;
__stack_top = .;
```

`. += 16384;` 是"把当前地址往前推 16384 字节，不生成任何输出 section"——这块空间不需要被链接进任何段的内容，只需要预留地址范围，运行时当栈用，本身不需要初始值。

### x86_64 路线

1. **`_start` 里先设 `%esp`**：和 Lab1 一样的动作，只是数值来源换成链接脚本的 `__stack_top`，不再是本文件内 `.skip` 出来的局部标号。
2. **清零 BSS**：用 `__bss_start`/`__bss_end` 算出字节数，逐字节循环清零。必须在**填页表之前**做——`pml4`/`pdpt`/`pd` 这三块空间本身就落在 `.bss` 里（继承自 Lab1），如果清零逻辑写在填页表之后，会把刚填好的页表项覆盖成 0。
3. 页表填充、`CR4.PAE`、`CR3`、`EFER.LME`、`CR0.PG`、`lgdt`+`ljmp` 这几步和 Lab1 完全一样，本 Lab 不重复讲，直接抄过来。
4. **`_start64` 里重新设 `%rsp`**：同样换成 `__stack_top`，64 位模式下用满宽度的 `%rsp`。

### riscv64 路线

1. **清零 BSS**：用 `t0`/`t1` 走字节循环，逻辑和 x86_64 一致，只是寄存器名和指令换了。riscv64 这边没有页表要填（本 Lab 还没有开分页，riscv64 恒等映射到 Lab4 才引入），所以清零顺序不像 x86_64 那样有"必须在填页表之前"的硬性要求，但仍然应该在 `kernel_main` 能访问任何 `.bss` 变量之前完成。
2. **设 `sp`，`call kernel_main`**：和 Lab1 一样，只是符号名换成 `__stack_top`。

### 两边通用：`kernel_main.c`

1. 声明一个没有显式初始化的 `static int untouched_bss_counter;`，打印它的值——如果 BSS 清零逻辑写对了，这里必须是 0。
2. 调用 `panic("Lab2 checkpoint: intentional panic to verify file/line reporting");`，验证诊断路径本身工作正常。

## QEMU 运行命令

```bash
make ARCH=x86_64 LAB=lab02-kernel-entry run
make ARCH=riscv64 LAB=lab02-kernel-entry run
```

两边都应该看到类似这样的输出（文件路径是相对仓库根目录的相对路径，不是你机器上的绝对路径——原因见下面"踩坑记录"）：

```
Hello OS from x86_64 (Lab2: kernel entry)
untouched_bss_counter = 0 (expect 0, proves boot.S zeroed .bss)

*** KERNEL PANIC ***
  at labs/lab02-kernel-entry/solution/x86_64/kernel_main.c:26
  Lab2 checkpoint: intentional panic to verify file/line reporting
System halted.
```

打印完之后 QEMU 不会自动退出（`panic_halt()` 让 CPU 停在 `hlt`/`wfi` 循环里），`Ctrl-A X` 或 `Ctrl-C` 结束。

## GDB/QEMU Monitor 调试方法

如果 `untouched_bss_counter` 打印出来不是 0，先确认问题出在"清零逻辑没跑"还是"清零逻辑跑了但清错了范围"：

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab02-kernel-entry
```

```
(gdb) break kernel_main
(gdb) continue
(gdb) p &untouched_bss_counter
(gdb) p/x __bss_start
(gdb) p/x __bss_end
```

确认 `&untouched_bss_counter` 落在 `[__bss_start, __bss_end)` 范围内——如果不在这个范围里，说明链接脚本的符号位置或者变量实际的段归属出了问题（比较少见，但如果你后续修改过 `linker.ld` 里 section 的顺序，值得排查一下）。如果确实在范围内但值不是 0，去 `boot.S` 里给清零循环加个断点单步，确认 `t0`/`edi` 的起止地址和 `__bss_start`/`__bss_end` 是否一致。

riscv64 同理，换成 `ARCH=riscv64`，寄存器名换成 `t0`/`t1`。

## 自动验收测试

```bash
make test ARCH=x86_64 LAB=lab02-kernel-entry
make test ARCH=riscv64 LAB=lab02-kernel-entry
```

`tests/expect-*.txt` 里包含 panic 输出的文件名一行，比对方式是子串匹配（见 [`scripts/test-lab.sh`](../../scripts/test-lab.sh)），只要求这一行**出现在**串口输出里，不要求出现在某个固定行号——所以你不需要让自己的实现在字符层面完全复刻 solution 的空行、注释位置，只要 `kernel_main.c` 里 `panic()` 调用点所在的文件路径一致，具体是第几行不影响测试通过。

## 常见坑与排查

- **BSS 清零循环忘了在填页表之前跑（x86_64）**：`pml4`/`pdpt`/`pd` 落在 `.bss` 里，清零循环如果排在页表填充**之后**执行，会把刚写好的页表项清成 0，CR3 指向一堆全 0 的页表项，等同于没有有效映射。实测现象是**串口一个字符都不会打印**，QEMU 直接陷入三重故障重启循环（用 `-d cpu_reset` 能看到反复出现的 `CPU Reset` 记录）——`ljmp` 跳进长模式那一刻就死了，代码根本走不到 `kernel_main`，所以不会出现"打印出垃圾值"这种中间状态。检查 `boot.S` 里清零循环和 `mov $pdpt, %eax; ...; mov %eax, (pml4)` 这几行的先后顺序。
- **忘记先跑 `make clean` 就切换 `ARCH`**：和 Lab1 一样的提醒，`build/` 目录下的旧产物有时会让人误以为新代码没生效。

### 踩坑记录：这两个问题是本课程设计 Lab2 时真实踩到的

**RWX/RWE 合并段**：本 Lab 最初的 `linker.ld` 版本里，`__bss_start`/`__bss_end` 紧跟在 `.rodata`/`.data` 后面，中间没有任何对齐边界。riscv64 那边构建时链接器直接报了一条警告：

```
riscv64-elf-ld: warning: kernel.elf has a LOAD segment with RWX permissions
```

用 `readelf -l` 对比 Lab1 和这个版本的 Lab2 才看清楚发生了什么：Lab1 的 riscv64 `.bss` 恰好独占一个干净的段（`RW`），Lab2 因为多了 `__bss_start`/`__bss_end` 两个符号紧贴在 `.rodata`（`R E`，因为 riscv64 链接脚本没有单独的 `.text`/`.rodata` 段分离对齐）后面，链接器把它们合并成了一个段，权限取了并集，变成 `RWE`。x86_64 那一版本当时反而没报警——但排查后发现那纯粹是运气：`boot.S` 里 `pml4` 的 `.align 4096` 声明恰好把 `.bss` 推到了下一个页，产生了一个"看起来正确但完全是偶然"的段边界。这个问题修好之后（两边的 `linker.ld` 都在 `.bss` 前加了显式的 `. = ALIGN(4096);`），`readelf -l` 才在两个架构上都显示出干净的 `R E` / `RW` 两段结构，不再依赖任何偶然的对齐副作用。

**`panic()` 打印出的文件路径是这台机器的绝对路径**：本 Lab 是全课程第一个真正调用 `panic()` 的 Lab，第一次跑自动化测试时才发现 `__FILE__` 展开出来的是构建时的绝对路径（比如 `/home/xxx/osdev-lab/labs/lab02-kernel-entry/solution/x86_64/kernel_main.c`）——这个值天然是构建机器相关的，写死进 `tests/expect-*.txt` 会导致换一台机器、换一个检出目录就测试失败，完全违反了"任何学习者在任何路径下检出仓库都应该能通过测试"的前提。修复方式是在 [`src/common/toolchain.mk`](../../src/common/toolchain.mk) 里给两个架构的 `CFLAGS` 都加上 `-ffile-prefix-map=$(REPO_ROOT)/=`——这个编译器选项会把 `__FILE__` 里匹配 `$(REPO_ROOT)/` 前缀的部分替换成空字符串，效果是不管仓库检出到哪个绝对路径，`__FILE__` 展开出来的都是相对仓库根目录的相对路径（比如 `labs/lab02-kernel-entry/solution/x86_64/kernel_main.c`），`tests/expect-*.txt` 才能写成一份对所有人都适用的期望值。

## 附录：用 `readelf`/`nm` 检查符号表和段布局

本 Lab 的核心教学内容（链接脚本、BSS、段权限）用肉眼读代码不容易验证是否正确，下面几个命令是排查这类问题最直接的手段，建议在完成 starter 实现之后亲自跑一遍，和上面"踩坑记录"里描述的现象对照着看。

**注意工具链前缀**：这几个命令必须用能识别目标架构 ELF 文件的 `readelf`/`nm`，Linux 上系统自带的 binutils 通常就够用，但 macOS 上系统没有 `readelf`（Apple 的开发工具链里没有这个命令），需要用 Homebrew 装的交叉工具链自带的版本——和本课程构建内核用的编译器是同一套工具链，前缀规则见 [`src/common/toolchain.mk`](../../src/common/toolchain.mk) 顶部的注释（一般是 `x86_64-elf-`/`riscv64-elf-`，如果你的 riscv64 环境解析成了 `riscv64-unknown-elf-` 或 `riscv64-linux-gnu-`，把前缀换成对应的即可）。

```bash
# 看 ELF 的 program header（段布局），重点看 Flags 这一列：应该是 R E 和 RW 两段，
# 不应该出现同时带 W 和 E 的段
x86_64-elf-readelf -l labs/lab02-kernel-entry/solution/x86_64/build/kernel.elf

# 看具体的符号地址，确认 __bss_start/__bss_end/__stack_top 的值符合预期
# （__bss_end 应该大于 __bss_start，__stack_top 应该比 __stack_bottom 大 16384）
x86_64-elf-nm labs/lab02-kernel-entry/solution/x86_64/build/kernel.elf | grep -E '__bss|__stack'

# 看 section 级别的布局（比 program header 更细），确认 .bss 的地址和大小
x86_64-elf-readelf -S labs/lab02-kernel-entry/solution/x86_64/build/kernel.elf | grep -A1 '\.bss'
```

riscv64 把上面三条命令的路径换成 `solution/riscv64/build/kernel.elf`，前缀换成 `riscv64-elf-`。

## 挑战任务

- **两边通用**：故意在 `linker.ld` 里去掉 `__bss_start` 前面的 `. = ALIGN(4096);`，重新构建，用 `readelf -l` 观察段布局的变化——你应该能复现"踩坑记录"里描述的 RWX 合并现象（在 riscv64 上更容易复现，x86_64 上取决于 `pml4` 的对齐是否恰好掩盖了这个问题，两边都试一下，体会一下"偶然正确"和"显式正确"的区别）。改完记得恢复。
- **两边通用**：把 `kernel_main.c` 里 `untouched_bss_counter` 的声明从 `static int untouched_bss_counter;` 改成 `static int untouched_bss_counter = 1;`（显式初始化为一个非零值），用 `nm`/`readelf -S` 确认它现在落在了 `.data` 段而不是 `.bss`（提示：`nm` 输出里符号类型字母 `d` 表示 `.data`，`b` 表示 `.bss`）。这个练习值得注意的地方是：如果你把初值改成 `= 0`（而不是非零值）再试一次，会发现编译器仍然把它放进 `.bss`——GCC 会把"显式初始化为 0"和"不初始化"当成同一件事来优化（反正 `.bss` 段本身在运行时就该是 0，没必要在 ELF 文件里存一份 0），只有初始化成非零值才会真正强制进 `.data`。这也是为什么本 Lab 选一个未显式初始化的变量来验证 BSS 清零：这类变量的存储位置是"编译器决定要不要清零"这条链路上最直接受影响的一环。
- **x86_64**：故意把 BSS 清零循环挪到页表填充**之后**执行，观察失败的具体表现——串口不会有任何输出，`make run` 会一直卡到超时，加 `-d cpu_reset` 能看到反复的三重故障，验证"核心概念"一节里提到的顺序依赖是真实存在的，不是纸上谈兵。改完记得恢复顺序。

## 参考

- OSDev Wiki: [Linker Scripts](https://wiki.osdev.org/Linker_Scripts)
- [GNU ld 官方文档：SECTIONS 命令](https://sourceware.org/binutils/docs/ld/SECTIONS.html)（`ALIGN`、符号赋值等语法的权威定义）
- [System V Application Binary Interface AMD64 位补充](https://gitlab.com/x86-psABIs/x86-64-ABI)（栈 16 字节对齐要求的出处）
- Rust 分支迁移要点见 [`docs/rust-track.md`](../../docs/rust-track.md) 的"Lab1-2：Boot 与内核入口"一节（已经涵盖 BSS 清零在 Rust 下的等价讨论）

## 下一步

进入 [Lab3：物理内存管理](../lab03-physical-memory/README.md)，从固件手里拿到真实可用的物理内存范围（x86_64 解析 Multiboot2 的内存映射 tag，riscv64 解析设备树），写一个链表式的空闲页分配器（`kalloc`/`kfree`），第一次让内核具备"动态管理一块真实资源"的能力。
