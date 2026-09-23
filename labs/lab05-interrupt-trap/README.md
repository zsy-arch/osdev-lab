# Lab5：中断、异常、trap、时钟

## 学习目标

- 理解"异常"（exception，CPU 执行指令本身触发的，比如 Lab4 的 #PF）和"中断"（interrupt，外部设备异步触发的，本 Lab 的定时器）在硬件递交机制上的相同点（都要保存现场、跳到固定入口、都不能用普通函数返回指令）和不同点（有没有硬件自动压栈的 error code，需不需要软件显式告诉硬件"处理完了"）。
- 亲手驱动一个可编程定时器：x86_64 是 legacy PIT（8253/8254）+ 8259A PIC，riscv64 是 SBI TIME extension（通过 ecall 请求 M-mode/OpenSBI 代劳编程 CLINT），理解两边"定时器"这个概念在硬件层面几乎没有共同点，只有"能周期性触发一个 trap"这个行为是共同的。
- 把内核主循环从 Lab4 的"跑完一次性验证就 panic"改造成第一次真正持续运行的形态——用 `hlt`/`wfi` 让 CPU 在等待中断时休眠，而不是忙等。
- 通过这个 Lab 的开发过程本身，理解"编译通过、QEMU 里跑起来不崩溃"和"行为真的符合设计"是两件不同的事——本 Lab 的三个真实 bug（见"常见坑与排查"）全部是"表面正常但实际错误"的类型，不是编译错误或者立即崩溃。

## 前置 Lab

依赖 [Lab4：虚拟内存](../lab04-virtual-memory/README.md)——本 Lab 复用 Lab4 建好的页表框架（`pagetable_create`/`pagetable_map`/`pagetable_activate`/`pagetable_lookup` 原样调用，不重新实现），IDT/GDT（x86_64）、`stvec`/Direct 模式（riscv64）的基础设施在 Lab4 已经搭好，本 Lab 只是往上加新的向量/中断源。

`boot.S`、`linker.ld`、`console_putc.c`、`panic_arch.c`（两个架构）、`memmap.c`、`pagetable.c`（riscv64）、x86_64 的 `grub.cfg` 这几个文件和 Lab4 完全一样，直接带过来，本 README 不重复讲解。

## 核心概念

**异常是同步的，中断是异步的，但硬件递交机制的骨架相同。** 无论是 Lab4 的 #PF（CPU 执行一条访存指令时自己发现"这个地址没映射"）还是本 Lab 的定时器中断（跟 CPU 正在执行的指令毫无关系，纯粹是外部时间到了），硬件的反应都是同一套流程：保存当前执行状态（PC/flags/栈指针这些）、跳到一个固定的软件入口、软件处理完之后用专门的返回指令（不是普通的 `ret`/`jr ra`）恢复现场继续执行。两者的差异集中在两点：有没有硬件自动压栈的额外信息（x86_64 的某些异常有 error code，中断没有；riscv64 全部靠 CSR，压根没有硬件压栈这个概念），以及处理完之后需不需要显式告诉产生这个事件的硬件"我处理完了"（外部中断源，比如 x86_64 的 PIC，需要软件发 EOI，否则会认为"上一个还没处理完"卡住不再递送；CPU 自己产生的异常不需要，`iretq`/`sret` 就结束了）。

**x86_64 的定时器链路：8259A PIC 重映射 + 8254 PIT 编程 + IDT 注册 + sti，四层缺一不可。** PIC 出厂默认把 IRQ0-15 映射到向量 0x08-0x0F/0x70-0x77，跟 CPU 自己的异常向量（0x00-0x1F）重叠——这是 IBM PC/XT 时代的历史遗留问题，所有 x86 内核都要重映射到 0x20-0x2F 才能用。PIT 是独立于 PIC 的另一块芯片，负责"多久触发一次"，PIC 负责"触发的信号往哪个向量号递送"，两者要分别编程。IDT 填好定时器向量之后，还差 `sti`（置 RFLAGS.IF）这最后一步——CPU 复位后 IF 默认是 0，此时会无条件忽略所有可屏蔽外部中断（异常不受 IF 影响，这也是为什么 Lab4 全程没开中断、#PF 依然能正常递送）。四层任何一层没做，定时器中断都不会被内核观察到，但失败的表现形式完全不同：PIC 没重映射会把定时器信号误当成 #DF；IDT 没填对应向量会在中断真正到达时触发 #GP 再级联 #DF；没 sti 则是"什么都不会发生，主循环卡死不打印"。

**riscv64 的定时器链路：SBI ecall + sie/sstatus 两个开关，且需要每次手动重新预约下一次触发。** riscv64 的 CLINT（管 mtime/mtimecmp）在 QEMU virt 平台上属于 M-mode 地址空间，S-mode 内核不能直接访问，必须通过 SBI（M-mode 固件对 S-mode 暴露的服务接口）用 `ecall` 请求代劳——这是本课程第一次用到 `ecall`。SBI 的二进制调用约定是 `a7=EID, a6=FID, a0-a5=参数`，用 C 变量普通命名成 `a6`/`a7` **不会**让编译器把它们分配到那两个物理寄存器（这是本 Lab 的第一个真实 bug，见下面"常见坑与排查"），必须用 GCC 显式寄存器变量语法。riscv64 和 x86_64 PIT mode 2 最大的行为差异是：8254 一旦设好 reload value 会自己周期性重触发，而 riscv64 的 `sbi_set_timer()` 只预约*一次*，每次处理完这次中断都必须在 handler 里重新调用一次 `sbi_set_timer()` 预约下一次，用当前 `time` CSR 的值（不是"上一次预约的值"）加上间隔作为新的目标，避免处理这次中断本身耗费的时间被累积计入下一个间隔。

**"能编译、能跑、不崩溃"不等于"正确"——本 Lab 在开发过程中依次踩中三个都属于这一类的真实 bug。** 一个 8 字节的越界写（x86_64 IDT 数组大小差 1）命中的正好是紧跟在数组后面的另一个静态变量，不会立刻崩溃，而是延迟到下一次中断递交才表现为诡异的级联异常；一个寄存器绑定失误（riscv64 `sbi_call` 用普通变量名"撞"寄存器名）会让 `ecall` 静默地带着陈旧寄存器值执行，SBI 固件可能"恰好"没有因为非法参数报错，表现为"看起来在正常触发中断，实际触发的时间点完全不受控"；一个链接脚本的输入 section 匹配遗漏（riscv64 `.sbss`/`.sdata` 未被 `linker.ld` 匹配）会让一个全局变量被链接器放到内核镜像描述的物理范围*之外*，之后被内存分配器当成空闲页发出去覆写——三者的共同点是：错误的初始症状（tick 打印的数字不对/PIC 重映射看起来没生效/QEMU 输出在几秒内打出上百个 tick）都不直接指向真正的根因，需要用 `objdump`/`nm` 核对符号实际落在哪个 section、用 GDB 附加断点核对寄存器实际内容，才能把"表面症状"和"真正原因"对上。

## x86_64 与 riscv64 对照表

| 维度 | x86_64 | riscv64 |
|---|---|---|
| 定时器硬件 | 8253/8254 PIT（独立芯片） | CLINT 的 mtime/mtimecmp（通过 SBI 间接编程，S-mode 不能直接访问） |
| 中断信号路由 | 8259A PIC（需要重映射向量偏移） | 无需路由芯片，直接是 CSR `sie`/`sip` 里的位 |
| 触发方式 | 硬件自动周期性重触发（mode 2 rate generator） | 软件每次处理完手动预约下一次（`sbi_set_timer`） |
| 中断使能的开关层数 | 3 层：IDT entry 填好 + PIC unmask + `sti`（RFLAGS.IF） | 2 层：`sie`.STIE 位 + `sstatus`.SIE 位 |
| 处理完是否需要显式确认 | 需要：PIC EOI（`outb(PIC1_CMD, 0x20)`），否则该 PIC 卡死不再递送任何中断 | 不需要：CSR 层面没有"确认收到"这个概念 |
| 中断向量的分发方式 | IDT 按向量号分发到不同入口地址（本 Lab 新增 `timer_stub`） | Direct 模式所有 trap 共享同一入口（`trap_entry.S` 不需要改，软件在 C 里靠 `scause` 分发） |
| 判断"是中断还是异常"的依据 | 向量号范围（0-31 CPU 保留，32+ 是重映射后的 IRQ） | `scause` 最高位（1=中断，0=异常） |
| 目标频率 | 100Hz（`pit.c` 的 `TIMER_HZ`） | 100Hz（`trap.c` 的 `TIMER_HZ`，两边保持对等） |
| 主循环等待指令 | `hlt` | `wfi` |

两边在"每秒打印一次 tick"这个可观察行为上完全对等，但硬件实现路径几乎没有相似之处——这正是本 Lab 想传达的教学要点：操作系统需要的"周期性事件"这个抽象需求是通用的，但满足这个需求要写的代码，在不同架构上几乎是从零开始的两套逻辑，没有多少可以共享。

## 代码目录与关键文件

```
labs/lab05-interrupt-trap/
├── Makefile                     # 构建入口，x86_64 新增 pit.c，riscv64 新增 sbi.c
├── README.md                    # 本文件
├── solution/
│   ├── x86_64/
│   │   ├── boot.S                # 和 Lab4 完全一样
│   │   ├── linker.ld             # 和 Lab4 完全一样
│   │   ├── pit.c                 # 新增：PIC 重映射 + PIT 驱动 + timer_interrupt_handler
│   │   ├── pit.h                 # 新增：pit_init()/pit_ticks 的声明
│   │   ├── trap.c                # 在 Lab4 基础上多填一项 IDT（定时器向量 32）
│   │   ├── trap_entry.S          # 在 Lab4 基础上多一个 timer_stub（无 error code）
│   │   ├── kernel_main.c         # 主循环改成持续运行 + 周期性打印
│   │   ├── console_putc.c        # 和 Lab4 完全一样
│   │   ├── panic_arch.c          # 和 Lab4 完全一样
│   │   ├── memmap.c              # 和 Lab4 完全一样
│   │   ├── pagetable.c           # 和 Lab4 完全一样
│   │   └── grub.cfg              # 和 Lab4 完全一样
│   └── riscv64/
│       ├── boot.S                # 和 Lab4 完全一样
│       ├── linker.ld             # 在 Lab4 基础上新增 .sdata/.sbss 的显式匹配（见常见坑）
│       ├── sbi.c                 # 新增：SBI ecall 包装 + sbi_set_timer
│       ├── sbi.h                 # 新增：sbi_set_timer() 的声明
│       ├── trap.c                # 在 Lab4 基础上扩展中断分发（scause 最高位）+ timer_enable
│       ├── trap_entry.S          # 和 Lab4 完全一样（Direct 模式无需新增 stub）
│       ├── kernel_main.c         # 主循环改成持续运行 + 周期性打印
│       ├── console_putc.c        # 和 Lab4 完全一样
│       ├── panic_arch.c          # 和 Lab4 完全一样
│       ├── memmap.c              # 和 Lab4 完全一样
│       └── pagetable.c           # 和 Lab4 完全一样
├── starter/                      # 结构和 solution 一一对应，教学文件带 TODO
│   ├── x86_64/
│   └── riscv64/
└── tests/
    ├── expect-x86_64.txt
    └── expect-riscv64.txt
```

`pit.c`/`sbi.c` 没有放进 `src/common/`：两边的硬件差异太大（一个是独立芯片+端口 I/O，一个是通过 ecall 请求固件代劳），勉强凑一份公共接口只会是"名字一样、参数完全不同"的假抽象，跟 Lab4 里 `pagetable.c`/`trap.c` 不共享的理由完全一样。

## 分步实现步骤

### x86_64 路线

1. **pit.c**：实现 `pic_remap()`——按 ICW1-4 四步初始化序列把 IRQ0-15 从出厂默认的 0x08-0x0F/0x70-0x77 重映射到 0x20-0x2F（PIC1 用 0x20 起，PIC2 用 0x28 起），重映射前后要保存/恢复调用前的 mask，不能假设恢复出厂默认。再实现 `pic_unmask_irq0()`（只开 IRQ0，其它保持屏蔽）、`pic_send_eoi()`（8259A OCW2 的 non-specific EOI，命令码 0x20）、`timer_interrupt_handler()`（自增 `pit_ticks`，发 EOI）。最后 `pit_init()` 按顺序：`pic_remap()` → 写 PIT command byte（0x34：通道 0、lobyte/hibyte、mode 2 rate generator、二进制计数）→ 写 reload value（`PIT_INPUT_HZ / TIMER_HZ`，先低字节再高字节）→ `pic_unmask_irq0()`。
2. **trap.c**：`IDT_ENTRIES` 从 Lab4 的 32 改成 33（**不是随便加 1，是因为本 Lab 新增的定时器向量号是 32，数组必须能容纳下标 32，即大小至少 33**——这正是本 Lab 的第一个真实 bug，见"常见坑与排查"）。新增 `IDT_VEC_TIMER = 32`（PIC 重映射后 IRQ0 对应的向量号）。`idt_init()` 在填 `IDT_VEC_PAGE_FAULT` 之外，多填一项 `idt_set_entry(IDT_VEC_TIMER, timer_stub)`。
3. **trap_entry.S**：新增 `timer_stub`——结构和 `page_fault_stub` 一样先保存全部通用寄存器，但因为 IRQ0 没有硬件 error code，不需要"从栈上取一项传参"这一步，直接 `call timer_interrupt_handler`（零参数），恢复寄存器后直接 `iretq`（不需要先 `add $8,%rsp` 弹 error code）。
4. **kernel_main.c**：在 Lab4"验证页表映射正确"之后，依次调用 `idt_init()` → `pit_init()` → `sti`（这个顺序不能变：先接好"收到中断跳到哪"，再打开"允许收"，最后才允许硬件真正开始产生中断，避免中间状态不一致时冒出接不住的中断）。主循环从 Lab4 的"故意触发异常然后 panic"改成：`for(;;) { hlt; 检查 pit_ticks 是否跨过下一个整秒边界，跨过了就打印 }`。

### riscv64 路线

1. **sbi.h/sbi.c**：声明并实现 `sbi_set_timer(uint64_t stime_value)`。内部的 `sbi_call()` 要把 `eid`/`fid`/`arg0` 分别放进 `a7`/`a6`/`a0` 再执行 `ecall`——**这里必须用 `register uint64_t a7 __asm__("a7") = eid;` 这种显式寄存器变量语法，普通的 `register uint64_t a7 = eid;` 不会真正绑定到物理寄存器**，这是本 Lab 的第二个真实 bug，见"常见坑与排查"。EID 用 `0x54494D45`（ASCII "TIME" 四个字符小端拼出来的值），FID 用 0（TIME extension 唯一的函数 `sbi_set_timer`）。
2. **trap.c**：新增 `SCAUSE_INT_SUPERVISOR_TIMER = 5`（riscv-privileged 规范里 S-mode 定时器中断的 code）。新增 `read_time()`（读非特权级只读 CSR `time`，映射自 CLINT 的 `mtime`）。新增 `timer_ticks` 全局变量、`timer_interrupt_handler()`（自增 `timer_ticks`，用*当前* `time` CSR 值加上间隔 `10000000/TIMER_HZ`（QEMU virt 平台 mtime 固定 10MHz）重新调用 `sbi_set_timer()` 预约下一次——不能用"上一次预约的值+间隔"，否则会把处理中断本身耗费的时间累积计入误差）。在 `supervisor_trap_handler()` 原有的 page fault 判断*之前*，插入对 `scause` 最高位的判断：是中断且 code 是定时器就调 `timer_interrupt_handler()` 然后直接返回，否则落到原有的 page fault 分支。新增 `timer_enable()`：`sie` 置 STIE 位（bit 5）+ `sstatus` 置 SIE 位（bit 1），两者都要用"读出来 `|=` 再写回"，不能直接整体赋值覆盖其它位。
3. **trap_entry.S**：不需要任何改动——Direct 模式下所有 trap（不管异常还是中断）共享同一个入口，分发逻辑全部在 `supervisor_trap_handler()` 内部的 C 代码里完成。
4. **linker.ld**：在 `.data`/`.bss` 输出 section 里新增对 `.sdata`/`.sbss` 输入 section 的显式匹配——这是本 Lab 的第三个真实 bug，见"常见坑与排查"，不加这两行 `timer_ticks` 会被放到内核镜像描述的物理范围之外。
5. **kernel_main.c**：在 Lab4"验证页表映射正确"之后，依次调用 `trap_init()`（跟 Lab4 一样，只是设 `stvec`）→ `timer_enable()` → 至少一次 `sbi_set_timer(read_time() + 10000000/100)`（第一次预约必须由软件主动做，硬件不会自己冒出第一个定时器中断）。主循环改成：`for(;;) { wfi; 检查 timer_ticks 是否跨过下一个整秒边界，跨过了就打印 }`。

## QEMU 运行命令

```bash
cd labs/lab05-interrupt-trap
make ARCH=x86_64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=x86_64 LAB=lab05-interrupt-trap VARIANT=solution TIMEOUT=6
```

x86_64 预期输出：

```
Hello OS from x86_64 (Lab5: interrupt & timer)
memmap: 2 available region(s) from Multiboot2 mmap tag, 32467 page(s) free
kernel image: phys [0x100000, 0x10d000)
switched to Lab4 page table, low identity map gone
page table from Lab4 verified, moving on to Lab5's interrupt framework
timer armed at 100Hz, entering main loop
tick: 1 seconds
tick: 2 seconds
tick: 3 seconds
tick: 4 seconds
tick: 5 seconds
```

```bash
make ARCH=riscv64 VARIANT=solution build
bash ../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab05-interrupt-trap VARIANT=solution TIMEOUT=6
```

riscv64 预期输出：

```
memmap: 1 region(s) from DTB /memory, 32216 page(s) free (kernel image excluded)
Hello OS from riscv64 (Lab5: interrupt & timer)
kernel image: phys [0x80200000, 0x80228000)
switched to Lab4 page table, low identity map gone
page table from Lab4 verified, moving on to Lab5's interrupt framework
timer armed at 100Hz, entering main loop
tick: 1 seconds
tick: 2 seconds
tick: 3 seconds
tick: 4 seconds
```

两边都是持续运行的内核（本 Lab 第一次没有 panic 收尾），QEMU 不会自己退出，`run-qemu.sh` 靠 `TIMEOUT` 参数（`timeout`/`gtimeout`）在固定秒数后强制终止——这是设计上的正常行为，`terminating on signal 15` 那一行不代表出错。tick 打印的具体条数取决于 `TIMEOUT` 给了多少秒（100Hz 计时，每凑够 100 个 tick 打印一次），riscv64 比 x86_64 少打一行是因为 riscv64 侧在打印"Hello OS"之前多做了一些实际耗时的初始化工作（`memmap_discover()` 在阶段 A 就执行过），实测在同样 6 秒的窗口下会比 x86_64 少一次整秒边界，`tests/expect-*.txt` 只要求这几行按顺序作为子串出现，不要求 tick 条数完全相同。

## GDB/QEMU Monitor 调试方法

```bash
bash scripts/debug-gdb.sh ARCH=riscv64 LAB=lab05-interrupt-trap VARIANT=solution
```

```gdb
target remote :1234
file solution/riscv64/build/kernel.elf
break sbi_call
commands
  print/x $a7
  print/x $a6
  print/x $a0
  continue
end
break timer_interrupt_handler
commands
  print/x $time
  continue
end
continue
```

排查"中断到底有没有真的触发/多久触发一次"最有用的做法：在中断处理函数入口打断点，`commands`/`end` 块里自动打印时间基准（x86_64 用软件自己维护的 `pit_ticks`，riscv64 用 `time` CSR），`continue` 之后对比连续两次断点命中之间的时间差，跟目标频率（100Hz，即两次之间应该差 1/100 秒对应的计数值）核对是否一致——本 Lab 开发过程中就是靠这个方法排除了"频率算错"这个假设，把真正的根因（riscv64 那个 `.sbss` 链接问题）从"计时逻辑"缩小到"内存布局"。

x86_64 一侧排查 PIC/IDT 是否配置正确，QEMU 自带的中断日志比 GDB 更直接：

```bash
qemu-system-x86_64 ... -d int -D int.log
```

日志里每一行 `v=XX` 就是一次中断/异常递交的向量号，正常情况应该看到 `v=20`（0x20=32，定时器）持续出现且不夹杂任何 `v=0d`（#GP）/`v=08`（#DF）——一旦看到这两者紧跟在 `v=20` 后面，基本可以确定是 IDT 数组越界或者 PIC 重映射本身出了问题，不需要先怀疑 PIT 的 reload value 算错。

## 自动验收测试

```bash
cd labs/lab05-interrupt-trap
bash ../../scripts/test-lab.sh ARCH=x86_64 LAB=lab05-interrupt-trap
bash ../../scripts/test-lab.sh ARCH=riscv64 LAB=lab05-interrupt-trap
```

`tests/expect-*.txt` 用逐行子串匹配（不要求整行完全一致，也不要求逐行相邻，只要求按顺序作为子串出现），所以 starter 版本实现完成后即使空闲页数、内核镜像结束地址这些和 solution 的具体数值不完全一样，或者 tick 打印的条数因为本机跑 QEMU 的速度差异而略有不同，也不影响测试通过——只要关键行的固定文本部分一致、顺序一致。

## 常见坑与排查

以下三个都是本 Lab 开发过程中**实测踩到、定位、修复过的真实 bug，不是猜测或者理论上可能发生的情况**：

- **x86_64：IDT 数组大小沿用 Lab4 的 32，导致越界写。** 本 Lab 新增的定时器向量号是 32，`idt[32]` 需要数组大小至少是 33，如果照抄 Lab4 写成 `IDT_ENTRIES = 32`，`idt[32]` 会越界写到数组之后紧跟着声明的下一个静态变量（比如 `idtp`）——这不会在编译期报错（C 数组下标不做静态越界检查），也不会立刻崩溃（越界写发生在 `idt_init()` 里，此刻还没开中断），而是延迟到*第一次*定时器中断真正被递交、CPU 去读被污染的 IDT 指针那一刻才炸：实测在 QEMU `-d int` 日志里看到 `v=20`（定时器，正常）成功进入一次，随后立刻级联 `v=0d`（#GP）→`v=08`（#DF），且 RIP 停在同一个地址不再前进。排查方法：`objdump -p kernel.elf` 或者直接 GDB `print sizeof(idt)`，核对数组实际大小是不是"最大用到的向量号+1"，不要凭 Lab4 的数字直接套用。
- **riscv64：`sbi_call()` 里用普通变量名"撞"寄存器名（`register uint64_t a6 = fid;` 而不是显式寄存器变量），导致 ecall 参数没有真正传递。** GCC/Clang 的 `register` 关键字在现代编译器里基本只是一个（通常被忽略的）优化建议，变量名恰好叫 `a6`/`a7` 不会让编译器把它分配到那个物理寄存器——实测编译出来的汇编里根本没有任何 `mv a7, ...`/`mv a6, ...`，等价于从来没有把 `eid`/`fid` 传给 `ecall`，执行时 `a6`/`a7` 里是随便什么陈旧值。这个 bug 的诡异之处在于它*不一定*立刻表现为崩溃或者报错——如果陈旧值恰好撞上一个 OpenSBI 认识的 EID/FID 组合，`ecall` 可能"看起来"正常返回，只是实际请求的行为跟代码字面表达的完全不是一回事。正确写法必须用 GCC "Specifying Registers for Local Variables" 里的显式寄存器变量语法：`register uint64_t a7 __asm__("a7") = eid;`。排查方法：`objdump -d` 反汇编检查 `ecall` 前面是否真的有把参数值搬进 `a0`/`a6`/`a7` 的指令，或者用 GDB 在 `ecall` 指令那一行打断点，`print/x $a6`/`$a7` 核对实际寄存器内容跟预期是否一致。
- **riscv64：`linker.ld` 没有匹配 `.sdata`/`.sbss` 输入 section，导致 8 字节的 `timer_ticks` 全局变量被放到内核镜像物理范围之外，随后被内存分配器当空闲页覆写。** RISC-V GCC 有一个"small data"优化：大小不超过 `-msmall-data-limit`（默认 8 字节）的全局变量会被放进 `.sdata`（有初值）或 `.sbss`（零初值）而不是 `.data`/`.bss`，为的是能用一条更省指令的 `gp` 相对寻址访问。`volatile uint64_t timer_ticks = 0;` 正好是 8 字节，被编译器放进了 `.sbss`——但 Lab4 传下来的 `linker.ld` 的 `.bss` 输出 section 只匹配 `*(.bss) *(.bss.*) *(COMMON)`，完全没有提到 `.sbss`。GNU ld 对完全没被任何规则匹配的输入 section，默认行为是按遇到顺序直接追加在*最后一个被显式安放的 section 之后*——这正好把 `timer_ticks` 的存储位置放在了 `__kernel_end`（`__bss_end`）这个地址上，后果是双重的：一是 `boot.S` 里"按 `__bss_start_phys`/`__bss_end_phys` 范围清零 BSS"的循环不会碰到它（越界，不在这个范围内），它一开始就是未初始化的垂圾值；二是更严重的，这个地址落在 `__kernel_phys_end`（链接脚本描述的内核镜像物理范围）*之外*，导致 `kernel_main.c` 建立正式页表时调用的 `kalloc_page()` 会把这个物理页当成空闲内存分配出去——页表节点的内容被写进这个地址，`timer_ticks` 的存储被覆盖。表现症状是"QEMU 输出在几秒内打出几十甚至上百个 tick"（因为读到的是页表构造过程写进去的、恰好很大的一串数值，一次性跨过了几十个整秒边界），第一眼很容易误判成"定时器触发频率算错了"，但用 GDB 在定时器中断处理函数里断点核对连续两次命中之间 `time` CSR 的差值，能确认真实的触发频率其实是对的（约等于 100Hz），问题出在 `timer_ticks` 读到的值本身不对。确诊方法：`riscv64-elf-nm kernel.elf | grep timer_ticks` 或者 `riscv64-elf-objdump -t kernel.elf`，核对这个符号实际的 section 类型是不是 `.sbss`（而不是期望的 `.bss`），以及它的地址是否恰好等于 `__kernel_end`。修复方式：在 `.data`/`.bss` 输出 section 里分别补上 `*(.sdata) *(.sdata.*)` 和 `*(.sbss) *(.sbss.*)` 的显式匹配。**同样的匹配遗漏在 Lab1-4 的 riscv64 `linker.ld` 里也存在**（Lab0 还没有 `linker.ld`），而且这不是推测——已经逐个实测确认过，结论分两半，都值得知道：

一是**按现在发布的代码，Lab1-4 不会踩到这个坑**：`readelf -S` 核对四个 Lab 的 `kernel.elf`，它们根本没有产生 `.sbss`/`.sdata` 这两个 section，因为那几个 Lab 里恰好没有一个"不超过 8 字节、又是可变状态"的全局变量。也就是说这是一个**潜伏**的缺陷，不是一个现存的 bug；Lab3/Lab4 里确实有一个没被任何规则匹配的 `.srodata` 孤儿 section，但它是只读数据，跟着 `.text`/`.rodata` 一起落进同一个 LOAD 段，位置正确、不受影响。

二是**这个潜伏的坑可以被精确触发，严重程度还随 Lab 递增**。验证办法是临时往内核里加一个 4 字节全局变量再看它落在哪（实测完已恢复）：在 Lab3 里 `.sbss` 落到 `0x80202010`，正好是 `__bss_end`，已经在 `boot.S` 的清零范围之外——变量拿到的是垃圾初值，但还没有别人来覆写它。到了 Lab4，同一个探针落到 `0xffffffc080228000`，这个地址同时等于 `__bss_end` 和 `__kernel_end`，并且**高于 `__stack_top`（`0xffffffc080227010`）**——而 `memmap.c` 的 `add_region_excluding_kernel_image()` 正是用 `__stack_top` 当作内核镜像的末尾来划分空闲内存的，于是这个地址落进了 `kalloc` 会拿去分配的池子里。两个后果就此凑齐：既不被清零，又会被分配器覆写——和本 Lab 上面描述的 `timer_ticks` 症状是同一个机制，一字不差。

换句话说，本 Lab 之所以是第一个真正炸出来的，不是因为前面的链接脚本更正确，而是因为本 Lab 第一次引入了一个**足够小、可变、而且内核真的会去读**的全局变量。这类"缺陷早就在了，只是还没有代码去踩"的情况，在系统编程里是常态，也是为什么"能跑通"不等于"是对的"——同一个道理的另一个实例见 [`docs/verification-methodology.md`](../../docs/verification-methodology.md)。

## 挑战任务

- 把 x86_64 版本的 IDT 也扩展到覆盖键盘中断（IRQ1，重映射后是向量 33），实现一个最简单的扫描码回显——顺带体会一下"一个中断源"和"两个中断源"之间，PIC 的 EOI/mask 处理需不需要额外考虑"哪个 IRQ 触发的"这个问题（提示：`pic_send_eoi()` 目前是无条件发给 PIC1 的，如果两个 IRQ 都可能触发，这个函数需不需要参数化）。
- riscv64 版本尝试把 Direct 模式换成 Vectored 模式（`stvec` 的 MODE 位设成 1），需要为每一种 `scause` code 单独写一段汇编入口，体会一下这种模式在中断源变多之后，相对 Direct 模式"软件里用 if 链分发"的优劣（提示：思考 Vectored 模式怎么处理"最高位是中断/异常"这个区分，是不是也得复用同一段跳转表）。
- 给两边的 tick 计数增加"漂移检测"：定期对比软件维护的 tick 计数换算出的秒数和一个独立时间源（x86_64 没有现成的独立时钟源，riscv64 可以直接读 `time` CSR）之间的差值，量化本 Lab 目前的计时方式实际累积误差有多大，思考在教学场景之外，真实内核会怎么处理这种误差（提示：NTP、tickless kernel）。
- 阅读 QEMU 源码里 `hw/intc/i8259.c`（x86_64 PIC 的具体实现）和 `hw/intc/riscv_aclint.c`（riscv64 CLINT 的具体实现），对照本 Lab 写的驱动代码，找出哪些行为是"规范要求"、哪些是"QEMU 这个具体实现选择这样做，规范留了余地"。

## 参考

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Chapter 10 "Advanced Programmable Interrupt Controller (APIC)"（本课程用的是更古老的 legacy PIC，但 SDM 同一章也覆盖了 8259A 的行为约定）
- Intel 8259A datasheet，"Initialization Command Words (ICW1-4)" 一节（PIC 重映射的权威定义）
- Intel 8254/8253 datasheet，"Mode/Command Register" 一节（PIT 各个 mode 的行为定义）
- OSDev Wiki，"8259 PIC" 页面（社区整理的标准重映射序列，本课程从零推导后确认与其一致）
- RISC-V SBI (Supervisor Binary Interface) Specification，"Binary Encoding" 一节（`ecall` 参数传递约定）、"TIME Extension" 一节
- The RISC-V Instruction Set Manual, Volume II: Privileged Architecture，"Supervisor Interrupt Registers (sip and sie)"、"sstatus"、"Machine Cause Register (mcause)" 几节
- GCC 文档，"Specifying Registers for Local Variables"（显式寄存器变量语法，本 Lab 第二个真实 bug 的修复依据）
- GCC RISC-V 选项文档，`-msmall-data-limit`（`.sdata`/`.sbss` 优化的触发条件，本 Lab 第三个真实 bug 的根本机制）

## 下一步

进入 [Lab6：系统调用与用户态](../lab06-syscall-user/README.md)——本 Lab 之前，CPU 从复位那一刻起就一直在内核自己手里，所有代码都跑在最高特权级；Lab6 第一次把控制权交给一段权限更低的代码，并且保证它想拿回高特权级只能走内核指定的唯一入口。本 Lab 搭好的 trap 入口会在那里第一次承担一个新角色：x86_64 的 SYSCALL 反而*绕过* IDT 走 MSR 配置的独立入口，riscv64 的 `ecall` 则和本 Lab 的定时器中断共享同一个 `stvec`——于是"trap 一定来自 S 模式、sp 已经是合法内核栈"这个本 Lab 成立的前提会被打破。

再往后是 [Lab7：进程与调度](../lab07-process-scheduler/README.md)，那里才会把本 Lab 的定时器从"演示中断能工作"变成调度器真正依赖的核心机制：周期性打印一行字换成周期性触发一次调度决策。
