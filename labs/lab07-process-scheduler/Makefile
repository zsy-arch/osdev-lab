# Lab7: 进程与调度——从 Lab6"单个用户程序、内核态忙等"扩展出真正的
# 多进程轮转调度（PCB、swtch() 上下文切换、fork/exec/wait/exit）。
#
# 用法:
#   make build ARCH=x86_64|riscv64 [VARIANT=solution|starter]
#   make clean
#
# 结构和 labs/lab06-syscall-user/Makefile 基本一样，见那边的注释；
# 本 Lab 独有的部分只是 ARCH_SRCS 多了两个文件：
#   - proc.c：进程表 + 调度器 + fork/exec/wait/exit 的具体实现。
#   - swtch.S：swtch()/trap_return——内核执行流之间的上下文切换,
#     以及"resume 一个进程"统一入口。
# 嵌入式用户程序那条构建流水线（user_prog.S -> user_prog.elf ->
# user_prog.bin -> user_blob.S 的 .incbin）跟 Lab6 完全一样,不重复
# 展开,见 Lab6 Makefile 顶部注释。

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
ARCH_SRCS := $(ARCH_DIR)/boot.S $(ARCH_DIR)/console_putc.c $(ARCH_DIR)/panic_arch.c $(ARCH_DIR)/kernel_main.c $(ARCH_DIR)/memmap.c $(ARCH_DIR)/pagetable.c $(ARCH_DIR)/trap.c $(ARCH_DIR)/trap_entry.S $(ARCH_DIR)/user_blob.S $(ARCH_DIR)/proc.c $(ARCH_DIR)/swtch.S
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
