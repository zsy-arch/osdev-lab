# Lab4：虚拟内存

## 学习目标

- 理解分页硬件的本质：页表把虚拟地址翻译成物理地址，翻译规则由 CPU 按固定算法解释一段特定格式的内存（页表），不是软件可以随意重新定义的抽象。
- 亲手写出 x86_64 4 级页表（PML4/PDPT/PD/PT）和 riscv64 Sv39 3 级页表（L2/L1/L0）的建表、查表代码，理解两种 PTE 格式的本质差异不是"位置不同"，而是"叶子节点的识别方式不同"（x86_64 靠层级+PS 位，riscv64 靠 RWX 是否全零）。
- 把内核从"链接地址=加载地址=低地址"改造成高半区内核（higher-half kernel），理解为什么现代内核几乎都把自己链接在地址空间顶端，以及"跳转后撤掉低地址映射"这个设计决定具体怎么落地。
- 手写最窄的缺页异常（page fault）处理路径：注册陷入向量、从硬件拿到出错地址和原因、panic 报告——不做故障恢复（按需分页、写时复制留给后面的 Lab），但要理解为什么"能识别、能报告"本身就是分页硬件带来的新增复杂度。

## 前置 Lab

依赖 [Lab3：物理内存管理](../lab03-physical-memory/README.md)——本 Lab 的页表节点全部通过 `kalloc_page()` 现场分配，`memmap_discover()` 的内存探测逻辑原样复用，不重新实现。

`console_putc.c`、`panic_arch.c`、x86_64 的 `grub.cfg` 这几个文件和 Lab3 完全一样，直接带过来，本 README 不重复讲解。

## 核心概念

**分页是一份约定好格式的内存，不是一段代码。** 打开分页硬件（x86_64 写 CR3+CR0.PG，riscv64 写 satp）之后，CPU 每次访存都会先查这份内存里的数据，按照架构手册规定的位布局把它解释成"下一级页表在哪"或者"这是叶子，物理地址是多少"。页表本身也是普通的物理内存页，用 `kalloc_page()` 分配、用指针写入——教学上最容易翻车的地方就是这里：页表节点的地址什么时候可以直接当指针解引用，什么时候不能，取决于当前生效的那份页表有没有把这个物理地址映射到某个虚拟地址上（见下面"分步实现步骤"里两个架构都踩过的 `walk()` 解引用问题）。

**x86_64 4 级页表**：PML4 → PDPT → PD → PT，每级 512 项、每项 8 字节，一张表正好一页（4KiB）。虚拟地址的 bit 47 及以上必须是 bit 47 的符号扩展（canonical address），否则访存直接触发 #GP，不会进入分页翻译——这不是软件约定，是 Intel SDM 规定的硬件行为。PTE 格式是"标志位在低位、物理地址在高位"：bit 0 是 Present，bit 1 是 R/W（1=可写），bit 2 是 U/S，bit 63 是 NX（1=不可执行，历史包袱：默认可执行，要显式关闭，需要 `EFER.NXE` 置位才真正生效）。中间层节点靠 Present+RW+US 三个位开门，权限最终由最后一级收紧。

**riscv64 Sv39 3 级页表**：L2 → L1 → L0，同样每级 512 项、每项 8 字节，但只有 39 位虚拟地址（比 x86_64 少一级）。canonical address 的分界线是 bit 38（本课程选的内核虚拟基址 `0xFFFFFFC000000000` 对应顶层页表 index 511）。PTE 格式和 x86_64 完全不是"位置不同"这么简单：物理地址整体右移 2 位存进 PPN 字段（因为 PPN 单位是"页帧号"不是"字节地址"），而且用 R/W/X 三个独立位区分"这是指向下一级的指针"还是"这是叶子"——R=W=X=0 表示中间层指针，任意一个非 0 表示叶子。这意味着 riscv64 的中间层节点权限位必须清零，不像 x86_64 那样可以开着走，一旦不小心把中间层的 RWX 也设了，CPU 会把这一项当叶子处理，翻译提前终止，读出来的物理地址是错的。

**高半区内核（higher-half kernel）**：内核链接地址（linker.ld 里的 `. = KERNEL_VIRT_BASE`）和实际加载的物理地址（x86_64 是 `0x100000`，riscv64 是 `0x80200000`）不再相等，中间差一个固定偏移（`KERNEL_VIRT_BASE - KERNEL_LOAD_ADDR`）。这要求编译器生成的每一条访问全局变量/函数的指令，都知道自己"活"在高地址——x86_64 用 `-mcmodel=kernel`（外部符号取地址走绝对立即数 `mov $imm32`），riscv64 用 `-mcmodel=medany`（外部符号取地址走 PC 相对的 `auipc+addi`，要求符号和当前 PC 差距在 ±2GiB 内，这就是为什么 riscv64 版本不能直接用 boot.S 里给低地址用的 `__kernel_phys_end`，必须另外定义一个高 VMA 的 `__kernel_end`，具体见 riscv64 `kernel_main.c` 顶部注释）。跳转到高地址执行，本身要求"跳转指令执行的那一刻，PC 所在的高地址在当前生效的页表里已经有映射"——这是一个先有鸡还是先有蛋的问题，两个架构分别用一份*临时*页表（在 boot.S 里手写，只覆盖前 1-2GiB，同时做低地址身份映射和高地址自映射）解决：先靠临时表跳过去，再在 C 代码里建一份*正式*的页表（只有高地址自映射，不含任何低地址项），最后调 `pagetable_activate()` 切换——从这一刻起，低地址身份映射永久失效，不是被显式清除某一项，而是从来没有被写进新表。

**缺页异常的落地方式两边完全不同，但目的一致**：x86_64 靠 IDT（Interrupt Descriptor Table）第 14 项指向 `page_fault_stub`（汇编入口），出错地址由 CPU 自动写入 `CR2`，error code 由硬件自动压栈；riscv64 靠 `stvec` 寄存器指向唯一一个 `supervisor_trap_entry`（Direct 模式，所有 trap 不分青红皂白先跳到这一个地址，分发逻辑全部在软件里用 `scause` 判断），出错地址由 CPU 写入 `stval`，没有硬件压栈的 error code（`scause`/`stval` 都是普通 CSR，C 函数体内随时可以自己读）。两边的汇编 stub 都要手动保存全部可能被打乱的寄存器（硬件不会像调用约定那样自动保存），并且都不能用普通的函数返回指令（x86_64 用 `iretq`，riscv64 用 `sret`）。

## x86_64 与 riscv64 对照表

| 维度 | x86_64 | riscv64 |
|---|---|---|
| 页表级数 | 4 级（PML4/PDPT/PD/PT） | 3 级（L2/L1/L0，Sv39） |
| 虚拟地址有效位数 | 48 位 | 39 位 |
| canonical address 分界线 | bit 47 | bit 38 |
| 页表根寄存器 | CR3 | satp（bit 63-60 是 MODE，Sv39=8） |
| 换根/开分页要不要显式刷 TLB | 不需要——写 CR3 隐式刷 | 需要——`sfence.vma`，写 satp 不会自动刷 |
| PTE 里权限位的含义 | Present/R-W/U-S/NX，NX=1 才是不可执行（默认可执行） | V/R/W/X/U，X=1 才可执行（默认不可执行） |
| 叶子节点识别方式 | 页表层级 + PS 位 | RWX 是否全零（全零=中间层指针，非零=叶子） |
| 物理地址在 PTE 里怎么编码 | 直接对齐存放（掩掉低位标志位） | 右移 2 位后存进 PPN 字段（单位是页帧号不是字节） |
| 触发缺页时地址存哪 | CR2 | stval |
| 硬件是否自动压栈 error code | 是（#PF 专属） | 否（scause/stval 都是普通 CSR，软件自己读） |
| 异常返回指令 | iretq | sret |
| 内存布局大模型选项 | `-mcmodel=kernel`（绝对立即数取地址） | `-mcmodel=medany`（PC 相对取地址，±2GiB 限制） |
| 分页从什么时候开始 | 从 Lab2 起就是开着的（本 Lab 只是换根） | 到 Lab3 为止一直是 bare mode（本 Lab 第一次真正打开） |

riscv64 到 Lab3 为止从未打开过分页（`satp=0`，直接物理地址寻址），所以本 Lab 对 riscv64 来说是"第一次开"，而 x86_64 从 Lab2 长模式切换那一刻起分页就已经开着（Lab1-3 都是恒等映射），本 Lab 只是构造一份新的、不含低地址身份映射的页表然后换根——这也是为什么 riscv64 版本的 boot.S 要在阶段 A 就手写一份完整的临时页表（连低地址身份映射都要从零搭），而 x86_64 版本相对而言只是在 Lab2 已有的临时页表基础上多插一项高地址映射。

## 代码目录与关键文件

```
labs/lab04-virtual-memory/
├── Makefile                     # 构建入口，新增 pagetable.c/trap.c/trap_entry.S
├── README.md                    # 本文件
├── solution/
│   ├── x86_64/
│   │   ├── boot.S                # 新增阶段 B（高地址自映射项）+ 阶段 C（跳高地址）
│   │   ├── linker.ld             # 新增 __boot_load_addr/__kernel_phys_end（物理地址符号）
│   │   ├── memmap.c              # 内存探测 + 排除内核镜像占用的物理页
│   │   ├── pagetable.c           # 4 级页表：建表/查表/激活
│   │   ├── trap.c                # IDT 初始化 + #PF 处理函数
│   │   ├── trap_entry.S          # #PF 汇编入口 stub（保存寄存器/取 error code/iretq）
│   │   ├── kernel_main.c         # 建正式页表→切换→验证映射→触发缺页
│   │   ├── console_putc.c        # 和 Lab3 完全一样
│   │   ├── panic_arch.c          # 和 Lab3 完全一样
│   │   └── grub.cfg              # 和 Lab3 完全一样
│   └── riscv64/
│       ├── boot.S                # 全新：从零搭临时 Sv39 页表 + 打开分页 + 跳高地址
│       ├── linker.ld             # 新增 __kernel_end（高 VMA 符号，供 kernel_main.c 用）
│       ├── memmap.c              # 内存探测（阶段 A 调用，早于分页打开）
│       ├── pagetable.c           # 3 级 Sv39 页表：建表/查表/激活
│       ├── trap.c                # trap_init + supervisor_trap_handler
│       ├── trap_entry.S          # trap 汇编入口 stub（保存寄存器/call/sret）
│       ├── kernel_main.c         # 建正式页表→切换→验证映射→触发缺页
│       ├── console_putc.c        # 和 Lab3 完全一样
│       └── panic_arch.c          # 和 Lab3 完全一样
├── starter/                      # 结构和 solution 一一对应，教学文件带 TODO
│   ├── x86_64/
│   └── riscv64/
└── tests/
    ├── expect-x86_64.txt
    └── expect-riscv64.txt
```

`pagetable.h`（架构无关的接口声明：`pagetable_map`/`pagetable_lookup`/`pagetable_create`/`pagetable_activate`）在 `src/common/include/`，和 Lab3 的 `kalloc.h` 同一个分工原则——只声明"做什么"，"怎么做"留给各架构的 `pagetable.c`。`pagetable.c`/`trap.c`/`trap_entry.S`/`boot.S` 这四个文件因为两个架构的硬件细节差异太大（页表级数、PTE 位布局、中断向量机制全都不同），没有放进 `src/common/`，各自在 `solution/<arch>/` 下独立实现。

## 分步实现步骤

### x86_64 路线

1. **linker.ld**：把 `. = KERNEL_LOAD_ADDR` 改成 `. = KERNEL_VIRT_BASE`（高地址），用 `AT(KERNEL_LOAD_ADDR + (. - KERNEL_VIRT_BASE))` 之类的表达式让每个 section 的 LMA（加载地址）仍然落在低物理地址，VMA（链接地址）落在高地址——这是"链接地址和加载地址分离"的标准做法。额外定义两个物理地址符号：`__boot_load_addr`（在设置 VMA 之前定义，此刻 VMA=LMA=低地址）和 `__kernel_phys_end`（`LOADADDR(.bss) + bss 大小`），专门给 boot.S 在分页还没打开、PC 还在低地址的阶段用——不能和高 VMA 的符号混用，见 x86_64 `kernel_main.c` 里对这个区别的详细注释。
2. **boot.S 阶段 B**：在 Lab2/3 已有的临时页表基础上，往 PML4 的 index 511（对应 `0xFFFFFFFF80000000` 起）也插一项，指向和低地址身份映射*同一份* PD——这样临时表同时覆盖低 1GiB 身份映射和高地址自映射，两者指向完全相同的物理页。**这里最容易犯的错是 PDPT 的索引写错**：`0xFFFFFFFF80000000` 对应的 PDPT index 是 510，不是 0——如果沿用低地址那份的 index 0，会把高地址映射悄悄叠在低地址映射的同一个 PDPT 项上，实测表现为"看起来能跳过去，但映射范围是错的"。
3. **boot.S 阶段 C**：用 `movabs` 把一个高地址标号的绝对地址装进寄存器，`jmp` 过去——这一步之后 PC 第一次落在链接器算的"真实"链接地址上，RIP 相对寻址（编译出来的 C 代码全部靠这个）才第一次生效，`__stack_top` 也要从这里开始切过去（不能在此之前用，此刻它还是一个高地址，用低地址的临时栈还没跳过去）。
4. **pagetable.c**：实现 `walk()`（4 级里前 3 级，中间层缺失就用 `kalloc_page()` 现场分配、清零、挂上去）、`pagetable_map()`/`pagetable_lookup()`（走完 4 级，最后一级单独处理）、`pagetable_create()`（`kalloc_page()` + 清零）、`pagetable_activate()`（写 CR3，x86_64 换根会隐式刷 TLB，不需要额外指令）。
5. **kernel_main.c**：`pagetable_create()` 建一份新表，循环 `pagetable_map()` 把内核镜像整段（`__boot_load_addr` 到 `__kernel_phys_end`）按 `VA = PA + KERNEL_VIRT_BASE` 的关系自映射，再手写一段"非身份"映射（VA 和 PA 不满足这个线性关系，用来验证 `pagetable_lookup()` 翻译对不对），`pagetable_activate()` 切换过去，然后调用 `pagetable_lookup()` 验证映射关系。
6. **trap.c + trap_entry.S**：`idt_init()` 只填第 14 项（#PF），指向 `page_fault_stub`；`page_fault_stub`（汇编）保存全部通用寄存器、从栈上取出硬件压栈的 error code 放进 `%rdi`、调用 `page_fault_handler()`；`page_fault_handler()`（C）读 `CR2` 拿出错地址，解析 error code 的 P/W/U 三个位，`kprintf` 报告后 panic。**必须在 `pagetable_activate()` 之后立刻调用一次 `gdt_init()`（重新加载一份高 VMA 的 GDT）**——原因见下面"常见坑与排查"，这是本 Lab 最容易漏、后果最隐蔽的一步。
7. **kernel_main.c 收尾**：调用 `idt_init()`，故意读一个从未映射过的地址（`0xdead0000`）触发 #PF，验证整条链路真的会被调用到。

### riscv64 路线

1. **linker.ld**：和 x86_64 同样的"VMA/LMA 分离"思路，但 riscv64 从 Lab1 起就没有这种分离（一直是恒等映射），这是第一次引入。额外定义 `__kernel_end`（高 VMA 符号，`-mcmodel=medany` 下 `kernel_main.c` 用它算自映射范围，不能用给 boot.S 低地址阶段专用的物理地址符号，理由见"核心概念"）。
2. **boot.S 阶段 A（全新）**：riscv64 到 Lab3 为止完全没有任何页表，这一步要从零搭一份临时 Sv39 页表，同时做"低 1GiB 身份映射"和"高地址前 1GiB 自映射"（riscv64 没有 x86_64 那种"必须先只做低映射，过会再插高地址项"的顺序要求，分页此刻还没打开，随便什么顺序填页表都不影响"当前生效的映射"，可以一次性填好两份）。**这一步要在打开分页之前调用 `memmap_discover()`**——原因是 QEMU 把 riscv64 的 DTB 放在"RAM 顶部往下 2MiB"的地方（远超临时页表覆盖的低 2MiB 范围），必须趁 `satp=0`（bare mode，任何 64 位地址都能直接当物理地址解引用）的窗口读掉，晚了会直接缺页。
3. **boot.S 阶段 B（全新）**：`csrw satp` 第一次真正打开 Sv39（不是换根），紧跟一条 `sfence.vma zero, zero`——riscv64 写 satp *不会*隐式刷 TLB，这是和 x86_64 CR3 的关键差异，漏掉这一条在 x86_64 背景的开发者身上是最容易犯的错，因为"忘记刷 TLB"在 x86_64 上往往不会立刻炸（换 CR3 顺带刷了），换到 riscv64 上会直接读到脏数据。
4. **boot.S 阶段 C**：跳到高地址执行，`__stack_top` 从这里开始切过去，和 x86_64 阶段 C 同一个道理。
5. **pagetable.c**：`walk()` 只需要循环 2 级（L2、L1，Sv39 只有 3 级），中间层新建节点的标志位必须*只有* V 位（R=W=X 全零，否则会被 CPU 当成叶子，翻译提前终止）；`pagetable_map()`/`pagetable_lookup()` 单独处理最后一级 L0；`pagetable_activate()` 写 satp（`(8ull << 60) | (root >> 12)`）后紧跟 `sfence.vma zero, zero`。**`walk()` 内部解引用页表节点时必须走 `phys + KERNEL_VIRT_BASE` 这个别名，不能直接拿物理地址当指针**——切换到正式页表之后，物理地址不再等于任何合法虚拟地址，这个坑两个架构都踩过，详见下面"常见坑与排查"。
6. **kernel_main.c**：和 x86_64 同一个流程（建表→自映射内核镜像→非身份映射一页→激活→验证），但自映射范围要故意比内核镜像本身多留到 2MiB 窗口边界（覆盖 `kalloc_page()` 现场分配的中间层节点物理地址），并且要额外单独映射 UART 的物理地址（riscv64 的串口是内存映射 I/O，激活新页表之后不显式映射就无法访问，不像 x86_64 版本走端口指令天然绕开页表）。
7. **trap.c + trap_entry.S**：`trap_init()` 写 `stvec`（Direct 模式，值 0）指向 `supervisor_trap_entry`；`supervisor_trap_entry`（汇编）保存全部 caller-saved 寄存器、`call supervisor_trap_handler`；`supervisor_trap_handler()`（C）读 `scause`/`stval`，只识别取指/读/写三种 page fault（12/13/15），其它一律 panic。
8. **kernel_main.c 收尾**：调用 `trap_init()`，故意读 `0xdead0000` 触发缺页。

## QEMU 运行命令

```bash
cd labs/lab04-virtual-memory
make ARCH=x86_64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=x86_64 LAB=lab04-virtual-memory VARIANT=solution TIMEOUT=15
```

x86_64 预期输出：

```
Hello OS from x86_64 (Lab4: virtual memory)
memmap: 2 available region(s) from Multiboot2 mmap tag, 32468 page(s) free
kernel image: phys [0x100000, 0x10c000)
switched to Lab4 page table, low identity map gone
non-identity mapping OK: VA=0xffffffff90000000 -> PA=0x100000
kernel self-map verified
idt_init() done, about to trigger a deliberate page fault
page fault: addr=0xdead0000 error_code=0 (not-present,read,kernel)

*** KERNEL PANIC ***
  at labs/lab04-virtual-memory/solution/x86_64/trap.c:161
  page_fault_handler: unrecoverable page fault (Lab4 does not implement fault recovery)
System halted.
```

```bash
make ARCH=riscv64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab04-virtual-memory VARIANT=solution TIMEOUT=15
```

riscv64 预期输出：

```
memmap: 1 region(s) from DTB /memory, 32217 page(s) free (kernel image excluded)
Hello OS from riscv64 (Lab4: virtual memory)
kernel image: phys [0x80200000, 0x80227000)
switched to Lab4 page table, low identity map gone
non-identity mapping OK: VA=0xffffffc010000000 -> PA=0x80200000
kernel self-map verified
trap_init() done, about to trigger a deliberate page fault
page fault: addr=0xdead0000 reason=read (scause=13)

*** KERNEL PANIC ***
  at labs/lab04-virtual-memory/solution/riscv64/trap.c:85
  supervisor_trap_handler: unrecoverable page fault (Lab4 does not implement fault recovery)
System halted.
```

空闲页数、内核镜像结束地址这类数字取决于 QEMU 具体分配的内存布局和编译器版本，不要求逐字节对应——`memmap:`/`kernel image:` 那两行的具体数值，只要和自己本地构建的结果一致就行，`tests/expect-*.txt` 用子串匹配，不检查这些会变的数字。riscv64 版本的 `memmap:`/`Hello OS...` 两行顺序和 x86_64 相反（`memmap_discover()` 在阶段 A 就调用过了，早于 `console_puts_line("Hello OS...")`），这是设计决定不是 bug，见上面"核心概念"。

## GDB/QEMU Monitor 调试方法

```bash
bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab04-virtual-memory VARIANT=solution
```

这个脚本会启动 QEMU 并挂起等待 GDB 连接，另开一个终端跑对应架构的 gdb（x86_64 用 `x86_64-elf-gdb`，riscv64 用 `riscv64-unknown-elf-gdb`），常用命令：

```gdb
target remote :1234
file solution/x86_64/build/kernel.elf
break pagetable_activate
continue
print/x $cr3          # 查看当前页表根（riscv64 换成 print/x $satp）
break page_fault_handler
continue
print/x $rdi          # x86_64：进入时 %rdi 是 error code
info registers cr2    # x86_64：CR2 是出错的虚拟地址
```

排查页表翻译问题最有用的单条命令是 QEMU monitor 里的 `info tlb`（如果目标架构支持）或者直接读页表节点的原始内容：`x/8gx <root 物理地址转出来的可访问虚拟地址>`，逐级手动核对 PTE 的值是不是符合预期的位布局——比 单步反复"猜"更快定位到底是哪一级建错了。

想确认某次访存到底是被硬件判定成"未映射"还是"映射了但权限不对"，在触发缺页之前 `break` 在 `pagetable_map` 的调用点，核对传进去的 `flags` 参数是不是本来想要的那个组合（比如误传成 `PTE_FLAG_WRITABLE` 却忘了 `PTE_FLAG_EXECUTABLE`，会导致取指触发 page fault，容易被误判成"整个映射都不存在"）。

## 自动验收测试

```bash
cd labs/lab04-virtual-memory
bash ../../scripts/test-lab.sh ARCH=x86_64 LAB=lab04-virtual-memory
bash ../../scripts/test-lab.sh ARCH=riscv64 LAB=lab04-virtual-memory
```

`tests/expect-*.txt` 用逐行子串匹配（不要求整行完全一致，也不要求文件里的每一行都在输出里出现在相邻位置，只要求按顺序、作为子串出现），所以 starter 版本实现完成后即使空闲页数、内核镜像结束地址这些和 solution 的具体数值不完全一样也没关系，只要关键行的固定文本部分一致就能通过。

## 常见坑与排查

- **x86_64：PDPT index 写错（0 而不是 510）**。`0xFFFFFFFF80000000` 对应的 PDPT 索引是 510，不是沿用低地址身份映射的 0——如果写错，高地址跳转"看起来"能成功（因为 index 0 那份 PD 恰好也有效映射），但实际映射到的物理范围是错的，后续访问会读到看似合理却实际错误的数据，比直接崩溃更难排查。用 GDB 在 `jmp` 到高地址标号之前 `print/x` 核对 PML4[511] 的值。
- **两个架构的 linker.ld 都出现过 PT_LOAD 段覆盖不全的问题**：如果 `.bss` 或者某个 section 没有被划进任何 PT_LOAD 段的范围内，加载器（GRUB/OpenSBI）不会把它清零/加载，运行时读到的是未初始化的垃圾——用 `objdump -p kernel.elf` 核对 PT_LOAD 段的 `VirtAddr`/`FileSiz`/`MemSiz` 是不是覆盖了链接脚本里定义的每一个 section。
- **riscv64：Sv39 顶层/中间层页表的 index 算错（`boot_l2`/`boot_l1` 数组下标）**。手写临时页表时，`shift` 用错、或者 index 提取的位宽算错（应该是 9 位掩码 `& 0x1FF`），会导致填的是这张表的某个不相关槽位，实际生效的映射范围不是预期的那一段——这类 bug 的典型症状是"跳转之后立刻卡死或者读到垃圾"，用 GDB 在 `csrw satp` 之前手动 dump 整张临时页表逐项核对。
- **riscv64：`fill_leaf` 之类的批量填表函数漏了 PTE 编码的位移**（忘记把物理地址 `>>12<<10`，直接把物理地址塞进 PTE 会让 PPN 字段的值差了 2 个二进制位的量级，翻译出来的物理地址完全不对）。riscv64 的 PPN 编码不是"直接对齐存放"，这一点和 x86_64 不一样，必须显式调用 `paddr_to_ppn_bits()`/`ppn_bits_to_paddr()` 这一对互逆的小函数，不要在调用点里直接手写位移。
- **riscv64：UART 的 MMIO 映射完全漏掉，或者映射了但漏了 W 位（`flags` 传成 `0x3` 该是 `0x7`，或者反过来传漏了某个权限位）**。`pagetable_activate()` 切换之后，串口输出会立刻卡死或者直接触发一次 store page fault——如果这个现象发生在"switched to Lab4 page table"这行打印之后就再没有任何输出，基本可以确定是 UART 的映射漏了或者权限不对，检查 `pagetable_map(root, 0x10000000ull, 0x10000000ull, ...)` 那一行的 `flags` 参数。
- **两个架构都出现过的设计级 bug：`walk()` 在 `pagetable_activate()` 之后直接拿物理地址当指针解引用**。页表节点的地址（`root` 参数、中间层新建节点的地址）全部以*物理地址*的形式存储和传递，`walk()` 内部要 `table[idx]` 这样直接解引用它们，这只有在"物理地址本身可以直接当指针用"这个前提下才成立——切换到正式页表之后，如果这份新表没有覆盖到页表节点自身所在的物理地址，这个前提就不满足了，`walk()` 第一次解引用就会立刻缺页（实测在 QEMU 下确认过，`load_page_fault`/`#PF` 的出错地址正好落在 `kalloc` 空闲池刚分配出去的头几页）。修复方式是让 `walk()` 统一通过 `phys + KERNEL_VIRT_BASE` 这个别名去访问表节点，并且要求 `kernel_main.c` 的自映射范围覆盖到这些页表节点实际会用到的物理地址（不能只覆盖到内核镜像本身），两处必须配合，单改一处不够。
- **x86_64 专属、最隐蔽的一个：GDT 从来没有被重新加载，导致触发第一个异常时直接三次故障重启**。`boot.S` 里的 `gdt64`/`gdt64_pointer` 定义在低物理地址（`.text.boot`，为了让阶段 A/B 的代码在跳到高地址之前能正常执行），`lgdt` 只在启动最早期执行过一次。`pagetable_activate()` 切换到 Lab4 的正式页表之后，这份 GDT 所在的物理地址在新表里没有任何映射——但 Intel SDM 规定，通过 IDT 的 interrupt gate 递交*任何*异常/中断都必须重新从 GDT 读取门描述符引用的代码段描述符、重新加载 CS，这在硬件层面每次异常递交都会发生，不是"不用它就没事"。结果是第一次触发缺页（本 Lab 故意触发的那一次）在异常递交过程中读 GDT 就直接缺页，级联成 #DF（同样没有对应的 IDT 项，因为本 Lab 只填了第 14 项），再级联成三次故障，QEMU 直接重启虚拟机，甚至走不到 `page_fault_handler()` 里面。用 `qemu-system-x86_64 ... -d int,cpu_reset -D debug.log` 能在日志里看到 `v=0e`（#PF）后面紧跟一个 `v=08`（#DF），IP 完全没变（说明异常递交本身就没走完），`CR2` 的值正好等于日志里 `GDT=` 那一行给出的基址加 8（GDT 第二项，也就是选择子 `0x08` 对应的那一项）——这是确诊的关键证据，不是猜测。修复方式是在 `trap.c` 里新增一个 `gdt_init()`，定义一份普通的高 VMA 静态数组当新 GDT（和 `idt[]` 数组本身从来不需要这种处理是同一个道理——它是普通 `.bss`/`.data`，天然落在高地址，本身就在新页表的自映射范围内），在 `pagetable_activate()` 之后、任何异常有可能被触发之前立刻调用。riscv64 没有 GDT 或者任何等价的段描述符表概念，这个 bug 没有 riscv64 版本。

## 挑战任务

- 给 `pagetable_map()` 增加大页支持（x86_64 的 PD 项 PS 位映射 2MiB 页，riscv64 的 Sv39 在 L1 层级就可以是叶子映射 2MiB 页），思考大页在什么场景下比逐页映射更有意义（提示：页表节点本身占用的物理内存和 TLB 命中率）。
- 实现一个 `pagetable_unmap()`，把某个虚拟地址的映射标记成无效——注意要不要连带释放这一页物理内存，要不要在中间层页表全部变空之后回收中间层节点本身（自己决定要不要做，两种取舍都有真实内核会采用）。
- x86_64 的 `page_fault_handler()`/riscv64 的 `supervisor_trap_handler()` 目前只会 panic，尝试改成"打印完整的错误信息但不 panic，直接跳过出错的那条指令继续执行"（提示：需要在陷入帧里手动调整 RIP/sepc，想清楚这样做在教学场景之外几乎从来不是正确的策略，仅作为理解陷入返回机制的练习）。
- 用 QEMU monitor 或者 GDB 手动 dump 出激活后的完整页表内容，写一个脚本自动校验"自映射范围内的每一页，VA-PA 的差值都恰好等于 KERNEL_VIRT_BASE"这条不变量，理解为什么这条不变量在本 Lab 的设计下必须成立。

## 参考

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Chapter 4 "Paging"（x86_64 页表格式的权威定义）
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Chapter 6 "Interrupt and Exception Handling"，尤其 6.12.1 节（本 Lab GDT bug 的根本依据）
- The RISC-V Instruction Set Manual, Volume II: Privileged Architecture，"Sv39: Page-Based 39-bit Virtual-Memory System" 一节（riscv64 页表格式的权威定义）
- The RISC-V Instruction Set Manual, Volume II: Privileged Architecture，"Machine Cause Register (mcause)" 一节（scause 编码，S 模式布局相同）

## 下一步

进入 [Lab5：中断、异常、trap、时钟](../lab05-interrupt-trap/README.md)——在本 Lab 搭好的最窄陷入基础设施上，扩展成完整的中断分发框架（x86_64 补全 IDT 剩余向量、riscv64 引入 Vectored 模式的可能性），接入可编程定时器，为后面的进程调度打基础。
