# Lab8：简单文件系统与块设备

## 学习目标

- 建立"块设备"这一层抽象：一个只有 `blk_init()` 和 `blk_read(blkno, buf)` 两个函数的接口，背后是两套毫无共同点的驱动（x86_64 用 ATA PIO 端口 I/O，riscv64 用 virtio-blk 的共享内存描述符环 + DMA）。两边都写完之后你会拿到一个可以机械验证的结论：`fs.c` 在两个架构下**逐字节相同**，而它下面的驱动一行都不一样——这就是一层抽象真正立住了的样子，不是"看起来差不多"。
- 亲手实现 xv6 风格的只读 inode 文件系统：超级块、空闲位图、inode 表、目录项四种磁盘结构，以及"inode 号 + 字节偏移 → 磁盘块号"这个映射的具体算法。理解为什么这些结构的大小必须整除块大小（本 Lab 的 `struct dinode` 被显式填充到 64 字节、`struct dirent` 到 32 字节），以及为什么 inode 0 必须永久保留为"无效"。
- 通过实现 `open`/`read`/`close` 三个系统调用，建立**文件描述符**这个抽象：用户程序拿到的只是一个小整数，"这个整数对应哪个 inode、已经读到第几个字节"这些状态全部由内核在 PCB 里维护。理解为什么不能只提供一个 `read_file(名字, 偏移, 缓冲区, 长度)` 就完事——把"当前偏移"搬进内核，是 Lab9 的 shell 重定向和 Lab10 的管道能够存在的前提。
- 理解"写"到底难在哪里。本 Lab 刻意只做只读：难的不是多写几个函数，而是**崩溃一致性**——元数据和数据的落盘顺序错了，掉电之后文件系统就是坏的，而且坏法是静默的。"核心概念"一节会把这个取舍完整讨论一遍（为什么需要日志、三种主流方案各自的代价），但本 Lab 不要求实现完整 journaling。
- 再一次确认"当前没触发不等于不存在"这类反直觉的事实。本 Lab x86_64 侧第一个真实崩溃，是一个**只读、纯轮询**的 ATA 驱动因为没有屏蔽设备中断而被打成三重故障——设备读完一个扇区"好心通知了一声"，内核的 IDT 里没有那一项。很容易以为"我不用中断，中断就跟我无关"，但中断是设备主动发起的。

## 前置 Lab

依赖 [Lab7：进程与调度](../lab07-process-scheduler/README.md)——本 Lab 复用 Lab7 建好的全部多进程基础设施（进程表、调度器、`swtch()`/`trap_return`、fork/exec/wait、per-进程内核栈与 TSS/`trap_kernel_sp_top`），不重新讲解这些机制本身。

Lab7 的基础设施有两处会以"约束"的形式影响本 Lab 的设计，值得提前知道：

1. **时钟中断处理程序会 `yield()`。** 这意味着 `fs_read()` 在任意一次 `blk_read()` 之后都可能被打断、切到另一个进程、再切回来。本 Lab 因此有一条硬规则：**`fs.c` 里所有块缓冲区一律在栈上，不许用 `static`**（见 `fs.c` 里 `read_inode()` 上方的完整推导）。
2. **每个进程的内核栈只有一页（4 KiB）。** 栈上的 512 字节块缓冲区不是免费的，本 Lab 刻意保证同一时刻最多两层块缓冲区同时活着（`fs_read` 一层 + `read_inode` 一层），峰值约 1 KiB。

`boot.S`、`linker.ld`、`console_putc.c`、`panic_arch.c`、`memmap.c`、`swtch.S`、`user_blob.S`、`user_prog.ld`、x86_64 的 `grub.cfg`/`pit.c`、riscv64 的 `sbi.c`/`sbi.h` 跟 Lab7 完全一样，本 README 不重复讲解。

## 核心概念

**块设备抽象只需要两个函数，而"只需要两个"本身就是这一层的全部价值。** `blk.h` 的全部内容是 `void blk_init(void)` 和 `void blk_read(uint32_t blkno, void *buf)`——没有错误返回码、没有异步完成回调、没有"一次读多块"。上面的 `fs.c` 因此完全不知道自己跑在什么机器上：它只会说"把第 7 块给我"，至于这句话是变成 8 次 `outb` 加 256 次 `insw`，还是变成三个描述符加一次 MMIO 写，跟它无关。**两个 `blk.h` 文件在两个架构目录下逐字相同，`make check-fs-iface` 用 `cmp` 守着**——改了一边忘了另一边会直接构建失败，抽象边界不靠"记得对齐"维持，靠机械检查维持。

**`blk_read()` 失败时 panic 而不是返回错误码，这是一个经过论证的选择，不是偷懒。** 在 QEMU 里，一次块读失败只可能是三种情况：镜像没挂上（`blk_init()` 阶段就该发现）、块号超出镜像范围（调用者的 bug）、驱动写错了（驱动的 bug）。三种都是**程序错误**，不是运行时可恢复的状况。给 `blk_read()` 加返回码的代价是上面每一个调用点都要写一遍 `if (err) ...`，而那些分支在本 Lab 的环境里永远不会被走到、也永远不会被测试到——未被测试的错误处理代码比没有错误处理更危险。真机上（坏道、线缆松动、设备超时）结论会反过来，那时错误码是必须的。

**"轮询而不是中断"的真正原因是 Lab7 的调度器还没有 SLEEPING 状态。** 中断驱动的块设备驱动长这样：发出请求 → 当前进程睡眠 → 设备完成、中断处理程序唤醒它。本课程的进程状态只有 `UNUSED`/`RUNNABLE`/`RUNNING`/`ZOMBIE`，没有"睡在某个等待队列上"这一档，也没有 sleep/wakeup 原语——补上它们最自然的位置是 Lab10 讨论并发的时候。在没有这套机制的前提下，"中断驱动"只能退化成"在中断处理程序里设一个标志、然后在原地忙等这个标志"，比直接轮询状态寄存器更绕，还多一个中断处理程序要维护。所以本 Lab 两个驱动都轮询，而且都有**明确的轮询上限**（`ATA_POLL_LIMIT`/`VIRTIO_POLL_LIMIT`，各一百万次）：没有上限的 `while (status & BSY)` 在"根本没有盘"的情况下就是一个死循环，内核表现成"启动到某一行然后永远卡住，什么都不打印"；有上限就能变成一句说清原因的 panic。**所有等硬件的循环都要有退出路径**，这是内核/嵌入式代码的通用纪律。

**磁盘上的结构大小必须整除块大小，这不是洁癖，是为了让"第 N 个 inode 在第几块的第几个字节"是一次除法而不是一次跨块拼接。** `struct dinode` 的自然大小是 40 字节（type/nlink/size/addrs[8]），本 Lab 用一个显式的 `uint8_t pad[24]` 把它撑到 64——`512 / 64 = 8`，每块正好 8 个 inode，任何一个 inode 都不会横跨两块。如果放任它是 40 字节，`512 / 40 = 12.8`，第 13 个 inode 的前 32 字节在一块、后 8 字节在下一块，`read_inode()` 就得先读两块再手工拼接。同理 `struct dirent` 被定到 32 字节（`DIRSIZ 30` + 2 字节 inum），每块 16 个目录项。`fs_format.h` 里六个 `_Static_assert` 把这些整除关系钉在编译期，改动任何一个常量会立刻在编译时撞上，而不是运行时悄悄算错。

**inum 0 永久保留为"无效"，于是 `fs_lookup()` 返回 0 就天然表示"找不到"，不需要额外的 out 参数或者哨兵值。** 根目录是 inum 1（`ROOTINO`），inode 表的第 0 号槽位在镜像里真实存在但永远不被分配。这个约定的收益在 `fs.h` 的接口形状上直接可见：`uint32_t fs_lookup(const char *name)` 一个返回值就够了，调用者写 `if (inum == 0)`。代价是每 512 字节的 inode 表浪费 64 字节。真实 Unix 文件系统做的是同一个选择（ext2 的 inode 也从 1 开始）。

**目录只是一个"内容恰好是 `struct dirent` 数组"的普通文件。** 它的 inode 有 `type == T_DIR`，除此之外和普通文件没有任何区别——同一张 `addrs[]` 表、同一套"偏移转块号"的算法，`fs_lookup()` 读根目录用的就是 `fs_read()` 本身，不是另一条特殊路径。这是 Unix 文件系统设计里最优雅的一处复用：**"目录"是一个类型标记 + 一份内容约定，不是一种新的存储结构**。本 Lab 的简化是目录只有一层（根目录），不解析路径——`fs_lookup("motd.txt")` 而不是 `fs_lookup("/usr/share/motd")`。加上路径解析并不需要新的磁盘结构，只需要在 `fs_lookup()` 外面套一个"按 `/` 切分、逐段查找、用上一段的结果当下一段的起点"的循环，这是挑战任务之一。

**目录项里的名字在填满时不带结尾的 `\0`，所以不能用 `strcmp()`。** `DIRSIZ` 是 30，一个正好 30 字节的名字会把 `name[]` 填满，后面紧跟的就是下一个 `dirent` 的 `inum` 字段。`strcmp()` 会一路读过去，把下一项的字节当成名字的一部分。`fs.c` 因此自己写了一个 `name_eq(disk_name, want)`：逐字节比较、最多比 `DIRSIZ` 个、并且要求"要么两边都到头，要么磁盘侧的这一位是 `\0`"。这是"磁盘格式和 C 字符串约定不是同一件事"的一个具体例子——磁盘格式为了省空间不留结尾符，是完全正常的设计。

**文件描述符的全部实现就是"把偏移存在内核里"这一件事。** `struct file { int used; uint32_t inum; uint32_t off; }`，fd 就是 `p->ofile[]` 的下标，没有任何间接层。`sys_read()` 做的事是：查 fd → 调 `fs_read(f->inum, f->off, buf, len)` → `f->off += got`。最后那一行就是"文件描述符携带状态"的完整实现。**关键是 `off` 存在 `struct file` 里而不是 inode 里**：同一个文件被 `open()` 两次会得到两个槽位、两个独立的 `off`，两个 fd 可以在同一个文件的不同位置各读各的。把偏移放进 inode（每个文件只有一个）就做不到这件事——这正是 Unix 把"文件"和"打开的文件"分成两个概念的原因。

**本 Lab 的 fd 表直接嵌在 `struct proc` 里，于是 `fork()` 之后父子进程的偏移不共享——这和真实 Unix 语义不同，而且差别是能观察到的。** 真实的 `fork()` 让父子共享同一个 `struct file`（只增加引用计数），父进程读一段、子进程接着读下一段。本 Lab 的 `fork()` 用 `memcpy` 整份拷贝 `ofile[]`，父子各有一份独立偏移，两边都从同一个位置开始读。做到真实语义需要一张全局 file 表 + 引用计数 + 在 `exit()`/`close()` 里递减，是挑战任务之一。

**只读是本 Lab 最大的简化，而它省掉的不是"几个函数"，是崩溃一致性这一整个问题。** 想一下"创建一个 1 KiB 的新文件"在磁盘上要改几处：位图上标记 2 个数据块已用、inode 表里写一个新 inode（type/size/addrs）、根目录里追加一个 dirent、把 1 KiB 内容写进那 2 个数据块。这是**至少四次独立的块写入**，而磁盘一次只原子地写一块。任意两步之间掉电，文件系统就处于一个不该存在的中间状态：

- 先写位图、还没写 inode 就掉电 → 两个块被标记为已用但没人引用（**空间泄漏**，不致命，`fsck` 能回收）。
- 先写 dirent、还没写 inode 就掉电 → 目录里有一个名字指向一个全 0 的 inode 槽位（**悬空目录项**，`type == 0` 的 inode 被当成有效文件打开）。
- 先写 inode（`addrs` 指向新块）、还没写位图就掉电 → 那两个块在位图里是"空闲"的，下一次分配会把它们再分给另一个文件，**两个文件共享同一个数据块**，写一个会破坏另一个。这是三种里最坏的，因为它在掉电之后还会继续制造新的损坏。

所以真实文件系统必须让"多次块写入"整体上表现为原子的，主流做法有三类：**(1) 日志/journaling**（ext3/ext4、xv6 的做法：先把"我准备做哪几个块写"完整写进一段日志区并打上提交标记，再真正写目标位置；崩溃后重放日志里已提交的事务、丢弃未提交的。代价是每个写操作至少写两遍，ext3 的 `data=ordered` 就是"只给元数据记日志、数据直写"的折中）；**(2) 写时复制/soft updates**（ZFS、btrfs：永远不原地改写，新数据写到新位置，最后用一次原子的根指针更新切过去。代价是空间放大和碎片）；**(3) 顺序约束**（FFS 的 soft updates：不记日志，但严格排定写入顺序，保证任何时刻掉电后的状态都只可能是"空间泄漏"这种良性错误，靠 `fsck` 收尾。代价是排序逻辑极其复杂，且 `fsck` 要扫全盘）。

本 Lab 只读，上面这些一个都不需要——`fs_init()` 做的位图自检（把所有 inode 引用的块收集成一个 `uint64_t` 集合，跟磁盘上的位图逐位对比）是一个**微型 `fsck`**，它在只读文件系统里永远应该通过，写进来之后就会变成真正有用的诊断工具。ROADMAP 对本 Lab 的要求是"讨论但不强制实现完整 journaling"，这一段就是那个讨论；把只读改成可写、并给写路径加一段日志，是本 Lab 最大的挑战任务。

**镜像不是内核造的，是宿主机上一个独立的程序造的。** `mkfs/mkfs.c` 用**宿主编译器**（`HOSTCC`，不是交叉编译器）编译成一个本机可执行文件，把 `fsroot/` 下的文件打包成 `fs.img`。这是本课程第一次出现"宿主工具链产物参与构建"——真实系统里这个角色就是 `mkfs.ext4`/`newfs`。关键的设计约束是 `mkfs.c` 和两个内核 `fs.c` **共享同一份 `fs_format.h`**（Makefile 的 `-I$(LAB_ROOT)`），靠 `__STDC_HOSTED__` 宏选择 `<stdint.h>` 还是内核自己的 `"types.h"`。磁盘格式只有一个定义，格式化工具和读取代码不可能对不上——这是消除"两处定义漂移"这类 bug 的唯一可靠办法。

## x86_64 与 riscv64 对照表

本 Lab 是整个课程里两个架构分歧最大的一次——不是"同一件事的两种写法"，而是两套完全不同的设备模型。

| 维度 | x86_64（ATA PIO，`ide.c` 297 行） | riscv64（virtio-blk，`virtio.c` 528 行） |
| --- | --- | --- |
| 访问设备的机制 | 端口 I/O：`inb`/`outb`/`insw` 指令，独立的 I/O 地址空间 | MMIO：普通的内存读写，但指针必须是 `volatile` 且必须是 32 位宽 |
| 设备地址从哪来 | 硬编码 `0x1F0`~`0x1F7` + `0x3F6`，PC/AT 遗留约定，不探测 | 扫描 `0x10001000` 起的 8 个槽位，比对 MagicValue/DeviceID/Version |
| 需要建页表映射吗 | 不需要，端口 I/O 不经过 MMU | 需要，`kernel_main` 把 8 个页 `0x10001000..0x10008fff` 恒等映射 |
| 谁搬数据 | CPU 亲自搬：每 512 字节 = 256 次 `insw`，一个字一个字进寄存器再进内存 | 设备 DMA：驱动在内存里摆好描述符、敲一下寄存器，数据由设备直接写内存，CPU 全程不碰数据 |
| 要不要虚实地址转换 | 不要——CPU 自己搬，用的就是 CPU 的地址视角 | **要**——`virt_to_phys()`，因为设备 DMA 不经过 MMU。这是本课程第一次"CPU 的地址视角"和"设备的地址视角"分岔 |
| 内存屏障 | 不需要（端口 I/O 本身就是强序的） | 需要：发布 `avail.idx` 之前和读 `used.idx` 之后各一条 `fence rw, rw` |
| 初始化流程 | 简单：选盘、等 DRDY、关中断 | 繁琐但有规范：状态位按 ACKNOWLEDGE→DRIVER→FEATURES_OK→DRIVER_OK 累积写入，其中 FEATURES_OK 必须回读确认 |
| 一次读 512 字节的交互次数 | 约 265 次总线交互（8 次写命令寄存器 + 轮询 + 256 次 `insw`） | 4 次：3 个描述符写内存（不算总线）+ 1 次写 QueueNotify + 轮询 `used.idx` |
| 必须处理中断吗 | **必须**——即使纯轮询也要写 `0x3F6` 的 nIEN 并屏蔽从片 PIC 的 IRQ 14，否则三重故障（见"常见坑"第 1 条） | 不必——本 Lab 全程没碰 virtio 的中断配置也不会出问题 |
| QEMU 挂盘参数 | `-drive file=fs.img,format=raw,index=0,if=ide` | `-drive file=fs.img,format=raw,if=none,id=d0` + `-global virtio-mmio.force-legacy=false` + `-device virtio-blk-device,drive=d0,bus=virtio-mmio-bus.0` |
| 现代替代路径 | PCI 枚举 → AHCI/NVMe（ATA PIO 是 1990 年代的接口） | 本身就是现代接口；真实系统用设备树而不是扫槽位来发现地址 |

**抽象之上的部分，两个架构逐字节相同**，由 `make check-fs-iface` 用 `cmp` 守着：

| 文件 | 两架构是否逐字相同 | 说明 |
| --- | --- | --- |
| `blk.h`（57 行） | 相同 | 块设备接口，两个函数 |
| `fs.h`（45 行） | 相同 | 文件系统接口，四个函数 |
| `fs.c`（469 行） | 相同 | 整个文件系统实现 |
| `sys_open`/`sys_read`/`sys_close` | 相同 | 在两边 `trap.c` 里的实现逐字相同 |
| `blk_init`/`blk_read` 的实现 | **完全不同** | `ide.c` vs `virtio.c`，没有一行共用 |

顺便留意 `sys_open`/`sys_read`/`sys_close` 跟 Lab7 的 `sys_fork`/`sys_exec` 的对比：后者两边**签名**就不一样（要拿架构相关的陷入帧），前者两边逐字相同。**这个差异本身就说明了哪些内核功能真的和架构相关**——文件系统调用完全不相关，进程创建完全相关。

`fs.c` 没有被提到 `src/common/` 下共享，是因为 `starter/` 需要两份可以各自独立填空的骨架：先在一个架构上把文件系统写通，再在另一个架构上重写一遍（或者直接抄过去、用 `cmp` 确认），比一开始就给一份共享代码更有教学价值。

## 代码目录与关键文件

```
labs/lab08-filesystem/
├── fs_format.h              # 【本 Lab 新增】磁盘格式的唯一定义，内核和 mkfs 共享
├── fsroot/                  # 【本 Lab 新增】要打进镜像的文件
│   ├── hello.txt            #   98 字节，最小用例
│   ├── motd.txt             #   1824 字节，跨多个数据块，1824 % 128 == 32（见下）
│   └── exact.txt            #   512 字节，正好一块，用来测边界
├── mkfs/
│   └── mkfs.c               # 【本 Lab 新增】宿主机工具，把 fsroot/ 打包成 fs.img
├── Makefile                 # 在 Lab7 基础上新增：fs.c 进 ARCH_SRCS、mkfs 用 HOSTCC 编译、
│                            #   fs.img 生成规则、check-fs-iface 一致性检查
├── tests/
│   ├── expect-x86_64.txt    # 20 行
│   └── expect-riscv64.txt   # 21 行
├── solution/x86_64/
│   ├── blk.h                # 【新增】块设备接口（和 riscv64 逐字相同）
│   ├── ide.c                # 【新增】ATA PIO 驱动，297 行
│   ├── fs.h                 # 【新增】文件系统接口（和 riscv64 逐字相同）
│   ├── fs.c                 # 【新增】文件系统实现，469 行（和 riscv64 逐字相同）
│   ├── kernel_main.c        # 在 Lab7 基础上新增：blk_init(); fs_init(); + 内核侧自检
│   ├── proc.h               # 在 Lab7 基础上新增：NOFILE、struct file、ofile[NOFILE]
│   ├── proc.c               # 在 Lab7 基础上新增：proc_alloc 清零 ofile、fork 拷贝 ofile
│   ├── syscall.h            # 在 Lab7 基础上新增：SYS_OPEN 6 / SYS_READ 7 / SYS_CLOSE 8
│   ├── trap.c               # 在 Lab7 基础上新增：fd_lookup + 三个系统调用 + 分发 + 第三个参数
│   ├── user_prog.S          # 【重写】改成一串文件读取测试
│   ├── pit.c                # 和 Lab7 完全一样
│   ├── boot.S linker.ld grub.cfg console_putc.c panic_arch.c memmap.c
│   ├── swtch.S user_blob.S user_prog.ld pagetable.c pagetable.h
│   ├── kalloc.c kalloc.h kprintf.c kprintf.h string.c types.h ...   # 和 Lab7 完全一样
├── solution/riscv64/
│   ├── blk.h fs.h fs.c      # 【新增】和 x86_64 逐字相同
│   ├── virtio.c             # 【新增】virtio-mmio 块设备驱动，528 行
│   ├── kernel_main.c        # 在 Lab7 基础上新增：恒等映射 8 页 MMIO + blk_init(); fs_init(); + 自检
│   ├── proc.h proc.c syscall.h trap.c user_prog.S  # 同 x86_64 的新增项
│   ├── sbi.c sbi.h          # 和 Lab7 完全一样
│   └── ...                  # 其余和 Lab7 完全一样
└── starter/{x86_64,riscv64}/   # 每个架构 7 个带 TODO 的文件，其余从 solution 直接给
```

几个值得单独说明的地方：

- **`fs_format.h` 在 Lab 根目录，不在任何架构目录下。** 它被三个编译目标包含：两个内核（交叉编译器）和 `mkfs`（宿主编译器）。`__STDC_HOSTED__` 宏在宿主编译时为 1、在内核的 `-ffreestanding` 编译下为 0，用它选 `<stdint.h>` 还是内核的 `"types.h"`。
- **`motd.txt` 是 1824 字节，而 `1824 % 128 == 32`。** 用户程序用 128 字节的块循环读它，最后一次只会拿到 32 字节。这个数字是刻意挑的：它让"`read()` 返回值小于请求长度**不等于**文件结束"这件事在测试里真实发生一次，而不是停留在文档里的一句提醒。POSIX 里"读到 0 字节"才是 EOF。
- **`exact.txt` 正好 512 字节**，即正好一个块、`addrs[0]` 用满而 `addrs[1]` 为 0。这是"偏移落在块边界上"的边界用例。
- **`FSROOT_FILES := $(sort $(wildcard $(LAB_ROOT)/fsroot/*))` 里的 `$(sort)` 是必需的，不是习惯。** `mkfs` 按命令行顺序分配 inode 号，而 expect 文件里硬编码了 `motd.txt` 是 inode 4。`$(wildcard)` 的返回顺序依赖文件系统，不排序的话在不同机器上 inode 号会变，测试会莫名其妙地失败。

## 分步实现步骤

两个架构的第 1 步（磁盘格式）、第 2 步（mkfs）和第 5 步之后（文件系统 + 系统调用）内容完全一致，只有块设备驱动那一段不同。建议的顺序是：**先把 mkfs 和镜像做出来（可以在宿主机上用 `xxd` 直接验证），再写驱动，最后写文件系统**——这样每一层都能在下一层写之前单独确认。

### x86_64 路线

1. **写 `fs_format.h`。** 定义 `BSIZE 512`、`FS_MAGIC 0x3842414Cu`，布局常量 `FS_BITMAPSTART 1`/`FS_INODESTART 2`/`FS_DATASTART 4`，规模常量 `FS_NBLOCKS 64`（32 KiB 镜像）/`FS_NINODES 16`，类型 `T_DIR 1`/`T_FILE 2`（0 表示未使用），`NDIRECT 8`（单文件上限 4096 字节），`DIRSIZ 30`，`ROOTINO 1`。定义 `struct superblock`、`struct dinode`（用 `uint8_t pad[24]` 填到 64 字节）、`struct dirent`（32 字节）。写六个 `_Static_assert` 把这些关系钉在编译期：`sizeof(struct dinode) == 64`、`sizeof(struct dirent) == 32`、`BSIZE % sizeof(struct dinode) == 0`、`BSIZE % sizeof(struct dirent) == 0`、`sizeof(struct superblock) <= BSIZE`（超级块装得进一个块），以及 `FS_INODESTART + FS_NINODES / (BSIZE / 64) <= FS_DATASTART`（inode 表放得进 `[FS_INODESTART, FS_DATASTART)` 这个区间）。
2. **写 `mkfs/mkfs.c`。** 在内存里开一个 `FS_NBLOCKS * BSIZE` 的数组，按顺序：写超级块 → 分配 inode 1 作为根目录（`type = T_DIR`）→ 对每个输入文件分配一个 inode、把内容按块追加（`inode_append`）、在根目录里加一条 dirent（`dir_add`）→ 每分配一个数据块就在位图里置位（`bitmap_mark`）→ 整个数组写进输出文件。用 `HOSTCC` 编译（**不是 `$(CC)`**，交叉编译器产出的二进制在宿主机上跑会报 "cannot execute binary file"）。写完先用 `xxd -l 64 build/fs.img` 看超级块的魔数对不对，这一步能独立验证，不要跳过。
3. **在 Makefile 里接上镜像生成。** `FS_IMG := $(BUILD_DIR)/fs.img`、`FSROOT_FILES := $(sort $(wildcard $(LAB_ROOT)/fsroot/*))`、`$(MKFS_BIN) $@ $(FSROOT_FILES)`，并把 `build` 目标改成 `build: $(KERNEL_ELF) $(FS_IMG)`。给内核和 mkfs 都加 `-I$(LAB_ROOT)` 以便共享 `fs_format.h`。
4. **写 `blk.h`。** 只有 `blk_init()` 和 `blk_read(uint32_t blkno, void *buf)` 两个声明，加上"失败即 panic""轮询而非中断"的原因注释。写完之后**把这个文件原样复制到 riscv64 目录**——它是两边共享的契约。
5. **写 `ide.c` 的端口定义和等待函数。** 定义 `0x1F0`(DATA)/`0x1F1`(ERROR)/`0x1F2`(SECCNT)/`0x1F3`(LBA_LO)/`0x1F4`(LBA_MID)/`0x1F5`(LBA_HI)/`0x1F6`(DRIVE)/`0x1F7`(CMD) 和 `0x3F6`。状态位 `BSY 0x80`/`DRDY 0x40`/`DRQ 0x08`/`ERR 0x01`。写 `ata_400ns_delay()`（连读 4 次 `0x3F6` 丢弃结果）、`ata_wait_ready_for_command()`（等 BSY 清、DRDY 置）、`ata_wait_data_ready()`（等 BSY 清、DRQ 置，顺带检查 ERR）。两个等待函数都要有 `ATA_POLL_LIMIT` 上限，超限就 `kprintf` 打出状态寄存器的值再 panic。
   - **注意 `0x3F6` 按读写方向是两个不同的寄存器**：读是 Alternate Status（和 `0x1F7` 位定义相同，但**不会**清除挂起的中断），写是 Device Control（含 nIEN 位）。轮询要读 `0x3F6` 而不是 `0x1F7`——读 `0x1F7` 会顺手确认中断，将来改成中断驱动时会莫名丢中断。
6. **写 `blk_init()`，并且一定要关掉设备中断。** 选主盘（往 `0x1F6` 写 `0xE0`）、等 DRDY、**往 `0x3F6` 写 nIEN 位（`0x02`）禁止设备发中断**、**再屏蔽从片 PIC 上 IRQ 14 对应的那一位**。这两件事少做一件就会三重故障，见"常见坑"第 1 条——这是本 Lab x86_64 侧最容易踩、也最难自己定位的坑，建议直接照着做，然后去读那一条的完整推导。
7. **写 `blk_read()`。** 一次一个扇区：等设备就绪 → 往 `0x1F2` 写扇区数 1 → 把 LBA 的低 24 位分别写进 `0x1F3`/`0x1F4`/`0x1F5`、高 4 位并进 `0x1F6` 的低 4 位 → 往 `0x1F7` 写 `ATA_CMD_READ_SECTORS 0x20` → 400 ns 延迟 → 等 DRQ → 用 `insw` 读 256 个 16 位字进缓冲区。
8. **在 `scripts/run-qemu.sh` 里接上盘。** 自动探测 `$BUILD_DIR/fs.img` 存在就加 `-drive file=$DISK,format=raw,index=0,if=ide`，并留一个 `DISK=NONE` 的逃生口用于测试"没有盘"的路径。**必须用 `index=0`**：`-cdrom` 占的是 IDE 从盘 `index=2`，写 `index=2` 会让 QEMU 直接报 "drive index 2 used twice" 拒绝启动。
9. **写 `fs.c` 的读取路径。** `fs_init()`：读第 0 块、验魔数（不对就把读到的魔数和期望值都打出来再 panic）、存进 `static struct superblock sb`、置 `fs_mounted`、跑位图自检。`read_inode(inum, out)`：块号 `FS_INODESTART + inum / (BSIZE / sizeof(struct dinode))`，块内下标 `inum % (...)`。`fs_size(inum)`。`fs_read(inum, off, dst, n)`：按 `off / BSIZE` 定位到 `addrs[]`、`off % BSIZE` 定位块内偏移、逐块拷贝，读到文件尾就**短读**返回实际字节数。`fs_lookup(name)`：用 `fs_read()` 遍历根目录的 dirent，用自己写的 `name_eq()` 比较（**不是 `strcmp`**）。
   - **所有块缓冲区都声明在栈上，一个 `static` 都不许有。** 时钟中断会在 `fs_read()` 中途切走进程，`static` 缓冲区会被另一个进程覆盖，症状是"偶发地读到别的文件的内容"，而且和时钟频率相关，几乎无法复现。
10. **写位图自检。** `collect_referenced_blocks()` 扫所有 inode、把它们引用的块号收集进一个 `uint64_t` 位集合（这也是 `_Static_assert(FS_NBLOCKS <= 64)` 的原因），加上 4 个元数据块，跟磁盘位图逐位比对。这是一个微型 `fsck`，在只读文件系统里应当永远通过。
11. **在 `kernel_main.c` 里调用 `blk_init(); fs_init();`。** 位置有三个约束：在 `proc_init()`/`scheduler()` **之前**（挂载自检要轮询读十几个块，放在有进程之后会让启动顺序难以推理）；在 `kalloc_set_phys_to_virt_offset()` **之后**（panic 路径要能打印）；`blk_init()` 在 `fs_init()` 之前（这样"没有盘"会被报成"总线上没有硬盘"，而不是"超级块魔数不对"——后者会让人去查镜像格式，方向全错）。
12. **加内核侧自检。** 挂载成功后直接用 `fs_lookup` + `fs_read` 读一次 `motd.txt` 的开头并打印，**刻意绕过 open/read 系统调用**。这样一条长链路被拆成两段：如果这行打印出来了但用户程序读不到，bug 在系统调用路径；如果这行就失败，再怎么调用户态代码也没用。截断打印的位置要切在已知是 ASCII 的 `'\n'` 上，不要按固定字节数切——固定字节数会把一个 3 字节的中文字符切一半，终端显示成替换字符，让人以为读出来的数据是错的（见"常见坑"第 8 条）。
13. **加 fd 表。** `proc.h` 里加 `#define NOFILE 8`、`struct file { int used; uint32_t inum; uint32_t off; }`、以及 `struct proc` 里的 `struct file ofile[NOFILE]`。`proc.c` 的 `proc_alloc()` 里 `memset(p->ofile, 0, sizeof(p->ofile))`（否则回收的槽位会把上一个进程的 fd 泄漏给新进程），`sys_fork()` 里 `memcpy(child->ofile, parent->ofile, sizeof(...))`。
14. **写三个系统调用，放在 `trap.c` 里。** 先写一个 `static struct file *fd_lookup(struct proc *p, int fd)` 把"范围检查 + `used` 检查"集中起来。`sys_open(name)`：`fs_lookup` → 找空槽 → 填 inum、`off = 0`、`used = 1` → 返回下标。`sys_read(fd, buf, len)`：`fd_lookup` → `fs_read` → **`f->off += got`** → 返回 `got`。`sys_close(fd)`：`fd_lookup` → 把 `used`/`inum`/`off` 全部清零（不只清 `used`，这样 close 后再用更容易暴露）。
    - **文件不存在返回 -1，绝不 panic。** 这是*用户程序*可能犯的错，不是内核的错误。fd 表满也返回 -1（真实 Unix 是 `EMFILE`）。只有内核自己的不变量被破坏才 panic。
    - `syscall.h` 加 `SYS_OPEN 6`/`SYS_READ 7`/`SYS_CLOSE 8`，分发处取第三个参数（x86_64 是用户态的 `rdx`）。
15. **重写 `user_prog.S`。** 一串文件读取测试：读 `hello.txt` 全文；用 128 字节的循环读完 `motd.txt`（会遇到一次 32 字节的短读）；读 `exact.txt` 验证整块边界；`open` 一个不存在的文件、确认返回 -1；同一个文件 `open` 两次、交错读、确认两个 fd 的偏移互不影响；**故意 `close` 同一个 fd 两次**、确认第二次返回 -1。最后打印 `user: all file tests passed`。

### riscv64 路线

1. **前 4 步和 x86_64 路线完全相同**（`fs_format.h`、`mkfs.c`、Makefile 镜像规则、`blk.h`）。如果先做的是 x86_64，这四个文件直接复制过来即可；`fs_format.h` 和 `mkfs.c` 本来就在架构目录之外，`blk.h` 要求逐字相同。
2. **先想清楚为什么这里不能抄 `ide.c`。** 两个独立的原因：RISC-V **没有** `in`/`out` 指令，不存在独立的 I/O 地址空间，所有设备访问都是 MMIO；而且 QEMU 的 `virt` 机器上**根本没有 ATA 控制器**。这不是"换个写法"，是换一套设备模型。
3. **在 `kernel_main.c` 里恒等映射 virtio 的 MMIO 窗口。** 把 `0x10001000..0x10008fff` **全部 8 个页**都映射上（扫描到空槽位是正常情况，不能因此触发缺页）。用恒等映射（VA == PA）和 UART 保持一致，这样代码里的地址和手册/设备树里的地址是同一个数。
   - 一个值得注意的巧合：`(0x10001000 >> 30) & 0x1ff == 0`，和 `0x10000000` 的 UART 落在**同一个顶层 PTE**（`KERNEL_UART_ROOT_INDEX` 就是 0）。所以 `pagetable_copy_kernel_range()` 原有的两个条目已经把它传播到每个进程的页表了，不需要额外改动。换成别的机器类型（比如 `sifive_u`）地址不同，就得连 `pagetable.c` 一起改。
4. **写 `virtio.c` 的 MMIO 访问器。** `mmio_read(off)`/`mmio_write(off, val)` 两个函数，指针**必须 `volatile`**（否则编译器会合并/消除对设备寄存器的访问），**而且必须是 32 位宽**——virtio-mmio 规范只定义 32 位访问，8 位或 64 位访问是未定义行为，QEMU 会报 guest error 或者直接返回 0。
5. **写 `virt_to_phys()`。** `KERNEL_VIRT_BASE 0xFFFFFFC000000000ull`，减掉即得物理地址。**这是本课程第一次需要它**：设备 DMA 不经过 MMU，驱动写进描述符里的地址必须是**物理地址**，而内核代码里拿到的指针是虚拟地址。`ide.c` 从来没有这个问题，因为那边是 CPU 亲自搬数据、用的就是 CPU 的地址视角。写错了症状是设备往一个不相干的物理地址写数据，而你的缓冲区一直是空的。
6. **写 `virtio_find_device()`。** 扫 8 个槽位（`0x10001000` + i * `0x1000`），依次检查 MagicValue == `0x74726976`、DeviceID == 2（块设备）、Version。**Version 读到 1 说明 QEMU 用的是 legacy 的 QueuePFN 方案**，这时不要硬撑，直接打印"请加 `-global virtio-mmio.force-legacy=false`"——错误信息要告诉人下一步做什么，这是本课程一贯的约定。
   - 即使 `run-qemu.sh` 已经把盘钉在 `bus.0` 上，这个扫描循环也值得保留：**环境约束和代码健壮性是两件事，不该互相替代**。真实驱动用设备树（riscv/ARM）或者 PCI 枚举（x86）来发现地址，扫槽位是这两者的简化版。
7. **写 `blk_init()` 的初始化握手。** 状态位是**累积**写入的，一位一位加上去：读 Status → 或上 `ACKNOWLEDGE(1)` 写回 → 或上 `DRIVER(2)` → 协商特性后或上 `FEATURES_OK(8)`、**回读确认这一位还在**（设备可以拒绝，这是四个状态位里唯一必须回读的）→ 最后或上 `DRIVER_OK(4)`。然后设置队列 0：写 QueueSel、检查 QueueNumMax、写 QueueNum = `VIRTQ_QUEUE_SIZE 8`、写三个队列结构的物理地址、写 QueueReady。
8. **准备队列内存。** 三部分：descriptor table、available ring、used ring。全部声明成 `static`（在 `.bss` 里，天然物理连续，而本 Lab 还没有能保证物理连续的多页分配器）并且带 `__attribute__((aligned(16)))`（规范对三者的要求分别是 16/2/4 字节，统一用 16 是安全地放宽）。每个结构必须**物理连续**，因为设备拿到的是一个起始物理地址加长度，它不会去走页表。
9. **写 `blk_read()` 的三描述符链。** 必须是三个描述符，不能塞成一个结构体：请求头（16 字节，driver→device）、数据缓冲区（512 字节，**device→driver，所以要打 `VIRTQ_DESC_F_WRITE` 标志**）、状态字节（1 字节，device→driver，也要打 WRITE）。
   - **`VIRTQ_DESC_F_WRITE` 是从设备的视角命名的**："设备写、驱动读"。第一次读很容易理解反。
   - **为什么不能是一个结构体**：WRITE 标志是**每个描述符独立**的，而这三段的方向不一样——这正是"描述符链"这个机制存在的理由。
10. **加内存屏障。** 把描述符索引写进 `avail.ring[]`、递增 `avail.idx` **之前**要有一条 `fence rw, rw`（保证设备看到新的 idx 时描述符内容已经可见），轮询 `used.idx` **之后**读数据之前也要一条（保证读到的是设备写完的数据，不是缓存里的旧值）。少了屏障的 bug 是间歇性的、和编译器优化等级相关的，非常难查。
11. **用 `g_used_seen` 记住已经处理到第几个完成项。** `used.idx` 是**单调递增**的，不会归零。"等 `used.idx == 1`"这种写法只在第一次读的时候对，第二次读就永远等不到了——症状是第一个块读成功、第二个块卡死。正确做法是保存上一次看到的值，等 `used.idx != g_used_seen`。
12. **`blk_init(); fs_init();` 的位置。** 除了 x86_64 路线第 11 步的三个约束，riscv64 还多一条：**必须在 `pagetable_activate()` 之后**——既因为要访问 MMIO 映射，也因为 `virt_to_phys()` 依赖 VA = PA + base 的自映射已经生效。
13. **后面的步骤（`fs.c`、位图自检、内核侧自检、fd 表、三个系统调用、`user_prog.S`）和 x86_64 路线的第 9、10、12、13、14、15 步完全一致。** `fs.c`/`fs.h` 和 `sys_open`/`sys_read`/`sys_close` 都要求和 x86_64 逐字相同，写完用 `make check-fs-iface` 确认。

## QEMU 运行命令

```bash
cd labs/lab08-filesystem

# 构建（会同时产出 kernel ELF 和 fs.img）
make VARIANT=solution ARCH=x86_64  build
make VARIANT=solution ARCH=riscv64 build

# 运行（run-qemu.sh 自动探测 build/fs.img 并挂上）
../../scripts/run-qemu.sh ARCH=x86_64  LAB=lab08-filesystem VARIANT=solution
../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab08-filesystem VARIANT=solution

# 故意不挂盘，观察 blk_init() 的报错路径
DISK=NONE ../../scripts/run-qemu.sh ARCH=riscv64 LAB=lab08-filesystem VARIANT=solution

# 检查两个架构的 fs.c / fs.h / blk.h 是否还一致
make check-fs-iface

# 在宿主机上直接看镜像，不启动内核
xxd -l 64 solution/x86_64/build/fs.img       # 超级块，开头应是魔数 4c 41 42 38
```

x86_64 的输出（节选，完整列表见 `tests/expect-x86_64.txt`）：

```
Hello OS from x86_64 (Lab8: filesystem)
...
ide: ATA primary master ready, PIO LBA28 read-only, IRQ 14 disabled
fs: mounted, magic ok, 64 blocks, 16 inodes, data from block 4
fs: bitmap self-check ok, 11/64 blocks in use
fs: kernel-side read of motd.txt (inode 4, 1824 bytes): === Lab8 motd.txt ===
filesystem ready, creating initial process
initial process created, entering scheduler
...
user: all file tests passed
```

riscv64 的输出只有前几行不同（`memmap:` 行在 banner 之前，驱动行是 virtio 的两行）：

```
memmap: 1 region(s) from DTB /memory,
Hello OS from riscv64 (Lab8: filesystem)
...
virtio: block device at slot 0 (0x10001000), version 2
virtio: queue 0 ready (8 descriptors), polled read-only
fs: mounted, magic ok, 64 blocks, 16 inodes, data from block 4
...
```

从 `fs: mounted` 这一行往后，两个架构的输出**完全一致**——这是抽象层立住了的直接证据。

**退出码 124 是正常的**，不是失败：`test-lab.sh` 用 `timeout` 跑内核，内核不会主动退出，所以超时是预期结果。脚本接受退出码 0 或 124，只按 expect 文件逐行 `grep -qF` 判定。

## GDB/QEMU Monitor 调试方法

**先在宿主机上把镜像看明白，再去内核里找 bug。** 这是本 Lab 最省时间的一条建议。`fs.img` 是一个普通文件，格式由你自己定义，用 `xxd` 就能逐块核对：

```bash
cd labs/lab08-filesystem/solution/x86_64
xxd -l 64          build/fs.img    # 块 0：超级块，魔数 + 各区起点
xxd -s 512  -l 16  build/fs.img    # 块 1：空闲块位图，11 个块在用 → 低两字节应是 ff 07
xxd -s 1024 -l 128 build/fs.img    # 块 2：inode 表的前两个 inode（各 64 字节）
xxd -s 2048 -l 128 build/fs.img    # 块 4：根目录内容，一串 32 字节的 dirent
```

如果超级块或者 inode 在镜像里就是错的，那 bug 在 `mkfs.c`，跟驱动和内核完全无关——这一步能砍掉一大半的搜索空间。

**分段验证长链路。** `sys_read` 读不到数据，可能的出错点有五处：fd 表、`fs_read` 的偏移算法、`read_inode`、`blk_read`、镜像本身。内核侧自检（绕过系统调用直接 `fs_lookup` + `fs_read`）把这条链切成两段；`xxd` 把镜像那一端摘掉。剩下的范围小到可以直接读代码。**把一条长链路拆成两段分别验证，是内核调试里最省时间的习惯。**

**x86_64：确认 ATA 状态寄存器。** 在 QEMU monitor（`Ctrl-A c`）里：

```
(qemu) info block          # 确认盘挂上了、路径对不对
(qemu) info registers
(qemu) x/8xb 0x1F7         # 注意：这看的是内存，不是端口；端口要靠驱动里的 kprintf
```

端口 I/O 在 monitor 里没法直接读，所以 `ide.c` 的两个等待函数在超限 panic 之前会把状态寄存器的值打出来——`BSY` 一直是 1 说明设备没响应（盘没挂上或者选盘写错了），`ERR` 是 1 说明命令本身被拒（LBA 超界了）。

**riscv64：MMIO 寄存器可以直接看。** 因为是恒等映射的普通内存地址：

```
(qemu) x/4xw 0x10001000    # MagicValue 应是 0x74726976，之后是 Version / DeviceID / VendorID
(qemu) x/4xw 0x10001070    # Status 寄存器，DRIVER_OK 之后应是 0xf
```

DeviceID 读到 0 说明这个槽位是空的（盘被 QEMU 分到别的槽了，见"常见坑"第 3 条）；Version 读到 1 说明缺 `force-legacy=false`。

**用 `-d int` 定位三重故障，不要用 `-d cpu_reset`。** 这是本课程 `docs/verification-methodology.md` 记录过的一个陷阱：加了 `-no-reboot` 之后，三重故障时 QEMU 是**停机**而不是重置，`-d cpu_reset` 的输出和健康启动一模一样（都是 2 条 `CPU Reset`），**是假阴性**。两种可行的办法：

`run-qemu.sh` 不接受透传的 QEMU 参数（给它未知参数会直接报错），所以这两个判据要手写 qemu 命令。在 `solution/x86_64/` 目录下：

```bash
# 办法一：去掉 -no-reboot，数重置次数和 banner 次数（健康基线：恰好 2 次重置、1 次 banner）
timeout 15 qemu-system-x86_64 -cdrom build/os.iso \
    -drive file=build/fs.img,format=raw,index=0,if=ide \
    -serial file:/tmp/vm_serial.txt -display none \
    -d cpu_reset -D /tmp/vm_reset.log
grep -c 'CPU Reset'     /tmp/vm_reset.log    # > 2 就是在重启循环
grep -c 'Hello OS from' /tmp/vm_serial.txt   # > 1 就是在重启循环

# 办法二：保留 -no-reboot，在 -d int 日志里找双故障
timeout 12 qemu-system-x86_64 -cdrom build/os.iso \
    -drive file=build/fs.img,format=raw,index=0,if=ide \
    -serial file:/tmp/vm_serial.txt -display none -no-reboot \
    -d int -D /tmp/vm_int.log
grep -n 'check_exception old: 0x8' /tmp/vm_int.log   # 出现即确定的三重故障
grep -n 'v=0d'                     /tmp/vm_int.log   # 本 Lab 第 1 条坑的 #GP
```

顺带一句：日志里的 `Servicing hardware INT=0x0e` / `INT=0x08` 是固件实模式的 BIOS 调用，**不是**内核的异常——健康启动和崩溃启动里这些行的数量完全一样，别被它们带偏。

**GDB 断点位置。** 驱动层断 `blk_read`，看 `blkno` 对不对；文件系统层断 `read_inode`，看算出来的块号和块内下标；系统调用层断 `sys_read`，看 `f->off` 有没有在累加。

```bash
bash scripts/debug-gdb.sh ARCH=riscv64 LAB=lab08-filesystem VARIANT=solution
```

```gdb
target remote :1234
file solution/riscv64/build/kernel.elf
break blk_read
commands
  print/x blkno
  continue
end
break read_inode
break sys_read
```

在 `sys_read` 的断点上 `print *f` 看 `struct file` 的 `inum`/`off`，连续几次 `continue` 就能确认 `off` 是不是在累加——如果 `off` 一直是 0，漏的就是 `f->off += got;` 那一行。

**`readelf` 之前一定先 `make clean`。** 陈旧的 `build/` 产物会安静地和当前源码矛盾，让人对着一个已经不存在的问题查半天。

## 自动验收测试

```bash
# 单个 Lab 单个架构
bash ../../scripts/test-lab.sh ARCH=x86_64  LAB=lab08-filesystem
bash ../../scripts/test-lab.sh ARCH=riscv64 LAB=lab08-filesystem

# 全课程回归（Lab0~Lab8，两个架构）
bash ../../scripts/run-all-tests.sh
```

判定方式：把内核输出跑到超时，然后对 `tests/expect-{arch}.txt` 里的每一行做 `grep -qF`（固定字符串，不是正则）。x86_64 20 行、riscv64 21 行。退出码 0 和 124 都算通过。

这些期望行覆盖了五段独立的链路，任何一段断了都会精确地指向一个层：

1. **驱动就绪**（`ide: ...` / `virtio: ...`）——块设备层通了。
2. **挂载 + 格式校验**（`fs: mounted, magic ok, 64 blocks, ...`）——超级块读对了，`mkfs` 和内核对格式的理解一致。
3. **位图自检**（`fs: bitmap self-check ok, 11/64 blocks in use`）——微型 `fsck`。11 = 4 个元数据块（超级块 1 + 位图 1 + inode 表 2）+ 7 个数据块（根目录 1 + hello.txt 1 + motd.txt 4 + exact.txt 1）。这个数字变了说明 `fsroot/` 的内容变了，期望文件要跟着更新。
4. **内核侧读取**（`fs: kernel-side read of motd.txt (inode 4, 1824 bytes): ...`）——`fs.c` 这一层独立可用，**不经过系统调用**。注意 `inode 4` 是硬编码的，这就是 Makefile 里必须 `$(sort)` 的原因。
5. **用户态读取**（一串 `user:` 行，以 `user: all file tests passed` 结束）——系统调用路径通了，含短读、整块边界、打开不存在的文件返回 -1、两个 fd 独立偏移、重复 close 返回 -1。

如果第 4 段过了而第 5 段没过，bug 一定在 `trap.c` 的三个系统调用或者 fd 表里，不用去看驱动。

## 常见坑与排查

以下九条都是本 Lab 开发过程中**实测踩到、定位、修复过的真实 bug，不是猜测或者理论上可能发生的情况**。

**1. x86_64：只读、纯轮询的 ATA 驱动被 IRQ 14 打成三重故障。** 症状极具误导性：`blk_init()` 正常打印成功信息，`fs_init()` 一个字都不打印，看起来像是卡死了——但**每个轮询循环都有上限，不可能死循环**，而且没有任何 panic 输出。真相是三重故障，而在 `-no-reboot -no-shutdown` 下三重故障和死循环的表现完全一样（QEMU 停机、屏幕不动）。用 `-d int` 看就清楚了：`v=0d e=0172`（#GP）→ `check_exception old: 0xd new 0xd` → `v=08`（#DF）→ 三重故障。错误码 `0x172` 要按位读：bit1 = 1 表示这是 IDT 相关的，索引是 `0x172 >> 3 = 46`，而 46 = PIC 基址 32 + IRQ 14（ATA 主通道）。本课程的 `IDT_ENTRIES` 是 33，第 46 项根本不存在，于是 #GP → #DF → 三重故障。**教训：轮询式驱动也必须处理中断。** 不注册处理程序并不意味着你收不到中断，只意味着你死得更难看。修法两处都要做：往 `0x3F6` 写 nIEN 位让设备别发中断，**并且**在 `blk_init()` 里屏蔽从片 PIC 上对应的那一位。

**2. x86_64：`-cdrom` 和 `-drive index=2` 冲突。** QEMU 直接拒绝启动并报 "drive index 2 used twice"。原因是 `-cdrom` 就是 IDE 从盘、占的正是 `index=2`。挂 `fs.img` 要用 `index=0`（主盘），这也和驱动里"只支持主通道主盘"的简化一致。

**3. riscv64：单块盘被分到 `virtio-mmio-bus.7`，不是 `.0`。** 症状是扫描 8 个槽位一个设备都找不到，或者在 `0x10001000` 上读 MagicValue 读到 0。原因是 **QEMU 从高到低分配 virtio-mmio 槽位**：只挂一块盘时它落在 `virtio-mmio-bus.7`，也就是 `0x10008000`，而绝大多数教程里硬编码的是 `0x10001000`。两个修法互补，本 Lab 两个都做了：在 `-device` 上显式写 `bus=virtio-mmio-bus.0` 把它钉住，同时驱动里保留扫描 8 个槽位的循环。这也是为什么 `kernel_main.c` 必须把 8 个页**全部**映射——扫到空槽位是正常流程，不能因此触发缺页。

**4. riscv64：不加 `force-legacy=false` 时 Version 寄存器读到 1。** QEMU 的 virtio-mmio 默认是 legacy 模式（用 QueuePFN 那套更老的队列地址寄存器），而现代驱动写的是 QueueDescLow/High 那一组。症状是初始化"看起来成功"但设备从不产生完成项，`used.idx` 永远是 0。修法是给 QEMU 加 `-global virtio-mmio.force-legacy=false`；驱动侧检测到 Version == 1 时应当**把这个命令行参数原文打出来**，而不是只说"版本不支持"。

**5. riscv64：忘了 `virt_to_phys()`，设备往错误的物理地址 DMA。** 症状是块读"成功"（`used.idx` 正常递增、状态字节是 0）但缓冲区全是 0。原因是描述符里填的是内核虚拟地址（`0xFFFFFFC0...`），而**设备 DMA 不经过 MMU**，它就按这个数当物理地址去写了。这是本课程第一次"CPU 的地址视角"和"设备的地址视角"分岔，`ide.c` 侧永远不会遇到，因为那边是 CPU 亲自搬数据。凡是要交给设备的地址，一律先过 `virt_to_phys()`。

**6. riscv64：`VIRTQ_DESC_F_WRITE` 的方向理解反了。** 这个标志是**从设备的视角**命名的：置位 = "设备写、驱动读"。所以三个描述符里，请求头（driver→device）**不**置位，数据缓冲区和状态字节（device→driver）**都要**置位。理解反了的症状是设备拒绝请求或者数据/状态没被写回。顺带解释了为什么这三段不能塞进一个结构体用一个描述符——WRITE 标志是每描述符独立的，而三段方向不同，这正是描述符链存在的理由。

**7. riscv64：缺内存屏障，或者用 `used.idx == 1` 判完成。** 两个独立的坑，症状都是间歇性的。屏障：发布 `avail.idx` 前、观察 `used.idx` 后各需要一条 `fence rw, rw`，少了之后 bug 和编译器优化等级相关，换个 `-O` 等级就变。`g_used_seen`：`used.idx` 是单调递增的，等 `== 1` 只有第一次对，第二次读就永远等不到——症状是"第一个块读成功、第二个块卡死"，很容易误判成驱动只能用一次。

**8. 按字节截断 UTF-8 文本，终端显示成替换字符。** 内核侧自检原本按固定 31 字节截断 `motd.txt` 的开头来打印，结果把一个 3 字节的中文字符切成两半，终端显示出一个替换字符，看起来像是**读出来的数据错了**。实际上每个字节都是对的，**"按字节截断 UTF-8 文本"本身才是问题**。修法是把截断点挪到已知是 ASCII 的 `'\n'` 上。真正的按字符截断需要在更高层做 UTF-8 解码，不属于本 Lab 的范围。这条坑的价值在于：**看到乱码先问"是数据错了还是显示错了"**，这两件事的排查方向完全相反。

**9. 构建相关的两条。** 其一，`mkfs` 必须用宿主编译器：写成 `$(CC)` 会用交叉编译器产出一个宿主机跑不了的二进制，报 "cannot execute binary file"，所以 Makefile 里单独有一个 `HOSTCC ?= cc`。其二，`FSROOT_FILES` 外面的 `$(sort)` 是必需的：`mkfs` 按命令行顺序分配 inode 号，而期望文件里硬编码了 `motd.txt` 是 inode 4；`$(wildcard)` 的顺序依赖文件系统，不排序的话换台机器 inode 号就变了，测试会莫名其妙地失败。**构建产物必须可复现**，否则测试的稳定性就成了运气问题。

另外两条不是 bug 但容易困惑的地方：**`panic()` 不是可变参数的**（`#define panic(msg) kernel_panic(__FILE__, __LINE__, (msg))`），致命路径上的惯例是先 `kprintf(...)` 打细节、再 `panic("一句话")`；**`kprintf` 不支持宽度和精度**（只有 `%d %u %x %X %p %s %c %%` 加 `l` 长度修饰符），64 位值要用 `%lx`/`%lu`/`%p`。

## 挑战任务

按从易到难排列，前三个是一两小时的量，最后两个基本是一个独立的小 Lab。

1. **把控制台也变成 fd 表里的一项。** 本 Lab 有一处刻意的不对称：`write` 只接受 fd 1（控制台），`read` 只接受 `open()` 出来的文件 fd。真实 Unix 里两者都是 fd 表里的普通条目。给 `struct file` 加一个 `type` 字段（`FD_CONSOLE` / `FD_INODE`），在 `proc_alloc()` 里把 fd 1 预置成控制台，然后让 `sys_write` 也走 `fd_lookup()`。做完这一步，Lab9 的输出重定向就只是"把 fd 1 指向一个 inode"而已。
2. **实现真正的路径解析。** 让 `fs_lookup("/a/b/c")` 能工作。不需要任何新的磁盘结构——目录已经是普通文件了，只要在现有 `fs_lookup()` 外面套一层"按 `/` 切分、逐段查找、用上一段的 inum 当下一段的起点"。`mkfs` 已经在根目录里写好了 `.` 和 `..`（都指向 inum 1），所以 `..` 的语义一开始就能验证；缺的是让 `mkfs` 支持创建子目录。
3. **加 `lseek` 和 `fstat`。** `lseek(fd, off, whence)` 只是改 `f->off` 一个字段——但正是这个"只是"说明了把偏移放进内核的价值。`fstat` 把 inode 的 type/size 返回给用户态，需要设计一个 `struct stat` 并处理用户态缓冲区的写入。
4. **实现间接块，把单文件上限从 4 KiB 提上去。** 现在 `addrs[]` 有 8 项、每项一个块，上限就是 4096 字节。经典做法是把最后一项改成"指向一个装满块号的块"（512 / 4 = 128 个），上限变成 (7 + 128) * 512 = 67.5 KiB。`mkfs.c` 和 `fs_read()` 都要改，而且 `fs_read()` 里"偏移转块号"的逻辑会从一次除法变成一个两级判断——这是真实文件系统里 inode 结构最核心的一段代码。
5. **让文件系统可写，并给它加一段日志。** 先实现 `balloc`（在位图里找空闲块）/`iupdate`（把内存里的 inode 写回磁盘）/`fs_write`/`sys_write` 对文件的分支/`fs_create`；跑通之后你会拥有一个**能被掉电破坏的文件系统**，"核心概念"一节列的三种损坏形态都可以在 QEMU 里用 `kill -9` 复现出来。然后加日志区：把"这次操作要写哪几个块"先完整写进日志、打提交标记、再写目标位置；启动时重放已提交的事务、丢弃未提交的。这是 xv6 的 `log.c`，也是 ROADMAP 里"讨论但不强制实现"的那部分——真要做，它比本 Lab 的其余全部内容加起来还难。
6. **给 `blk_read()` 加一层块缓存。** 一个小的 `struct buf` 数组 + LRU 替换，避免重复读同一块（`fs_lookup` 现在每次都要重读根目录）。做完之后立刻会撞上一个 Lab7 埋下的问题：缓存是**共享**的，多进程并发访问需要锁，而本课程的锁在 Lab10。这正好说明了为什么本 Lab 的所有缓冲区都在栈上——没有共享就没有竞争。

## 参考

- xv6-riscv 的 `fs.c` / `file.c` / `log.c` / `virtio_disk.c`：本 Lab 的磁盘格式和 `fs.c` 的结构直接参考它，可写路径和日志是挑战任务 5 的标准答案。<https://github.com/mit-pdos/xv6-riscv>
- 《xv6: a simple, Unix-like teaching operating system》第 8 章 "File system"：崩溃一致性和日志那一节值得逐段读。<https://pdos.csail.mit.edu/6.828/2023/xv6/book-riscv-rev3.pdf>
- ATA/ATAPI Command Set (ACS)，以及 OSDev Wiki 的 "ATA PIO Mode"：`0x3F6` 读写方向不同、400 ns 延迟的来历都在这里。<https://wiki.osdev.org/ATA_PIO_Mode>
- Virtual I/O Device (VIRTIO) Version 1.2 规范：第 2.7 节（virtqueue）和第 4.2 节（MMIO 传输）是 `virtio.c` 的依据，状态位的累积顺序和 FEATURES_OK 必须回读都是规范的硬要求。<https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html>
- QEMU `virt` 机器的设备树：`qemu-system-riscv64 -M virt -machine dumpdtb=virt.dtb` 然后 `dtc -I dtb -O dts virt.dtb`，可以看到 8 个 virtio-mmio 节点的真实地址，以及真实驱动本该用什么方式发现设备。
- 本课程 `docs/verification-methodology.md`：三重故障的正确检测方法，以及 `-no-reboot` + `-d cpu_reset` 为什么是假阴性。

## 下一步

进入 [Lab9：Shell 与用户程序](../lab09-shell-userspace/README.md)。

Lab8 结束时，你的内核已经能从磁盘上按名字读出文件内容——但能读的还只是一个手写在 `user_prog.S` 里的、被编进内核镜像的汇编程序。Lab9 要把这最后一层写死的东西拆掉：实现一个 **ELF 加载器**，让 `exec()` 从**文件系统里**读取程序并装载运行；写一个**极简 libc**，让用户程序可以用 C 而不是汇编写；然后用这两样东西做出一个真正的 **shell**——能跑 `ls`、`echo`，能把命令的输出重定向到文件，能用管道把两个程序接起来。

到那时候，"从磁盘加载并运行一个用户程序"这条链路上的每一环都是你自己写的。本 Lab 的挑战任务 1（把控制台变成 fd 表里的一项）是 Lab9 输出重定向最直接的铺垫，如果有时间不妨先做掉。

