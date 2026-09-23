# Lab9: shell 与用户程序——在 Lab8 的文件系统上加一层进程执行环境：ELF
# 加载器、管道、一个极简 libc，六个能在其中运行的用户程序（init/sh/ls/cat/
# echo/grep），把 Lab7 手写汇编的单个 user_prog 换成真正"从磁盘加载、C 语言
# 写成、多个独立程序互相 fork/exec/管道"的用户态。
#
# 用法:
#   make build ARCH=x86_64|riscv64 [VARIANT=solution|starter]
#   make fs.img            # 只生成磁盘镜像（build 会自动依赖它）
#   make check-shared-iface  # 检查两个架构共享文件是否还一致
#   make clean
#
# 结构继承 labs/lab08-filesystem/Makefile，见那边的注释；本 Lab 新增/改动
# 的部分有四块：
#
#   1. ARCH_SRCS 去掉了 user_blob.S（Lab7/Lab8 那种"把单个用户程序的裸二
#      进制嵌进内核镜像"的模式已经不存在了），加了 exec.c（ELF 加载器）和
#      pipe.c（管道实现）。两者都是架构无关的文件系统层旁边新增的模块，
#      跟 fs.c 一样两个架构各有一份、要求逐字节相同。
#
#   2. 全新的用户程序构建流水线（USER_DIR 下）：6 个 .c/.S 源文件集合
#      （ulib.c + crt0_<arch>.S + usys_<arch>.S + 6 个程序各自的 .c）分别
#      编译链接成 6 个 ELF 可执行文件，用 user.ld（两个架构共用一份，见
#      该文件顶部注释）链接、-z max-page-size=4096 收紧段对齐。产物不再
#      objcopy 成裸二进制——它们要作为*文件*躺进磁盘镜像，由内核的 ELF
#      加载器在运行时解析，不再是构建期嵌入内核镜像的字节块。
#
#   3. fs.img 的输入列表从"fsroot/ 下的几个文本文件"扩展成"fsroot/ 的文本
#      文件 + 6 个刚构建出的用户程序 ELF"。mkfs.c 本身没有变化（见其顶部
#      注释）：它只是被喂了更多文件。inode 号的稳定性因此更要紧——见下面
#      FS_IMG_INPUTS 的排序说明。
#
#   4. check-fs-iface 改名 check-shared-iface，覆盖面从三个文件（fs.c/
#      fs.h/blk.h）扩大到六个（加上 exec.c/pipe.c/pipe.h）——这三个新文件
#      同样"两个架构各一份、必须相同"，理由跟 fs.c 完全一样（见其顶部
#      注释）：它们描述的是与硬件无关的逻辑（ELF 解析、环形缓冲区管道），
#      架构差异已经被 blk.h/pagetable.h 那一层挡掉了。

ARCH ?= x86_64
VARIANT ?= solution

LAB_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(LAB_ROOT)/../..)
COMMON_DIR := $(REPO_ROOT)/src/common
ARCH_DIR := $(LAB_ROOT)/$(VARIANT)/$(ARCH)
BUILD_DIR := $(ARCH_DIR)/build

include $(COMMON_DIR)/toolchain.mk

# -I$(LAB_ROOT)：内核代码 #include "fs_format.h"/"elf.h"/"syscall.h"——这
# 三个头文件只在 Lab 根目录放一份（跟 Lab8 的 fs_format.h 同一个理由，
# 见那三个文件各自顶部的说明），两个架构的内核共用。
CFLAGS := $(CFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -I$(LAB_ROOT) -g
ifeq ($(ARCH),x86_64)
CFLAGS += -mcmodel=kernel
endif
ASFLAGS := $(ASFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -g
LDFLAGS := $(LDFLAGS_$(ARCH))

COMMON_SRCS := $(COMMON_DIR)/console.c $(COMMON_DIR)/panic.c $(COMMON_DIR)/string.c $(COMMON_DIR)/kalloc.c
ARCH_SRCS := $(ARCH_DIR)/boot.S $(ARCH_DIR)/console_putc.c $(ARCH_DIR)/panic_arch.c $(ARCH_DIR)/kernel_main.c $(ARCH_DIR)/memmap.c $(ARCH_DIR)/pagetable.c $(ARCH_DIR)/trap.c $(ARCH_DIR)/trap_entry.S $(ARCH_DIR)/proc.c $(ARCH_DIR)/swtch.S $(ARCH_DIR)/fs.c $(ARCH_DIR)/exec.c $(ARCH_DIR)/pipe.c
ifeq ($(ARCH),x86_64)
ARCH_SRCS += $(ARCH_DIR)/pit.c $(ARCH_DIR)/ide.c
else
ARCH_SRCS += $(ARCH_DIR)/sbi.c $(ARCH_DIR)/virtio.c
endif

ALL_SRCS := $(COMMON_SRCS) $(ARCH_SRCS)

OBJ_DIR := $(BUILD_DIR)/obj
OBJS := $(addprefix $(OBJ_DIR)/,$(notdir $(ALL_SRCS:.S=.o)))
OBJS := $(OBJS:.c=.o)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
LINKER_SCRIPT := $(ARCH_DIR)/linker.ld

vpath %.c $(COMMON_DIR) $(ARCH_DIR)
vpath %.S $(ARCH_DIR)

# ---------------------------------------------------------------------------
# 用户程序构建流水线
# ---------------------------------------------------------------------------
# 用户源码只有一份（不分 solution/starter 的 user/ 目录——用户态程序本身
# 不是本 Lab 的 TODO 骨架对象，starter 的教学内容全在内核侧的 exec.c/
# pipe.c/proc.c 里；用户程序对学生来说是"已经能跑的应用"，用来验证内核
# 是否写对）。VARIANT 只影响内核，不影响 USER_DIR。
USER_DIR := $(LAB_ROOT)/solution/user
USER_BUILD_DIR := $(BUILD_DIR)/user
USER_OBJ_DIR := $(USER_BUILD_DIR)/obj

# 用户态编译选项跟内核选项是两套完全独立的 flags，不能从 CFLAGS 派生：
#
#   - x86_64：内核用 -mcmodel=kernel（假设代码在虚拟地址高 2GB，配合
#     KERNEL_VIRT_BASE），用户程序链接到 0x400000，跟这个假设不搭，必须
#     去掉；-mno-red-zone 内核需要（中断可能在任意指令边界打进来，红区
#     没有栈切换保护），用户程序没有这个顾虑，但留着也无害，为了两套
#     flags 差异最小化特意保留。
#   - riscv64：内核用 -mabi=lp64d（启用 D 扩展，即双精度浮点寄存器可用
#     于参数传递）配 -mcmodel=medany（PC 相对寻址，代码可以放在地址空间
#     任何位置，不要求在低 2GB）；用户程序改用 -march=rv64imac（去掉 f/d
#     浮点扩展——用户程序没有一行浮点运算，没必要为不存在的功能生成不同
#     的调用约定）配 -mabi=lp64（不带 d 后缀，跟 march 的浮点能力对应）。
#     mcmodel 选项对用户态没有意义（那是内核态"代码在哪个地址范围"的
#     指令编码策略），直接省略，编译器会用默认值。
#
# 这组差异不是随手选的，是实测编译 solution/user/*.c 验证过的：两套 flags
# 下全部 6 个程序 + ulib.c 都能干净编译，且 riscv64 用户程序确实不含任何
# 浮点指令（没有理由要求 D 扩展）。
USER_CFLAGS_x86_64  := -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -Wall -Wextra $(FILE_PREFIX_MAP)
USER_ASFLAGS_x86_64 := -m64 -ffreestanding
USER_CFLAGS_riscv64  := -march=rv64imac -mabi=lp64 -ffreestanding -fno-stack-protector -fno-pic -Wall -Wextra $(FILE_PREFIX_MAP)
USER_ASFLAGS_riscv64 := -march=rv64imac -mabi=lp64 -ffreestanding

# -I$(LAB_ROOT)：usys_<arch>.S 里 #include "../../syscall.h"——等一下，
# 那是相对路径写死的，不需要 -I；但 crt0/程序本身如果哪天要用到 elf.h
# 里的常量（目前不需要），这条 -I 留着不吃亏。真正必需的是 -I$(USER_DIR)
# 本身：6 个程序的 .c 文件都 #include "user.h"/"types.h"，这两个头文件
# 就在 USER_DIR 里，跟自己平级 #include 不需要额外 -I，但 vpath 展开后
# 编译器的"当前目录"是 Makefile 所在目录，所以还是要显式给出。
USER_CFLAGS := $(USER_CFLAGS_$(ARCH)) -I$(USER_DIR) -I$(LAB_ROOT) -g
USER_ASFLAGS := $(USER_ASFLAGS_$(ARCH)) -g
USER_LDFLAGS := -nostdlib -static -z max-page-size=4096

USER_LD := $(USER_DIR)/user.ld
USYS_SRC := $(USER_DIR)/usys_$(ARCH).S
CRT0_SRC := $(USER_DIR)/crt0_$(ARCH).S

# 6 个用户程序，每个由"自己的 .c + ulib.c + usys_<arch>.S + crt0_<arch>.S"
# 四个目标文件链接而成。用一个 make 函数生成这四条规则，而不是手写 6 遍
# ——6 份几乎相同的规则里任何一处笔误（比如某个程序漏链 usys）都会在
# 运行时才暴露成"这个程序调用系统调用直接跳飞"，而不是链接错误，非常
# 难查。宏保证结构一致性，程序名只出现在一个地方（下面的 USER_PROGS 里）。
USER_PROGS := init sh ls cat echo grep

define USER_PROG_RULE
$(USER_OBJ_DIR)/$(1).o: $(USER_DIR)/$(1).c $(USER_DIR)/user.h $(USER_DIR)/types.h | $(USER_OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $(USER_DIR)/$(1).c -o $$@

$(USER_BUILD_DIR)/$(1): $(USER_OBJ_DIR)/$(1).o $(USER_OBJ_DIR)/ulib.o $(USER_OBJ_DIR)/usys.o $(USER_OBJ_DIR)/crt0.o $(USER_LD) | $(USER_BUILD_DIR)
	$(LD) $(USER_LDFLAGS) -T $(USER_LD) -o $$@ $(USER_OBJ_DIR)/crt0.o $(USER_OBJ_DIR)/$(1).o $(USER_OBJ_DIR)/ulib.o $(USER_OBJ_DIR)/usys.o
endef

$(foreach p,$(USER_PROGS),$(eval $(call USER_PROG_RULE,$(p))))

$(USER_OBJ_DIR)/ulib.o: $(USER_DIR)/ulib.c $(USER_DIR)/user.h $(USER_DIR)/types.h | $(USER_OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $(USER_DIR)/ulib.c -o $@

$(USER_OBJ_DIR)/usys.o: $(USYS_SRC) $(LAB_ROOT)/syscall.h | $(USER_OBJ_DIR)
	$(AS) $(USER_ASFLAGS) -c $(USYS_SRC) -o $@

$(USER_OBJ_DIR)/crt0.o: $(CRT0_SRC) | $(USER_OBJ_DIR)
	$(AS) $(USER_ASFLAGS) -c $(CRT0_SRC) -o $@

$(USER_OBJ_DIR) $(USER_BUILD_DIR):
	mkdir -p $@

USER_PROG_BINS := $(addprefix $(USER_BUILD_DIR)/,$(USER_PROGS))

.PHONY: user
user: $(USER_PROG_BINS)

# ---------------------------------------------------------------------------
# 宿主工具 mkfs + 磁盘镜像
# ---------------------------------------------------------------------------
# 注意这里用的是 HOSTCC（宿主编译器）而不是 $(CC)（交叉编译器）：mkfs 要在
# 你的开发机上运行，编译出 riscv64 或裸机 x86 的二进制是跑不起来的。这是
# 交叉编译工程里一个非常常见的翻车点——一旦把宿主工具误用交叉编译器构建，
# 报错会是 "cannot execute binary file"，而不是编译错误。
HOSTCC ?= cc
HOSTCFLAGS ?= -std=c11 -Wall -Wextra -O1 -I$(LAB_ROOT)

MKFS_SRC := $(LAB_ROOT)/mkfs/mkfs.c
MKFS_BIN := $(LAB_ROOT)/mkfs/mkfs

# 磁盘镜像放在 build/ 下，跟 kernel.elf 并列：run-qemu.sh 会自动发现
# $BUILD_DIR/fs.img 并把它挂给 QEMU（见那个脚本里的 DISK 处理），所以
# 这里的路径不是随意选的，改了要同步改脚本。
#
# 这几个变量必须定义在下面 build 规则*之前*：make 在读到一条规则时就会
# 展开它的依赖列表，此处如果 FS_IMG 还是空的，build 就不会依赖镜像，
# 症状是"第一次 make 之后镜像不存在，内核起来直接 panic"。这类"变量定义
# 晚于使用"的错误在 Makefile 里不会报错，只会静默少一条依赖。
FS_IMG := $(BUILD_DIR)/fs.img

# 镜像内容：fsroot/ 下的所有文件，加上刚构建出的 6 个用户程序 ELF。
#
# 依赖写成 wildcard，改了 fsroot/ 文件内容会重新生成镜像；但*新增*文件
# 需要 make 重新读一遍 Makefile 才能被 wildcard 看到——加文件后如果镜像
# 没更新，`make clean` 一下即可。
#
# $(sort) 不只是为了整齐——它决定了 inode 号。mkfs 按命令行顺序依次分配
# inode，而 $(wildcard) 返回的是目录项顺序，不同文件系统/不同机器上可能
# 不一样。不排序的话，同一份 fsroot/ 在两台机器上会生成 inode 号不同的
# 镜像，而 tests/expect-*.txt 里写着具体的 inode 号，测试就会随机失败。
# 凡是"构建输出被外部顺序影响"的地方都要显式定序，这是可重复构建的基本要求。
#
# fsroot 的文本文件排在用户程序前面，用户程序自己保持 USER_PROGS 里写的
# 固定顺序（init sh ls cat echo grep）——两段各自内部顺序固定就足以保证
# inode 号可重复，不需要把两段合并再统一排序。
FSROOT_FILES := $(sort $(wildcard $(LAB_ROOT)/fsroot/*))
FS_IMG_INPUTS := $(FSROOT_FILES) $(USER_PROG_BINS)

.PHONY: build clean iso fs.img mkfs check-shared-iface

# build 依赖 fs.img：没有磁盘镜像的话内核起来会在 fs_init() 里 panic，
# 让"忘了生成镜像"变成一次构建就能发现的事，而不是运行时才炸。
build: $(KERNEL_ELF) $(FS_IMG)
ifeq ($(ARCH),x86_64)
build: iso
endif

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

mkfs: $(MKFS_BIN)

$(MKFS_BIN): $(MKFS_SRC) $(LAB_ROOT)/fs_format.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(MKFS_SRC)

fs.img: $(FS_IMG)

$(FS_IMG): $(MKFS_BIN) $(FS_IMG_INPUTS) | $(OBJ_DIR)
	$(MKFS_BIN) $@ $(FS_IMG_INPUTS)

# ---------------------------------------------------------------------------
# 两个架构之间的一致性检查
# ---------------------------------------------------------------------------
# fs.c/fs.h/blk.h/exec.c/pipe.c/pipe.h 在两个架构目录下各有一份、且必须
# 逐字节相同（理由见 fs.c 顶部注释：它们描述的是硬件无关的逻辑，架构差异
# 已经被 blk.h 这一层挡掉了，两份不同就意味着某处出现了不该有的分歧）。
# 刻意的重复需要机械化的守卫，否则"改了一边忘了另一边"只会在另一个架构
# 的测试跑起来时才暴露，而且症状看起来毫不相干。
#
# 本 Lab 比 Lab8 多了 exec.c/pipe.c/pipe.h 三个文件——ELF 加载器和管道都
# 是新增的架构无关模块，跟 fs.c 遵循同一条约定。
#
# solution 和 starter 两对都查：starter 里这些文件是带 TODO 的骨架，同样
# "两个架构一份答案"，所以同样需要守卫。注意是"x86_64 对 riscv64"，不是
# "starter 对 solution"——后者本来就该不同（一个是 TODO 骨架，一个是
# 实现）。
#
# 用 cmp 而不是 diff：这里只关心"是否相同"，cmp 的退出码就够了，而且
# 不一致时 cmp 会直接指出第一个不同的字节位置。
FS_SHARED_FILES := fs.c fs.h blk.h exec.c pipe.c pipe.h
FS_SHARED_VARIANTS := solution starter

check-shared-iface:
	@for v in $(FS_SHARED_VARIANTS); do \
		for f in $(FS_SHARED_FILES); do \
			if [ ! -f $(LAB_ROOT)/$$v/x86_64/$$f ] && [ ! -f $(LAB_ROOT)/$$v/riscv64/$$f ]; then \
				continue; \
			fi; \
			if ! cmp -s $(LAB_ROOT)/$$v/x86_64/$$f $(LAB_ROOT)/$$v/riscv64/$$f; then \
				echo "错误: $$v/x86_64/$$f 和 $$v/riscv64/$$f 不一致"; \
				echo "  这两份文件必须逐字节相同（见 fs.c 顶部注释）。差异:"; \
				diff -u $(LAB_ROOT)/$$v/x86_64/$$f $(LAB_ROOT)/$$v/riscv64/$$f | head -30; \
				exit 1; \
			fi; \
		done; \
	done
	@echo "check-shared-iface: fs.c/fs.h/blk.h/exec.c/pipe.c/pipe.h 在 solution 和 starter 下各自两个架构一致"

# ---------------------------------------------------------------------------
# 内核构建
# ---------------------------------------------------------------------------
$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S | $(OBJ_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)

ISO_DIR := $(BUILD_DIR)/isodir
OS_ISO := $(BUILD_DIR)/os.iso

iso: $(OS_ISO)

$(OS_ISO): $(KERNEL_ELF) $(ARCH_DIR)/grub.cfg
ifeq ($(GRUB_MKRESCUE),)
	$(error 没找到可用的 grub-mkrescue，见 docs/environment.md 的安装说明。Linux: apt install grub-pc-bin grub-common xorriso mtools | macOS: brew install i686-elf-grub xorriso mtools)
endif
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp $(ARCH_DIR)/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

clean:
	rm -rf $(LAB_ROOT)/solution/x86_64/build $(LAB_ROOT)/solution/riscv64/build
	rm -rf $(LAB_ROOT)/starter/x86_64/build $(LAB_ROOT)/starter/riscv64/build
	rm -f $(MKFS_BIN)
