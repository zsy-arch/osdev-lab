# src/common/toolchain.mk
#
# 每个 Lab 的 Makefile 在 build 相关规则之前 include 这个文件，拿到按 ARCH
# 探测好的 CC/AS/LD/OBJCOPY/GDB 等变量，不用在每个 Lab 里重复写探测逻辑。
#
# 用法（Lab 的 Makefile 里）:
#   ARCH ?= x86_64
#   include ../../src/common/toolchain.mk
#   $(CC) $(CFLAGS_$(ARCH)) -c foo.c -o foo.o
#
# 显式覆盖（探测选错时用）:
#   make build ARCH=x86_64 CROSS_X86_64=x86_64-elf-
#   make build ARCH=riscv64 CROSS_RISCV64=riscv64-linux-gnu-
#
# 探测顺序见 docs/environment.md 的"环境变量与工具链前缀"一节，这里的实现要跟那份
# 文档保持一致——如果这里改探测顺序，记得同步改文档。

ifndef ARCH
$(error toolchain.mk 需要先设置 ARCH=x86_64 或 ARCH=riscv64)
endif
ifndef REPO_ROOT
$(error toolchain.mk 需要先设置 REPO_ROOT（仓库根目录的绝对路径），用于 -ffile-prefix-map；参照现有 labs/*/Makefile 里 REPO_ROOT 的定义方式)
endif
ifeq ($(filter $(ARCH),x86_64 riscv64),)
$(error ARCH 必须是 x86_64 或 riscv64，收到的是: $(ARCH))
endif

UNAME_S := $(shell uname -s)

# -ffile-prefix-map=<repo根>/= ：把 __FILE__ 展开出来的绝对路径（编译器
# 默认按传给它的路径原样展开，我们的 Makefile 传的是绝对路径）改写成相对
# 于仓库根的路径。从 Lab2 开始，panic() 宏（src/common/include/panic.h）
# 会把 __FILE__ 打到串口输出给自动化测试比对，如果不做这个映射，比对字符串
# 就得写死某一台机器上仓库的具体 checkout 路径，换一台机器、换个目录名
# 克隆仓库，字符串就对不上——这不是假设出来的问题，是实际写 Lab2 测试时
# 触发的：__FILE__ 展开成了 /Users/xxx/.../kernel_main.c 这种本机专属路径。
#
# 放在这里（两个 ARCH 分支之外），不是随手选的位置：这个变量跟 ARCH 无关，
# 两个架构都要用。第一版实现把它放进了 `ifeq ($(ARCH),x86_64)` 分支内部，
# 结果 ARCH=riscv64 时这个分支整体不会被求值，$(FILE_PREFIX_MAP) 展开成
# 空字符串——riscv64 的编译命令里完全没有这个 flag，__FILE__ 照样是本机
# 绝对路径，实测跑 riscv64 测试时输出对不上、x86_64 却是对的，才揪出这个
# 分支作用域问题。
FILE_PREFIX_MAP := -ffile-prefix-map=$(REPO_ROOT)/=

# ---------------------------------------------------------------------------
# x86_64
# ---------------------------------------------------------------------------
ifeq ($(ARCH),x86_64)

ifdef CROSS_X86_64
  # 用户显式指定前缀，直接信任，不做探测。
  CC  := $(CROSS_X86_64)gcc
  AS  := $(CROSS_X86_64)gcc
  LD  := $(CROSS_X86_64)ld
  OBJCOPY := $(CROSS_X86_64)objcopy
  GDB := $(CROSS_X86_64)gdb
else ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
  # 优先用 -elf- 交叉工具链：macOS 上这是唯一选项，Linux 上如果装了也优先用它，
  # 保证两个平台走同一套二进制、行为一致。
  CC  := x86_64-elf-gcc
  AS  := x86_64-elf-gcc
  LD  := x86_64-elf-ld
  OBJCOPY := x86_64-elf-objcopy
  GDB := $(if $(shell command -v x86_64-elf-gdb 2>/dev/null),x86_64-elf-gdb,gdb)
else ifeq ($(UNAME_S),Darwin)
  # macOS 且没装 x86_64-elf-gcc：没有平替，系统 gcc 是 Apple Clang，其 as/ld
  # 不认识本课程用的 GNU AT&T 汇编语法和 -T 链接脚本，直接报错而不是静默用错工具链。
  $(error [x86_64] macOS 上没找到 x86_64-elf-gcc，且没有平替可用 (系统 gcc 是 Apple Clang，其 as/ld 不支持本课程的 GNU 汇编语法与链接脚本)。安装: brew install x86_64-elf-gcc x86_64-elf-grub xorriso mtools)
else
  # Linux/Docker：系统 gcc/clang 背后的 as/ld 就是真正的 GNU binutils，
  # 配合 -ffreestanding 可以直接用，不强制要求装 -elf- 交叉工具链。
  # 用 -dumpmachine 确认一下，避免在非 Linux 的类 Unix 系统上误用（比如某些 BSD）。
  SYSTEM_CC := $(if $(shell command -v gcc 2>/dev/null),gcc,clang)
  SYSTEM_CC_TARGET := $(shell $(SYSTEM_CC) -dumpmachine 2>/dev/null)
  ifeq ($(findstring linux,$(SYSTEM_CC_TARGET)),linux)
    CC  := $(SYSTEM_CC)
    AS  := $(SYSTEM_CC)
    LD  := ld
    OBJCOPY := objcopy
    GDB := $(if $(shell command -v gdb-multiarch 2>/dev/null),gdb-multiarch,gdb)
  else
    $(error [x86_64] 没找到 x86_64-elf-gcc，且系统编译器 $(SYSTEM_CC) 的目标 "$(SYSTEM_CC_TARGET)" 不是 Linux (可能是 Apple Clang 或其它)，其 as/ld 不一定是 GNU binutils。安装 x86_64-elf-gcc，或显式指定 CROSS_X86_64=<前缀>)
  endif
endif

CFLAGS_x86_64  := -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -Wall -Wextra $(FILE_PREFIX_MAP)
ASFLAGS_x86_64 := -m64 -ffreestanding
LDFLAGS_x86_64 := -nostdlib -static

# grub-mkrescue 的实际二进制名在不同平台不一样，见 docs/environment.md。
#
# 注意顺序：i686-elf-grub-mkrescue 排在 x86_64-elf-grub-mkrescue 前面，
# 不是随意的。Homebrew 的 x86_64-elf-grub 这个 formula 只编译了 GRUB 的
# x86_64-efi 平台目标，完全没有打包 i386-pc（legacy BIOS）平台需要的
# eltorito.img / boot_hybrid.img。用它的 grub-mkrescue 做出来的 ISO，
# El Torito 引导目录里只有一条 UEFI 记录，QEMU 默认的 SeaBIOS 完全读不出来
# （报 "Boot failed: Could not read from CDROM (code 0009)"，然后掉进
# iPXE 网络引导，串口上什么都不会打印，很容易误判成内核代码的问题）。
# 用 `brew install i686-elf-grub` 装的 grub-mkrescue 才会正确生成带
# BIOS 引导记录的 ISO。如果系统里同时有这两个二进制，必须优先选 i686 版本。
GRUB_MKRESCUE := $(firstword $(foreach c,i686-elf-grub-mkrescue grub-mkrescue x86_64-elf-grub-mkrescue i386-elf-grub-mkrescue,$(if $(shell command -v $(c) 2>/dev/null),$(c))))
ifeq ($(GRUB_MKRESCUE),)
  # 只在真正需要制作 ISO 的规则里才会用到 GRUB_MKRESCUE，这里不报错，
  # 留给调用它的 Lab 规则自己在用到时检查是否为空。
endif

endif # ARCH == x86_64

# ---------------------------------------------------------------------------
# riscv64
# ---------------------------------------------------------------------------
ifeq ($(ARCH),riscv64)

ifdef CROSS_RISCV64
  CC  := $(CROSS_RISCV64)gcc
  AS  := $(CROSS_RISCV64)gcc
  LD  := $(CROSS_RISCV64)ld
  OBJCOPY := $(CROSS_RISCV64)objcopy
  GDB := $(CROSS_RISCV64)gdb
else
  # riscv64 没有"本机工具链可以凑合用"这个选项（无论 Linux 还是 macOS，
  # 开发机都不是 riscv64，系统 as/ld 从来不认识 riscv64 指令），
  # 必须是某个交叉工具链，按下面顺序探测，全部找不到就报错。
  RISCV_PREFIX := $(firstword $(foreach p,riscv64-elf- riscv64-unknown-elf- riscv64-linux-gnu-,$(if $(shell command -v $(p)gcc 2>/dev/null),$(p))))
  ifeq ($(RISCV_PREFIX),)
    $(error [riscv64] 没找到 riscv64-elf-gcc / riscv64-unknown-elf-gcc / riscv64-linux-gnu-gcc 任何一个。Linux: apt install gcc-riscv64-linux-gnu | macOS: brew install riscv64-elf-gcc | 或显式指定 CROSS_RISCV64=<前缀>)
  endif
  CC  := $(RISCV_PREFIX)gcc
  AS  := $(RISCV_PREFIX)gcc
  LD  := $(RISCV_PREFIX)ld
  OBJCOPY := $(RISCV_PREFIX)objcopy
  GDB := $(if $(shell command -v $(RISCV_PREFIX)gdb 2>/dev/null),$(RISCV_PREFIX)gdb,$(if $(shell command -v gdb-multiarch 2>/dev/null),gdb-multiarch,gdb))
endif

CFLAGS_riscv64  := -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -fno-pic -Wall -Wextra $(FILE_PREFIX_MAP)
ASFLAGS_riscv64 := -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding
LDFLAGS_riscv64 := -nostdlib -static

endif # ARCH == riscv64

# 每个 Lab 的 Makefile 用 $(CFLAGS_$(ARCH))/$(ASFLAGS_$(ARCH))/$(LDFLAGS_$(ARCH))
# 取到当前架构对应的那一份，避免两个架构的规则互相踩到对方的 flags。
