# Lab5: 中断、异常、trap、时钟——从 Lab4 的最窄单向量扩展成能处理外部
# 中断（定时器）的框架，让内核第一次能"持续运行"而不是跑完一次性验证
# 就 panic。
#
# 用法:
#   make build ARCH=x86_64|riscv64 [VARIANT=solution|starter]
#   make clean
#
# 结构和 labs/lab04-virtual-memory/Makefile 完全一样，见那边的注释；
# 这里多了两处：
#   - x86_64 的 ARCH_SRCS 新增 pit.c（PIC 重映射 + PIT 驱动，trap.c 只
#     管 IDT 本身的形状，跟设备打交道的部分单独放一个文件，理由跟
#     memmap.c/pagetable.c 分文件是同一个"一个文件一个关注点"的原则）。
#   - riscv64 的 ARCH_SRCS 新增 sbi.c（SBI ecall 的通用包装 +
#     sbi_set_timer()，S-mode 没法直接编程 CLINT，必须通过 SBI 找
#     M-mode/OpenSBI 代劳，见 sbi.c 顶部注释）。

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
ARCH_SRCS := $(ARCH_DIR)/boot.S $(ARCH_DIR)/console_putc.c $(ARCH_DIR)/panic_arch.c $(ARCH_DIR)/kernel_main.c $(ARCH_DIR)/memmap.c $(ARCH_DIR)/pagetable.c $(ARCH_DIR)/trap.c $(ARCH_DIR)/trap_entry.S
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
