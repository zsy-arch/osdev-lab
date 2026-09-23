# Lab1: Boot 与裸机输出
#
# 用法:
#   make build ARCH=x86_64|riscv64 [VARIANT=solution|starter]
#   make clean
#
# 顶层 Makefile（make run/test/debug）会 cd 到这个目录后调用 `make build`，
# 见 ../../Makefile 里的 run/test/debug 目标。

ARCH ?= x86_64
VARIANT ?= solution

LAB_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(LAB_ROOT)/../..)
COMMON_DIR := $(REPO_ROOT)/src/common
ARCH_DIR := $(LAB_ROOT)/$(VARIANT)/$(ARCH)
BUILD_DIR := $(ARCH_DIR)/build

include $(COMMON_DIR)/toolchain.mk

CFLAGS := $(CFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -g
ASFLAGS := $(ASFLAGS_$(ARCH)) -I$(COMMON_DIR)/include -g
LDFLAGS := $(LDFLAGS_$(ARCH))

# 共享的架构无关源文件：console.c（kprintf 等）、panic.c（kernel_panic
# 的架构无关部分）、string.c。它们都只依赖 console_putc()/panic_halt()
# 这两个每个架构自己实现的钩子函数，见 src/common/include/console.h、
# src/common/include/panic.h 里的说明。
COMMON_SRCS := $(COMMON_DIR)/console.c $(COMMON_DIR)/panic.c $(COMMON_DIR)/string.c

# 架构专属源文件：boot.S（入口与启动阶段汇编）、console_putc.c、
# panic_arch.c（panic_halt 的架构实现）、kernel_main.c。
ARCH_SRCS := $(ARCH_DIR)/boot.S $(ARCH_DIR)/console_putc.c $(ARCH_DIR)/panic_arch.c $(ARCH_DIR)/kernel_main.c

ALL_SRCS := $(COMMON_SRCS) $(ARCH_SRCS)

# 目标文件统一放进 build/obj/，用源文件的绝对路径做 hash 前缀不现实，
# 直接按 basename 放（common 和 arch 目录下的文件名互不重名，够用）。
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

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S | $(OBJ_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)

# --- x86_64 专属：把 kernel.elf 打进一个真的能被 SeaBIOS 引导的 GRUB ISO ---
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
