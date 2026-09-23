# Lab6: 系统调用与用户态——从 Lab5 的"内核态 trap 框架"扩展出第一条
# 真正的特权级切换路径（x86_64 SYSCALL/SYSRET，riscv64 ecall/sret）。
#
# 用法:
#   make build ARCH=x86_64|riscv64 [VARIANT=solution|starter]
#   make clean
#
# 结构和 labs/lab05-interrupt-trap/Makefile 基本一样，见那边的注释；
# 本 Lab 独有的部分是"嵌入式用户程序"这一整条额外构建流水线：
#   1. user_prog.S 用它自己的链接脚本（user_prog.ld）独立编译/链接成
#      一个 ELF（跟内核完全不同的 ENTRY、地址空间设定，两者不共享
#      任何符号，是两个独立的构建产物）。
#   2. objcopy -O binary 把这个 ELF 转成扁平二进制（丢弃 ELF 头/
#      program header，只留 .text/.rodata 合并后的原始字节）。
#   3. user_blob.S 用 .incbin 把这份扁平二进制整段吸进内核镜像，
#      __user_prog_start/__user_prog_end 两个符号夹住它，kernel_main.c
#      运行时用这两个符号算长度、拷贝到新分配的用户页。
#
# 第 3 步依赖第 2 步的产物先存在于 ARCH_DIR 里（.incbin 的路径是相对
# 汇编器工作目录/-I 搜索路径的，不是相对 Makefile），所以
# user_blob.S 编译成 .o 这条规则必须显式加一条对 user_prog.bin 的
# 依赖——否则 make 的并行构建（-jN）不保证 user_prog.bin 先生成，
# .incbin 会在文件还不存在时报错，或者更隐蔽地吸进上一次构建留下的
# 陈旧内容。

ARCH ?= x86_64
VARIANT ?= solution

LAB_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(LAB_ROOT)/../..)
COMMON_DIR := $(REPO_ROOT)/src/common
ARCH_DIR := $(LAB_ROOT)/$(VARIANT)/$(ARCH)
BUILD_DIR := $(ARCH_DIR)/build

include $(COMMON_DIR)/toolchain.mk

CFLAGS := $(CFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -g
ifeq ($(ARCH),x86_64)
CFLAGS += -mcmodel=kernel
endif
ASFLAGS := $(ASFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -g
LDFLAGS := $(LDFLAGS_$(ARCH))

COMMON_SRCS := $(COMMON_DIR)/console.c $(COMMON_DIR)/panic.c $(COMMON_DIR)/string.c $(COMMON_DIR)/kalloc.c
ARCH_SRCS := $(ARCH_DIR)/boot.S $(ARCH_DIR)/console_putc.c $(ARCH_DIR)/panic_arch.c $(ARCH_DIR)/kernel_main.c $(ARCH_DIR)/memmap.c $(ARCH_DIR)/pagetable.c $(ARCH_DIR)/trap.c $(ARCH_DIR)/trap_entry.S $(ARCH_DIR)/user_blob.S
ifeq ($(ARCH),x86_64)
ARCH_SRCS += $(ARCH_DIR)/pit.c
else
ARCH_SRCS += $(ARCH_DIR)/sbi.c
endif

ALL_SRCS := $(COMMON_SRCS) $(ARCH_SRCS)

OBJ_DIR := $(BUILD_DIR)/obj
OBJS := $(addprefix $(OBJ_DIR)/,$(notdir $(ALL_SRCS:.S=.o)))
OBJS := $(OBJS:.c=.o)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
LINKER_SCRIPT := $(ARCH_DIR)/linker.ld

vpath %.c $(COMMON_DIR) $(ARCH_DIR)
vpath %.S $(ARCH_DIR)

.PHONY: build clean iso

build: $(KERNEL_ELF)
ifeq ($(ARCH),x86_64)
build: iso
endif

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# 用户程序独立构建流水线——见文件顶部注释的三步说明。
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
# -I$(ARCH_DIR)，让 .incbin 能找到刚 objcopy 出来的这份文件，不需要
# 在 user_blob.S 里写绝对路径（绝对路径会让这份汇编源码在别的机器/
# 别的 checkout 路径下失效，教学代码应该保持可移植）。
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
