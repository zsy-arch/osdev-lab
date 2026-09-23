# Lab8: 文件系统——在 Lab7 的多进程内核上加一层只读文件系统，让用户程序
# 能通过 open/read/close 读到磁盘上真实存在的文件。
#
# 用法:
#   make build ARCH=x86_64|riscv64 [VARIANT=solution|starter]
#   make fs.img        # 只生成磁盘镜像（build 会自动依赖它）
#   make check-fs-iface  # 检查两个架构的 fs.c/fs.h/blk.h 是否还一致
#   make clean
#
# 结构继承 labs/lab07-process-scheduler/Makefile，见那边的注释。本 Lab
# 新增的部分有三块：
#
#   1. ARCH_SRCS 多了 fs.c（架构无关的文件系统层）和一个架构专属的块
#      设备驱动：x86_64 是 ide.c（ATA PIO），riscv64 是 virtio.c
#      （virtio-blk MMIO）。两者都只对上层暴露 blk.h 里那两个函数。
#
#   2. 一个*宿主机*上运行的工具 mkfs/mkfs.c，用宿主 cc 编译（不是交叉
#      编译器），把 fsroot/ 下的文件打包成 fs.img。这是本课程第一次出现
#      "宿主工具"这个角色：内核不会自己格式化磁盘，磁盘镜像是构建时在
#      宿主上造好的。真实系统里这个角色就是 mkfs.ext4 / newfs。
#
#   3. -I$(LAB_ROOT)：让内核代码和 mkfs 都能 #include "fs_format.h"。
#      磁盘格式定义只有一份，被三个编译目标共用（两个内核 + 宿主工具），
#      这是"同一份格式定义，多个独立实现"最直接的保证方式。

ARCH ?= x86_64
VARIANT ?= solution

LAB_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(LAB_ROOT)/../..)
COMMON_DIR := $(REPO_ROOT)/src/common
ARCH_DIR := $(LAB_ROOT)/$(VARIANT)/$(ARCH)
BUILD_DIR := $(ARCH_DIR)/build

include $(COMMON_DIR)/toolchain.mk

CFLAGS := $(CFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -I$(LAB_ROOT) -g
ifeq ($(ARCH),x86_64)
CFLAGS += -mcmodel=kernel
endif
ASFLAGS := $(ASFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -g
LDFLAGS := $(LDFLAGS_$(ARCH))

COMMON_SRCS := $(COMMON_DIR)/console.c $(COMMON_DIR)/panic.c $(COMMON_DIR)/string.c $(COMMON_DIR)/kalloc.c
ARCH_SRCS := $(ARCH_DIR)/boot.S $(ARCH_DIR)/console_putc.c $(ARCH_DIR)/panic_arch.c $(ARCH_DIR)/kernel_main.c $(ARCH_DIR)/memmap.c $(ARCH_DIR)/pagetable.c $(ARCH_DIR)/trap.c $(ARCH_DIR)/trap_entry.S $(ARCH_DIR)/user_blob.S $(ARCH_DIR)/proc.c $(ARCH_DIR)/swtch.S $(ARCH_DIR)/fs.c
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

# 镜像内容：fsroot/ 下的所有文件。依赖写成 wildcard，改了文件内容会重新
# 生成镜像；但*新增*文件需要 make 重新读一遍 Makefile 才能被 wildcard
# 看到——加文件后如果镜像没更新，`make clean` 一下即可。
# $(sort) 不只是为了整齐——它决定了 inode 号。mkfs 按命令行顺序依次分配
# inode，而 $(wildcard) 返回的是目录项顺序，不同文件系统/不同机器上可能
# 不一样。不排序的话，同一份 fsroot/ 在两台机器上会生成 inode 号不同的
# 镜像，而 tests/expect-*.txt 里写着具体的 inode 号，测试就会随机失败。
# 凡是"构建输出被外部顺序影响"的地方都要显式定序，这是可重复构建的基本要求。
FSROOT_FILES := $(sort $(wildcard $(LAB_ROOT)/fsroot/*))

.PHONY: build clean iso fs.img mkfs check-fs-iface

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

$(FS_IMG): $(MKFS_BIN) $(FSROOT_FILES) | $(OBJ_DIR)
	$(MKFS_BIN) $@ $(FSROOT_FILES)

# ---------------------------------------------------------------------------
# 两个架构之间的一致性检查
# ---------------------------------------------------------------------------
# fs.c / fs.h / blk.h 在两个架构目录下各有一份、且必须逐字节相同（理由见
# fs.c 顶部注释）。刻意的重复需要机械化的守卫，否则"改了一边忘了另一边"
# 只会在另一个架构的测试跑起来时才暴露，而且症状看起来毫不相干。
#
# solution 和 starter 两对都查：starter 里这三个文件是带 TODO 的骨架，
# 同样"两个架构一份答案"（fs.c 顶部说明让学生做完一个架构直接拷过去），
# 所以同样需要守卫。注意是"x86_64 对 riscv64"，不是"starter 对 solution"
# ——后者本来就该不同（一个是 TODO 骨架，一个是实现）。
#
# 用 cmp 而不是 diff：这里只关心"是否相同"，cmp 的退出码就够了，而且
# 不一致时 cmp 会直接指出第一个不同的字节位置。
FS_SHARED_FILES := fs.c fs.h blk.h
FS_SHARED_VARIANTS := solution starter

check-fs-iface:
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
	@echo "check-fs-iface: fs.c / fs.h / blk.h 在 solution 和 starter 下各自两个架构一致"

# ---------------------------------------------------------------------------
# 内核构建（与 Lab7 相同）
# ---------------------------------------------------------------------------
# 用户程序独立构建流水线——见 Lab6 Makefile 顶部注释的三步说明。
USER_PROG_SRC := $(ARCH_DIR)/user_prog.S
USER_PROG_LD := $(ARCH_DIR)/user_prog.ld
USER_PROG_ELF := $(BUILD_DIR)/user_prog.elf
USER_PROG_BIN := $(ARCH_DIR)/user_prog.bin

$(USER_PROG_ELF): $(USER_PROG_SRC) $(USER_PROG_LD) | $(OBJ_DIR)
	$(AS) $(ASFLAGS) -c $(USER_PROG_SRC) -o $(BUILD_DIR)/user_prog.o
	$(LD) -T $(USER_PROG_LD) -o $@ $(BUILD_DIR)/user_prog.o

$(USER_PROG_BIN): $(USER_PROG_ELF)
	$(OBJCOPY) -O binary $< $@

# user_blob.S 的 .incbin "user_prog.bin" 是相对汇编器 -I 搜索路径找的
# ——ASFLAGS 里已经有 -I$(COMMON_DIR)/include，这里再加一条
# -I$(ARCH_DIR)，让 .incbin 能找到刚 objcopy 出来的这份文件。
$(OBJ_DIR)/user_blob.o: $(ARCH_DIR)/user_blob.S $(USER_PROG_BIN) | $(OBJ_DIR)
	$(AS) $(ASFLAGS) -I$(ARCH_DIR) -c $(ARCH_DIR)/user_blob.S -o $@

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
	rm -f $(LAB_ROOT)/solution/x86_64/user_prog.bin $(LAB_ROOT)/solution/riscv64/user_prog.bin
	rm -f $(LAB_ROOT)/starter/x86_64/user_prog.bin $(LAB_ROOT)/starter/riscv64/user_prog.bin
	rm -f $(MKFS_BIN)
