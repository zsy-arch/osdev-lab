# osdev-lab 成品 OS 顶层构建入口。
#
# 这是 lab01~lab09 全部内容累积后的完整系统：物理/虚拟内存管理、中断与
# 系统调用、进程调度（fork/exec/wait）、块设备驱动、只读文件系统、ELF
# 加载器、管道、一个极简用户态 libc，外加六个用户程序（init/sh/ls/cat/
# echo/grep）。x86_64 和 riscv64 两个架构各有一份独立内核实现，用户态
# 代码与文件系统/ELF/管道逻辑在两个架构间共享（fs.c/fs.h/blk.h/exec.c/
# pipe.c/pipe.h 逐字节相同，见 check-shared-iface 目标）。
#
# 本目录是从 labs/lab09-shell-userspace/solution/ 提炼出来的独立产物：
# 内核与用户态源码未做任何逻辑改动，只是去掉了 Lab 框架（starter 骨架、
# VARIANT 切换、每课一份 Makefile）之后的重新打包，外加自带一份
# src/common/ 的副本（见 common/），使这个目录可以脱离整个仓库单独存在、
# 单独构建。
#
# 用法:
#   make build ARCH=x86_64|riscv64   # 编译内核 + 生成磁盘镜像（x86_64 额外生成 os.iso）
#   make run   ARCH=x86_64|riscv64   # 编译并在 QEMU 里跑起来（串口输出到当前终端）
#   make test  ARCH=x86_64|riscv64   # 编译、跑一次、跟 tests/expect-<arch>.txt 逐行比对
#   make test-all                    # 两个架构各跑一次 test
#   make debug ARCH=... [MONITOR=1]  # QEMU 挂起等待 GDB（-s -S），另开一个终端 gdb 连 :1234
#   make clean                       # 清理两个架构的构建产物
#
# 交叉编译器/GRUB 工具找不到时的排错，见下面 CROSS_X86_64/CROSS_RISCV64
# 两个变量的说明，或直接把报错信息拿去搜。

SHELL := /bin/bash
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ARCH ?= x86_64
TIMEOUT ?= 15

ifeq ($(filter $(ARCH),x86_64 riscv64),)
$(error ARCH 必须是 x86_64 或 riscv64，收到的是: $(ARCH))
endif

# ---------------------------------------------------------------------------
# 工具链探测
# ---------------------------------------------------------------------------
# 跟 labs/ 下每个 Lab 共享的 src/common/toolchain.mk 是同一套探测逻辑（这
# 里没有直接 include 那个文件，而是内联了一份——本目录要能脱离整个仓库
# 单独存在，不应该在构建期回头依赖 ../src/common）。改动任何一边时另一边
# 也要检查是否需要同步。
UNAME_S := $(shell uname -s)

# -ffile-prefix-map：把 __FILE__ 展开出来的绝对路径改写成相对于本目录的
# 路径，避免 panic() 打印出来的文件名带上构建机器的具体目录结构。
FILE_PREFIX_MAP := -ffile-prefix-map=$(ROOT_DIR)/=

ifeq ($(ARCH),x86_64)

ifdef CROSS_X86_64
  CC  := $(CROSS_X86_64)gcc
  AS  := $(CROSS_X86_64)gcc
  LD  := $(CROSS_X86_64)ld
  GDB := $(CROSS_X86_64)gdb
else ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
  # -elf- 交叉工具链：macOS 上这是唯一选项，Linux 上如果装了也优先用它。
  CC  := x86_64-elf-gcc
  AS  := x86_64-elf-gcc
  LD  := x86_64-elf-ld
  GDB := $(if $(shell command -v x86_64-elf-gdb 2>/dev/null),x86_64-elf-gdb,gdb)
else ifeq ($(UNAME_S),Darwin)
  $(error [x86_64] macOS 上没找到 x86_64-elf-gcc，系统 gcc 是 Apple Clang，其 as/ld 不支持本项目用的 GNU 汇编语法与链接脚本。安装: brew install x86_64-elf-gcc i686-elf-grub xorriso mtools)
else
  # Linux/Docker：系统 gcc/clang 背后就是真正的 GNU binutils，配合
  # -ffreestanding 可以直接用。用 -dumpmachine 确认目标是 Linux，避免在
  # 其它类 Unix 系统上误用。
  SYSTEM_CC := $(if $(shell command -v gcc 2>/dev/null),gcc,clang)
  SYSTEM_CC_TARGET := $(shell $(SYSTEM_CC) -dumpmachine 2>/dev/null)
  ifeq ($(findstring linux,$(SYSTEM_CC_TARGET)),linux)
    CC  := $(SYSTEM_CC)
    AS  := $(SYSTEM_CC)
    LD  := ld
    GDB := $(if $(shell command -v gdb-multiarch 2>/dev/null),gdb-multiarch,gdb)
  else
    $(error [x86_64] 没找到 x86_64-elf-gcc，且系统编译器 $(SYSTEM_CC) 的目标 "$(SYSTEM_CC_TARGET)" 不是 Linux。安装 x86_64-elf-gcc，或指定 CROSS_X86_64=<前缀>)
  endif
endif

CFLAGS_x86_64  := -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -Wall -Wextra $(FILE_PREFIX_MAP)
ASFLAGS_x86_64 := -m64 -ffreestanding
LDFLAGS_x86_64 := -nostdlib -static

# grub-mkrescue 的实际二进制名在不同平台不一样。i686-elf-grub-mkrescue 排
# 在最前面：Homebrew 的 x86_64-elf-grub 只打包了 x86_64-efi 平台目标，没有
# i386-pc（legacy BIOS）需要的 boot_hybrid.img，用它做出的 ISO QEMU 默认的
# SeaBIOS 读不出来。装了 i686-elf-grub 就必须优先用它。
GRUB_MKRESCUE := $(firstword $(foreach c,i686-elf-grub-mkrescue grub-mkrescue x86_64-elf-grub-mkrescue i386-elf-grub-mkrescue,$(if $(shell command -v $(c) 2>/dev/null),$(c))))

endif # ARCH == x86_64

ifeq ($(ARCH),riscv64)

ifdef CROSS_RISCV64
  CC  := $(CROSS_RISCV64)gcc
  AS  := $(CROSS_RISCV64)gcc
  LD  := $(CROSS_RISCV64)ld
  GDB := $(CROSS_RISCV64)gdb
else
  # riscv64 没有"本机工具链凑合用"这个选项，必须是某个交叉工具链。
  RISCV_PREFIX := $(firstword $(foreach p,riscv64-elf- riscv64-unknown-elf- riscv64-linux-gnu-,$(if $(shell command -v $(p)gcc 2>/dev/null),$(p))))
  ifeq ($(RISCV_PREFIX),)
    $(error [riscv64] 没找到 riscv64-elf-gcc / riscv64-unknown-elf-gcc / riscv64-linux-gnu-gcc 任何一个。Linux: apt install gcc-riscv64-linux-gnu | macOS: brew install riscv64-elf-gcc | 或指定 CROSS_RISCV64=<前缀>)
  endif
  CC  := $(RISCV_PREFIX)gcc
  AS  := $(RISCV_PREFIX)gcc
  LD  := $(RISCV_PREFIX)ld
  GDB := $(if $(shell command -v $(RISCV_PREFIX)gdb 2>/dev/null),$(RISCV_PREFIX)gdb,$(if $(shell command -v gdb-multiarch 2>/dev/null),gdb-multiarch,gdb))
endif

CFLAGS_riscv64  := -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -fno-pic -Wall -Wextra $(FILE_PREFIX_MAP)
ASFLAGS_riscv64 := -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding
LDFLAGS_riscv64 := -nostdlib -static

endif # ARCH == riscv64

# ---------------------------------------------------------------------------
# 内核构建
# ---------------------------------------------------------------------------
COMMON_DIR := $(ROOT_DIR)/common
ARCH_DIR := $(ROOT_DIR)/$(ARCH)
BUILD_DIR := $(ARCH_DIR)/build

CFLAGS := $(CFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -I$(ROOT_DIR) -g
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

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S | $(OBJ_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)

ISO_DIR := $(BUILD_DIR)/isodir
OS_ISO := $(BUILD_DIR)/os.iso

.PHONY: iso
iso: $(OS_ISO)

$(OS_ISO): $(KERNEL_ELF) $(ARCH_DIR)/grub.cfg
ifeq ($(GRUB_MKRESCUE),)
	$(error 没找到可用的 grub-mkrescue。Linux: apt install grub-pc-bin grub-common xorriso mtools | macOS: brew install i686-elf-grub xorriso mtools)
endif
	rm -rf $(ISO_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp $(ARCH_DIR)/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

# ---------------------------------------------------------------------------
# 用户程序构建流水线
# ---------------------------------------------------------------------------
USER_DIR := $(ROOT_DIR)/user
USER_BUILD_DIR := $(BUILD_DIR)/user
USER_OBJ_DIR := $(USER_BUILD_DIR)/obj

# 用户态编译选项跟内核选项是两套完全独立的 flags：x86_64 内核用
# -mcmodel=kernel（假设代码在高 2GB 虚拟地址），用户程序链接到 0x400000，
# 必须去掉这个假设；riscv64 内核启用 D 扩展（-mabi=lp64d 配 rv64gc），用户
# 程序改用 -march=rv64imac -mabi=lp64（没有一行浮点运算，不需要 D 扩展）。
USER_CFLAGS_x86_64  := -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -Wall -Wextra $(FILE_PREFIX_MAP)
USER_ASFLAGS_x86_64 := -m64 -ffreestanding
USER_CFLAGS_riscv64  := -march=rv64imac -mabi=lp64 -ffreestanding -fno-stack-protector -fno-pic -Wall -Wextra $(FILE_PREFIX_MAP)
USER_ASFLAGS_riscv64 := -march=rv64imac -mabi=lp64 -ffreestanding

USER_CFLAGS := $(USER_CFLAGS_$(ARCH)) -I$(USER_DIR) -I$(ROOT_DIR) -g
USER_ASFLAGS := $(USER_ASFLAGS_$(ARCH)) -g
USER_LDFLAGS := -nostdlib -static -z max-page-size=4096

USER_LD := $(USER_DIR)/user.ld
USYS_SRC := $(USER_DIR)/usys_$(ARCH).S
CRT0_SRC := $(USER_DIR)/crt0_$(ARCH).S

# 6 个用户程序，每个由"自己的 .c + ulib.c + usys_<arch>.S + crt0_<arch>.S"
# 四个目标文件链接而成，用 make 函数生成规则而不是手写 6 遍。
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

$(USER_OBJ_DIR)/usys.o: $(USYS_SRC) $(ROOT_DIR)/syscall.h | $(USER_OBJ_DIR)
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
# 用 HOSTCC（宿主编译器）而不是 $(CC)（交叉编译器）：mkfs 要在开发机上跑，
# 编译成 riscv64 或裸机 x86_64 的二进制是执行不了的。
HOSTCC ?= cc
HOSTCFLAGS ?= -std=c11 -Wall -Wextra -O1 -I$(ROOT_DIR)

MKFS_SRC := $(ROOT_DIR)/mkfs/mkfs.c
MKFS_BIN := $(ROOT_DIR)/mkfs/mkfs

.PHONY: mkfs
mkfs: $(MKFS_BIN)

$(MKFS_BIN): $(MKFS_SRC) $(ROOT_DIR)/fs_format.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(MKFS_SRC)

FS_IMG := $(BUILD_DIR)/fs.img

# 镜像内容：fsroot/ 下的所有文件，加上刚构建出的 6 个用户程序 ELF。
# $(sort) 决定 inode 号的稳定性——mkfs 按命令行顺序依次分配 inode，不排序
# 的话同一份 fsroot/ 在不同机器上可能生成不同 inode 号的镜像，而
# tests/expect-*.txt 里写死了具体 inode 号，测试就会随机失败。
FSROOT_FILES := $(sort $(wildcard $(ROOT_DIR)/fsroot/*))
FS_IMG_INPUTS := $(FSROOT_FILES) $(USER_PROG_BINS)

.PHONY: fs.img
fs.img: $(FS_IMG)

$(FS_IMG): $(MKFS_BIN) $(FS_IMG_INPUTS) | $(OBJ_DIR)
	$(MKFS_BIN) $@ $(FS_IMG_INPUTS)

# ---------------------------------------------------------------------------
# 顶层目标：build / run / debug / test / clean
# ---------------------------------------------------------------------------
.PHONY: build
build: $(KERNEL_ELF) $(FS_IMG)
ifeq ($(ARCH),x86_64)
build: iso
endif

.PHONY: run
run: build
	bash $(ROOT_DIR)/scripts/run-qemu.sh ARCH=$(ARCH)

.PHONY: debug
debug: build
	bash $(ROOT_DIR)/scripts/run-qemu.sh ARCH=$(ARCH) DEBUG=1 MONITOR=$(MONITOR)

.PHONY: test
test: build
	bash $(ROOT_DIR)/scripts/test.sh ARCH=$(ARCH)

.PHONY: test-all
test-all:
	$(MAKE) test ARCH=x86_64
	$(MAKE) test ARCH=riscv64

.PHONY: clean
clean:
	rm -rf $(ROOT_DIR)/x86_64/build $(ROOT_DIR)/riscv64/build
	rm -f $(MKFS_BIN)

.PHONY: help
help:
	@echo "用法: make <target> ARCH=x86_64|riscv64"
	@echo ""
	@echo "  build      编译内核 + 生成磁盘镜像（x86_64 额外生成 os.iso）"
	@echo "  run        编译并在 QEMU 里跑起来，串口输出到当前终端"
	@echo "  debug      编译并在 QEMU 里挂起等待 GDB 连接 (:1234)"
	@echo "  test       编译、跑一次、跟 tests/expect-<arch>.txt 比对"
	@echo "  test-all   两个架构各跑一次 test"
	@echo "  clean      清理两个架构的构建产物"
	@echo ""
	@echo "示例: make run ARCH=riscv64"
