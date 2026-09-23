# Lab3：物理内存管理

## 学习目标

- 第一次从固件/引导器手里拿到"这台机器真的有多少内存、分布在哪几段"这个信息——x86_64 解析 GRUB 按 Multiboot2 协议塞给内核的内存映射（memory map）tag，riscv64 解析 OpenSBI 通过 `a1` 交给内核的设备树（Devicetree Blob，DTB）。两条路线格式完全不同，但目的完全一样：把"这段物理地址范围可以用"这件事从"外部环境告诉我们的原始数据"翻译成一组 `(base, length)`。
- 写一个真正的资源管理器，不再是"一次性用完就不管"的教学代码：链表式空闲页分配器要支持反复分配、释放、合并，长期运行不泄漏、不重叠，这是本课程第一个需要考虑"正确性该怎么验证"而不是"能跑就行"的数据结构。
- 建立"内存发现"和"内存分配"之间的清晰边界：`kalloc_add_region()` 是这两层之间唯一的接口，架构专属的探测代码（`memmap.c`）只管找到可用范围然后喂给它，完全不需要知道链表怎么组织、怎么合并——这条边界从本 Lab 开始固定下来，后面的 Lab（尤其是 Lab4 的页表、Lab7 的进程内存）都会在这个分配器之上继续搭。
- 亲手处理一次"文档里不会写、只能靠实测发现"的现实：OpenSBI 固件自己占用的那段物理内存，不会出现在 DTB 的任何字段里，内核必须自己知道要排除哪一段。

## 前置 Lab

[Lab2：内核入口、链接脚本、BSS、栈、panic](../lab02-kernel-entry/README.md)。本 Lab 复用 Lab2 的 `boot.S`/`linker.ld` 不做任何修改（跳进 C 代码之前要做的事没有变化），新增内容全部在 `kernel_main.c` 要调用的新文件 `memmap.c` 里，以及 `src/common/` 下第一次新增的共享文件 `kalloc.c`。

## 核心概念

**Multiboot2 boot information 不是一份文档，是一段你要亲手走一遍的 tag 序列**：GRUB 跳进 `kernel_main` 之前，会把内核入口参数（x86_64 是 `%edi`，见 Lab2 里 `boot.S` 那处"为什么用 `%edi` 不用 `%rdi`"的注释）设成一个内存地址，指向一段"boot information 结构"。结构开头是 8 字节头（`total_size` + `reserved`，各 4 字节 `uint32_t`），后面跟着一串 8 字节对齐的 tag，每个 tag 自己也有 8 字节头（`type` + `size`，同样各 4 字节）。要找的内存映射 tag 是 `type == 6`，它比其它 tag 多两个字段（`entry_size`/`entry_version`），紧跟着的是若干条 24 字节的表项（`base_addr`/`length` 各 8 字节 `uint64_t`，`type`/`reserved` 各 4 字节 `uint32_t`），`type == 1` 表示这段是可用 RAM。从一个 tag 跳到下一个 tag，偏移量不是 `size` 本身，是 `(size + 7) & ~7`——size 描述的是这个 tag 实际占用的字节数，但下一个 tag 必须从 8 字节边界开始，中间的 padding 字节需要手动跳过，这个对齐计算错一位就会导致后续所有 tag 全部解析错位。

**riscv64 这边的"boot information"是设备树，OpenSBI 给的是指针不是结构体**：OpenSBI 交接给内核时的约定是 `a0` = 当前 hart（核心）编号，`a1` = 设备树二进制（Devicetree Blob，DTB/FDT）在物理内存里的地址——这正好是 riscv 调用约定里前两个整数参数寄存器，所以 `kernel_main(uint64_t hartid, uint64_t dtb_paddr)` 不需要任何额外汇编就能拿到这两个值（细节见 riscv64 `boot.S` 顶部注释和 `kernel_main.c` 里"为什么参数顺序不能换"那条注释）。DTB 的格式和 Multiboot2 完全是另一套体系：文件头是 10 个大端 `uint32_t` 字段（本 Lab 只用到其中两个，见 `memmap.c` 里为什么没有声明完整 `struct fdt_header` 的注释），结构块是一串 token（`FDT_BEGIN_NODE`/`FDT_END_NODE`/`FDT_PROP`/`FDT_NOP`/`FDT_END`），要靠手写的 token walker 找到 `/memory` 节点的 `reg` 属性。本课程不链接 `libfdt`——和 `types.h` 不用宿主 `<stdint.h>` 是同一个取向：所有格式细节自己写代码走一遍，不引入一个"能用但看不见内部发生了什么"的库。

**大端序是 DTB 解析里唯一需要手动处理的"意外"**：riscv64 本身是小端 CPU，但 Devicetree Specification 明确规定结构块里所有多字节字段都是大端序——如果直接把 DTB 里的字节当 `uint32_t`/`uint64_t` 解引用，读出来的 `size_dt_struct`、`reg` 地址之类字段会是一个完全错误的巨大数字（字节序反过来读，几乎不可能凑巧是个合理值），表现成"解析出来的内存范围完全不对，或者直接触发地址越界"。`memmap.c` 里的 `be32()`/`be64_from_cells()` 就是唯一负责这次字节序转换的地方。

**OpenSBI 固件占用的内存，DTB 里不会告诉你**：QEMU 的 `virt` 机器上，OpenSBI 的 `fw_jump` 固件本身占用 `0x80000000`-`0x80200000`（这也是为什么内核链接地址选在紧跟固件后面的 `0x80200000`），但这段占用完全不反映在 DTB 的任何字段里——`/memory` 节点的 `reg` 属性报的是"这段物理地址范围存在"，不是"这段范围空闲"，`reserved-memory` 节点通常用来标记这类占用，但 QEMU 生成的 DTB 里没有为 OpenSBI 自己打这个标记（[riscv-software-src/opensbi 项目的 issue 讨论](https://github.com/riscv-software-src/opensbi/issues/70)确认过这一点是已知行为，不是本课程环境特有的 bug）。内核自己占用的那段内存（代码/数据/BSS/栈）同样不会被固件动态排除，需要内核自己用链接脚本给的 `__stack_top` 符号算出自己的内存footprint，在把 DTB 报出来的范围喂给分配器之前先排除掉重叠部分——这一步如果漏掉，`kalloc_pages()` 分配出来的地址可能落在内核自己的代码段或者栈上，覆盖内核正在运行所需要的内存，多数情况下的直接表现是内核在毫无征兆的地方跑飞或者 panic 信息本身变得不可信（因为 panic 用到的字符串常量、`kprintf` 内部状态可能已经被覆盖）。

**空闲页链表为什么按"一段连续页（run）"组织，不是按单页**：ROADMAP 对本 Lab 的明确要求是"分配/释放任意数量的 4KiB 页"——调用者随时可能一次要 N 个连续页，如果链表节点粒度是单页，没有办法保证分配出来的 N 页物理上连续。`src/common/include/kalloc.h` 里选择的设计是把空闲内存组织成若干个 run（每个 run 是一段连续的空闲页），每个 run 的元数据（下一个 run 的指针 + 页数）就存在这个 run 自己的第一页里——空闲页反正没有别的用途，不需要单独找地方存链表节点。分配用 first-fit（从链表头开始找第一个够大的 run），不是 best-fit，这是教学取向的简化：本 Lab 的重点是"链表怎么正确地拆分、合并"这个机制本身，不是内存碎片优化策略。

## x86_64 与 riscv64 对照表

| 主题 | x86_64 | riscv64 |
|---|---|---|
| "boot information" 长什么样 | 一段 tag 序列（Multiboot2 boot information），GRUB 通过 `%ebx`→`%edi` 给指针 | 一段大端序二进制 token 流（DTB/FDT），OpenSBI 通过 `a1` 给指针 |
| 要找的具体结构 | `type == 6` 的内存映射 tag，每条表项 24 字节 | `/memory` 节点的 `reg` 属性，每组 16 字节（`#address-cells`/`#size-cells` 都是 2） |
| 字节序 | 全程小端，和 CPU 原生字节序一致，不需要任何转换 | 结构块字段全程大端，需要手动 `be32`/`be64` 转换 |
| "这段内存不能用"的隐藏信息来源 | 无——Multiboot2 mmap tag 的 `type` 字段本身就区分可用/不可用/保留，没有本 Lab 需要额外处理的隐藏占用 | 有：OpenSBI 固件自己占用的 `0x80000000`-`0x80200000` 不反映在 DTB 里，内核必须用 `__stack_top` 自行排除 |
| 找到范围之后的处理 | 每条 `type == 1` 的表项直接调 `kalloc_add_region()` | 每组 `reg` 先过 `add_region_excluding_kernel_image()`，排除和内核镜像重叠的部分，剩下的再调 `kalloc_add_region()` |
| 本 Lab 新增代码量 | `memmap.c` 约 90 行（tag 遍历 + mmap 表项遍历） | `memmap.c` 约 150 行（DTB 头解析 + token 遍历 + 节点深度跟踪 + 内核镜像排除） |

**riscv64 这次的解析逻辑明显更长，但不是因为"riscv64 更难"**：Multiboot2 的 tag 结构是扁平的（一个 tag 挨着下一个 tag，不需要跟踪嵌套层级），DTB 的结构块是树状的（节点可以嵌套节点），找到目标节点需要一个深度计数器分辨"现在在哪一层、这个属性到底属于哪个节点"，这是格式本身的复杂度差异，不是架构指令集层面的差异——本课程到目前为止的所有架构对照，第一次出现"复杂度差异来自外部数据格式，不来自 CPU 本身"的情况。

## 代码目录与关键文件

```
labs/lab03-physical-memory/
  README.md                  本文件
  Makefile                    ARCH=x86_64|riscv64 VARIANT=solution|starter 通用构建入口
                               （COMMON_SRCS 新增 kalloc.c，ARCH_SRCS 新增 memmap.c）
  starter/
    x86_64/                   boot.S/linker.ld/console_putc.c/panic_arch.c/grub.cfg 与 solution 完全一致
                               （都不是本 Lab 教学内容），memmap.c 和 kernel_main.c 带 TODO
    riscv64/                  同上，boot.S/linker.ld/console_putc.c/panic_arch.c 与 solution 完全一致
  solution/
    x86_64/
      boot.S                    与 Lab2 完全相同
      linker.ld                  与 Lab2 完全相同
      console_putc.c              与 Lab1/Lab2 完全相同
      panic_arch.c                 与 Lab1/Lab2 完全相同
      grub.cfg                      与 Lab2 相同，menuentry 名字换成 lab03
      memmap.c                       解析 Multiboot2 mmap tag，喂给 kalloc_add_region()
      kernel_main.c                   调 memmap_discover()，做一次 kalloc_pages/kfree_pages 往返验证
    riscv64/
      boot.S                    与 Lab2 完全相同
      linker.ld                  与 Lab2 完全相同
      console_putc.c              与 Lab1/Lab2 完全相同
      panic_arch.c                 与 Lab1/Lab2 完全相同
      memmap.c                       解析 DTB /memory 节点，排除内核镜像占用后喂给 kalloc_add_region()
      kernel_main.c                   同 x86_64
  tests/
    expect-x86_64.txt           x86_64 期望的串口输出
    expect-riscv64.txt           riscv64 期望的串口输出

src/common/
  include/kalloc.h              分配器公开接口（本 Lab 新增，架构无关）
  kalloc.c                       链表式空闲页分配器实现（本 Lab 新增，两个架构共用同一份，不区分 VARIANT）
```

**`kalloc.c` 放在 `src/common/`，不是某个架构的 starter/solution 目录**：本 Lab 的教学重点分成两半——"怎么从固件手里拿到内存范围"是架构专属的（放进各自的 `memmap.c`），"拿到范围之后怎么管理"是架构无关的（分配器本身不关心这些地址是 Multiboot2 报的还是 DTB 报的）。分配器实现直接放进 `src/common/kalloc.c`，两个架构、甚至 starter/solution 两个变体都共用这同一份实现——这意味着 starter 变体里你只需要实现 `memmap.c`（怎么发现内存），分配器本身是现成可用的，不需要重新实现链表逻辑。

## 分步实现步骤

### x86_64 路线：`memmap.c`

1. **拿到 boot information 的起始地址**：`kernel_main` 的参数（`%edi`，Lab2 已经设好）就是这个地址，直接当 `const uint8_t *` 用（本 Lab 阶段页表仍是身份映射，物理地址可以直接当指针解引用，这个前提到后面引入更复杂虚拟内存布局的 Lab 才会失效）。
2. **读 8 字节头**，拿到 `total_size`。
3. **从偏移 8 开始遍历 tag**：每个 tag 先读 `{type, size}`，`type == 0` 是结束标记，遇到就停；`type == 6` 是内存映射 tag，进第 4 步；其它 type 直接跳过。跳到下一个 tag 的偏移量是当前偏移 `+ ((size + 7) & ~7)`，不是 `+ size`。
4. **内存映射 tag 内部再遍历表项**：表项起始偏移是 tag 起始偏移 `+ 16`（`type`/`size`/`entry_size`/`entry_version` 四个 `u32`），每条表项占 `entry_size` 字节（用 tag 里读出来的值，不要硬编码 24——即使实测这个值恰好总是 24，读所在 tag 自己声明的字段仍然是更直接对的做法）。`type == 1` 的表项调 `kalloc_add_region(base_addr, length)`。

### riscv64 路线：`memmap.c`

1. **拿到 DTB 起始地址**：`kernel_main` 的第二个参数（`a1`，OpenSBI 已经设好）。先读开头 4 字节校验 `magic == 0xd00dfeed`，不是的话说明 `a1` 根本没指向一个合法的 FDT，直接 panic 比继续往下解析出一堆垃圾数据更容易定位问题。
2. **读文件头里的 `off_dt_struct`（偏移 8）和 `size_dt_struct`（偏移 36）**，算出结构块的起止范围。
3. **遍历结构块的 token 流，维护一个深度计数器**：`FDT_BEGIN_NODE` 深度加一，`FDT_END_NODE` 深度减一。**根节点自己会占用深度 1**——`/memory` 是根节点的直接子节点，落在深度 2，不是深度 1（这一点如果想当然按"设备树顶层节点是深度 1"来判断，会导致永远匹配不到 `/memory`，本 Lab 设计阶段真的因为这个想当然的假设踩过一次，具体过程见下面"常见坑与排查"）。
4. **在深度 2 判断节点名是不是 `memory` 或者 `memory@...`**，记录"当前是否在目标节点内部"这个状态。
5. **遇到 `FDT_PROP` 且当前在目标节点内部**：属性数据按 16 字节一组解析（`#address-cells`/`#size-cells` 都是 2，一组是 `addr_hi/addr_lo/size_hi/size_lo` 四个大端 `u32`），每组算出 `(base, length)`。
6. **每组范围先过一次内核镜像排除**：和 `[dtb 报的 base, __stack_top)` 求交集之外的部分才是真正空闲的，调 `kalloc_add_region()` 喂的是排除之后的范围，不是 DTB 原始报的范围。

### 两边通用：`kernel_main.c`

1. 打印一行身份字符串（和之前每个 Lab 一样），调用 `memmap_discover()`。
2. 记录调用前的 `kalloc_free_pages()`，跑一次 `kalloc_pages(4)` → 校验非 NULL、页对齐 → `kfree_pages(..., 4)` → 校验释放之后的 `kalloc_free_pages()` 和调用前完全一致（这是检验"没有泄漏、没有重复计数"最直接的方式）。
3. 用 `panic()` 结束，延续本课程每个 Lab 用一次故意触发的 panic 作检查点的习惯。

## QEMU 运行命令

```bash
make ARCH=x86_64 LAB=lab03-physical-memory run
make ARCH=riscv64 LAB=lab03-physical-memory run
```

x86_64 预期输出（`32480` 这个数字取决于 QEMU 默认给的 128MiB 内存减去低地址那段固定保留区域，不是一个需要你精确复现的魔法数字，只要数量级和"没有变成 0 或者变成一个荒谬的巨大值"对得上就说明解析逻辑基本正确）：

```
Hello OS from x86_64 (Lab3: physical memory)
memmap: 2 available region(s) from Multiboot2 mmap tag, 32480 page(s) free
kalloc_pages(4) = 0x100000
kalloc/kfree round-trip OK, free pages unchanged at 32480

*** KERNEL PANIC ***
  at labs/lab03-physical-memory/solution/x86_64/kernel_main.c:48
  Lab3 checkpoint: kalloc/kfree verified, halting here on purpose
System halted.
```

riscv64 预期输出（前面会先看到 OpenSBI 自己的启动横幅，这是固件打印的，不是内核代码的输出）：

```
Hello OS from riscv64 (Lab3: physical memory)
memmap: 1 region(s) from DTB /memory, 32249 page(s) free (kernel image excluded)
kalloc_pages(4) = 0x80207000
kalloc/kfree round-trip OK, free pages unchanged at 32249

*** KERNEL PANIC ***
  at labs/lab03-physical-memory/solution/riscv64/kernel_main.c:47
  Lab3 checkpoint: kalloc/kfree verified, halting here on purpose
System halted.
```

打印完之后 QEMU 不会自动退出，`Ctrl-A X` 或 `Ctrl-C` 结束。

## GDB/QEMU Monitor 调试方法

如果 `memmap_discover()` 报出来的 region 数量是 0（直接触发 panic），或者数量/页数明显不对，先确认问题出在"根本没找到 tag/节点"还是"找到了但范围算错了"：

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab03-physical-memory
```

```
(gdb) break memmap_discover
(gdb) continue
(gdb) next
# 单步走过 tag/token 遍历循环，配合 p/x offset、p hdr->type、p hdr->size 之类命令
# 确认每一步算出来的偏移量和实际读到的 type 是否符合预期
```

riscv64 换成 `ARCH=riscv64`，重点关注 `depth`/`in_memory_node` 这两个变量的变化时机——如果 `in_memory_node` 一直是 `false`，说明深度判断的条件（`depth == 2`）和你的实现假设不一致，去对照上面"常见坑与排查"里描述的根节点深度问题。

## 自动验收测试

```bash
make test ARCH=x86_64 LAB=lab03-physical-memory
make test ARCH=riscv64 LAB=lab03-physical-memory
```

`tests/expect-*.txt` 用子串匹配（见 [`scripts/test-lab.sh`](../../scripts/test-lab.sh)），每一行只要求在串口输出里出现，不要求整行位置精确对应。注意 `kalloc_pages(4) = 0x...` 和 `free pages unchanged at N` 这两类行里的具体数值，在测试环境里是确定性的（QEMU 默认内存大小、内核镜像大小在没有额外改动的情况下都是固定的），但如果你在 starter 实现过程中给 `kernel_main.c` 加了额外的调试打印、或者往内核里加了新的全局变量导致镜像变大，这些数值可能会发生小幅偏移——这不代表你的实现错了，只代表期望值这一行需要你根据自己实际跑出来的输出更新，不需要强迫自己的镜像大小和 solution 完全一致。

## 常见坑与排查

- **riscv64：`/memory` 节点判断永远匹配不上，`region_found` 一直是 0**：如果按"设备树顶层节点是深度 1"这个直觉写判断条件，会漏掉一个事实——DTB 的根节点（名字是空字符串）本身就是第一个被 `FDT_BEGIN_NODE` 打开的节点，占用了深度 1，`/memory` 作为根节点的直接子节点，实际落在深度 2。排查方式：用 `qemu-system-riscv64 -machine virt -bios default -machine dumpdtb=/tmp/qemu-virt.dtb` 把 QEMU 实际生成的 DTB 导出来，用一个小 Python 脚本按 FDT 格式手动走一遍 token 流（不需要装 `pylibfdt`，用 `struct.unpack('>I', ...)` 手动读大端 `u32` 就够），打印每个 `FDT_BEGIN_NODE` 的名字和当时的深度，比隔着内核代码猜测更快确认真实的嵌套层级。
- **riscv64：解析出来的内存范围包含了内核自己，`kalloc_pages()` 分配出一个落在内核代码段里的地址**：DTB 的 `/memory` `reg` 属性报的是"物理内存存在"，不是"物理内存空闲"，OpenSBI 固件自己占用的范围和内核镜像占用的范围都不会被 DTB 自动排除。排查方式：`riscv64-elf-nm build/kernel.elf | grep __stack_top` 看内核镜像实际占到哪个地址，和 `memmap.c` 里 `add_region_excluding_kernel_image()` 算出来的排除范围手动核对一遍。
- **x86_64：把内存映射表项的间距硬编码成 24 字节**：`entry_size` 是 mmap tag 自己声明的字段，规范允许这个值和 24 不同（比如未来固件版本在表项末尾加了新字段），本 Lab 的参考实现读的是 `mmap_tag->entry_size` 而不是 `sizeof(struct mb2_mmap_entry)`，即使这两个值目前恰好总是相等。
- **两边通用：`kalloc_add_region()` 传入的 base/length 没有考虑页对齐**：分配器内部会自动把 base 向上取整、把结束地址向下取整到页边界，取整之后剩下不够一整页的部分会被安静丢弃（不会报错），这是有意的教学简化——调用方不需要自己算对齐，但也不应该假设传进去的每一个字节都会变成可用页。
- **忘记先跑 `make clean` 就切换 `ARCH`**：和前面每个 Lab 一样的提醒。

## 挑战任务

- **两边通用**：把 `kernel_main.c` 里的 `kalloc_pages(4)` 换成一个明显超过实际可用页数的请求（比如 `kalloc_pages(100000)`），确认 `kalloc_pages()` 返回 `NULL` 而不是崩溃或者返回一个错误地址——这是在验证"分配失败"这条路径本身是不是可靠的，很多手写分配器在实现阶段只测试过"分配成功"的路径。
- **两边通用**：连续调用几次不同大小的 `kalloc_pages()`/`kfree_pages()`（比如分配 3 页、再分配 5 页、释放第一次分配的 3 页、再分配 2 页），用 `kalloc_free_pages()` 在每一步之后打印剩余页数，手动验证链表的拆分和合并逻辑在多次操作之后仍然保持正确——单次"分配再释放"的往返测试不足以暴露链表维护逻辑里的边界条件问题（比如释放的 run 恰好在链表中间、需要同时向前向后合并的情况）。
- **riscv64**：故意把 `add_region_excluding_kernel_image()` 注释掉（直接把 DTB 报的原始范围喂给 `kalloc_add_region()`），观察会发生什么——提示：不一定会立刻崩溃或者立刻能看出来哪里错了，`kalloc_pages()` 可能会分配出一个落在内核已用内存范围内的地址，如果这次分配恰好没有被马上写入，症状可能会推迟到后续某次真正往这块"内存"写数据时才出现，这类"分配到了不该分配的内存，但不是立刻炸"的 bug 在真实内核开发里比"直接崩溃"的 bug 更难定位，值得亲自体会一次。做完记得恢复。
- **x86_64**：查一下 Multiboot2 规范里除了内存映射 tag（type=6）之外还有哪些 tag 类型（比如 type=8 是 framebuffer 信息，type=9 是 ELF 符号表信息），在 `memmap.c` 的 tag 遍历循环里加一个分支，打印一下你的 QEMU/GRUB 组合实际提供了哪些 tag——这能帮助建立"boot information 不是一份写死的清单，实际内容取决于引导器版本和内核自己在 Multiboot2 header 里有没有主动请求"这个认识。

## 参考

- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)（内存映射 tag 的字段定义、tag 对齐规则的权威出处）
- [Devicetree Specification](https://www.devicetree.org/specifications/)（FDT 二进制格式、token 类型、`reg`/`#address-cells`/`#size-cells` 语义的权威出处）
- [riscv-software-src/opensbi issue #70](https://github.com/riscv-software-src/opensbi/issues/70)（OpenSBI 固件占用内存不反映在 DTB 里这一行为的讨论）
- OSDev Wiki: [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_%28x86%29)

## 下一步

进入 [Lab4：虚拟内存](../lab04-virtual-memory/README.md)，在本 Lab 建好的物理页分配器之上，第一次让内核自己管理虚拟地址到物理地址的映射关系——x86_64 实现 4 级页表，riscv64 实现 Sv39，`kalloc_page()` 会成为页表本身占用内存的来源。
