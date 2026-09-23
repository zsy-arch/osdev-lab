#!/usr/bin/env bash
# 检查本机（或容器内）工具链是否齐备。
# 不会自动安装任何东西，只报告状态 + 给出安装建议，安装与否由你决定。
set -u

PASS=0
FAIL=0
MISSING_OPTIONAL=0

# 判断是否在 Docker 容器 / devcontainer 内，仅用于调整提示文案
IN_CONTAINER=0
if [ -f /.dockerenv ]; then
    IN_CONTAINER=1
fi

OS_NAME="$(uname -s)"

color() {
    # $1: color code, $2: text
    if [ -t 1 ]; then
        printf '\033[%sm%s\033[0m' "$1" "$2"
    else
        printf '%s' "$2"
    fi
}

ok() {
    printf '[%s] %-22s -> %s\n' "$(color 32 OK)" "$1" "$2"
    PASS=$((PASS + 1))
}

miss_required() {
    printf '[%s] %-22s -> %s\n' "$(color 31 MISS)" "$1" "not found -- $2"
    FAIL=$((FAIL + 1))
}

miss_optional() {
    printf '[%s] %-22s -> %s\n' "$(color 33 SKIP)" "$1" "not found (optional) -- $2"
    MISSING_OPTIONAL=$((MISSING_OPTIONAL + 1))
}

check_required() {
    # $1: binary name, $2: version flag, $3: hint on failure (optional)
    local bin="$1" flag="${2:---version}" hint="${3:-}"
    if command -v "$bin" >/dev/null 2>&1; then
        local ver
        ver="$("$bin" "$flag" 2>&1 | head -1)"
        ok "$bin" "$ver"
    else
        miss_required "$bin" "$hint"
    fi
}

check_optional() {
    local bin="$1" flag="${2:---version}" hint="${3:-}"
    if command -v "$bin" >/dev/null 2>&1; then
        local ver
        ver="$("$bin" "$flag" 2>&1 | head -1)"
        ok "$bin" "$ver"
    else
        miss_optional "$bin" "$hint"
    fi
}

echo "=== osdev-lab 环境检查 (OS: $OS_NAME, container: $IN_CONTAINER) ==="
echo

echo "--- 核心工具 ---"
check_required make "--version" "Linux: apt install make | macOS: xcode-select --install"
check_required git "--version" "Linux: apt install git | macOS: xcode-select --install"

echo
echo "--- QEMU ---"
check_required qemu-system-x86_64 "--version" "见 docs/environment.md：Linux apt install qemu-system-x86；macOS brew install qemu"
check_required qemu-system-riscv64 "--version" "见 docs/environment.md：Linux apt install qemu-system-misc；macOS brew install qemu"

echo
echo "--- x86_64 编译工具链 ---"
if command -v x86_64-elf-gcc >/dev/null 2>&1; then
    check_required x86_64-elf-gcc "--version"
    check_required x86_64-elf-ld "--version" "随 x86_64-elf-gcc 一起装的 x86_64-elf-binutils 应该已提供，检查 brew list x86_64-elf-binutils"
elif [ "$OS_NAME" = "Darwin" ]; then
    # macOS 上系统 gcc/ld 是 Apple Clang/ld，不认识本课程用的 GNU AT&T 汇编语法
    # 和 -T 链接脚本参数，不能当替代品，必须装交叉工具链。
    miss_required "x86_64-elf-gcc" "macOS 上没有平替：系统 gcc 实际是 Apple Clang，其 as/ld 不支持本课程的 GNU 汇编语法与链接脚本。brew install x86_64-elf-gcc x86_64-elf-grub xorriso mtools"
elif command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; then
    # Linux/Docker 上系统 as/ld 就是 GNU binutils，系统编译器配合 -ffreestanding 可直接用。
    printf '[%s] %-22s -> %s\n' "$(color 32 OK)" "x86_64 compiler" "未装 x86_64-elf-gcc，回退使用系统 gcc/clang + -ffreestanding（Linux 下可行，系统 as/ld 是真正的 GNU binutils）"
    PASS=$((PASS + 1))
else
    miss_required "x86_64-elf-gcc / gcc / clang" "至少需要一个可用编译器，见 docs/environment.md"
fi

echo
echo "--- riscv64 交叉工具链 (裸机开发必需，无平替) ---"
RISCV_GCC=""
for candidate in riscv64-elf-gcc riscv64-unknown-elf-gcc riscv64-linux-gnu-gcc; do
    if command -v "$candidate" >/dev/null 2>&1; then
        RISCV_GCC="$candidate"
        break
    fi
done
if [ -n "$RISCV_GCC" ]; then
    check_required "$RISCV_GCC" "--version"
else
    miss_required "riscv64-*-gcc" "Linux: apt install gcc-riscv64-linux-gnu | macOS: brew install riscv64-elf-gcc"
fi

echo
echo "--- ISO/引导工具 (x86_64 Multiboot2 需要) ---"
GRUB_MKRESCUE=""
# i686-elf-grub-mkrescue 排最前面：macOS 上 x86_64-elf-grub 这个 formula 只打包了
# GRUB 的 x86_64-efi 平台目标，没有 i386-pc（legacy BIOS）需要的引导映像，用它做出来的
# ISO 在 QEMU 默认的 SeaBIOS 下读不出来（表现是串口完全没输出，容易误判成内核代码的
# 问题）。i686-elf-grub 这个 formula 才有完整的 i386-pc 平台目标。见 docs/environment.md。
for candidate in i686-elf-grub-mkrescue grub-mkrescue x86_64-elf-grub-mkrescue i386-elf-grub-mkrescue; do
    if command -v "$candidate" >/dev/null 2>&1; then GRUB_MKRESCUE="$candidate"; break; fi
done
if [ -n "$GRUB_MKRESCUE" ]; then
    check_required "$GRUB_MKRESCUE" "--version"
else
    miss_required "grub-mkrescue" "Linux: apt install grub-pc-bin grub-common | macOS: brew install i686-elf-grub (装完后命令叫 i686-elf-grub-mkrescue；不要用 x86_64-elf-grub，它做出来的 ISO 在 SeaBIOS 下启动不了)"
fi
check_required xorriso "--version" "Linux: apt install xorriso | macOS: brew install xorriso"
check_optional mtools "--version" "grub-mkrescue 的依赖之一，通常随上面两项一起装 (brew install mtools)"

echo
echo "--- 自动化测试超时工具 (make test 需要) ---"
# scripts/run-qemu.sh 里 `make test` 用 timeout/gtimeout 给 QEMU 施加超时；QEMU 用
# -serial stdio 起来后，如果内核没打印出期望的字符串就会一直挂着不退出，没有这层
# 超时包装的话 `make test` 会真的卡死（不是变慢，是永远不返回），很容易被误判成
# 内核代码有 bug。Linux 的 coreutils 默认自带 timeout；macOS 默认两个都没有。
if command -v timeout >/dev/null 2>&1; then
    check_required timeout "--version"
elif command -v gtimeout >/dev/null 2>&1; then
    check_required gtimeout "--version"
else
    miss_required "timeout / gtimeout" "Linux: 一般已随 coreutils 自带，缺失少见 | macOS: brew install coreutils (装完后命令叫 gtimeout)"
fi

echo
echo "--- 调试工具 ---"
if command -v gdb-multiarch >/dev/null 2>&1; then
    check_required gdb-multiarch "--version"
elif command -v gdb >/dev/null 2>&1; then
    printf '[%s] %-22s -> %s\n' "$(color 33 INFO)" "debugger" "未找到 gdb-multiarch，将使用系统 gdb（多数发行版的 gdb 已内置多架构支持，够用）"
    PASS=$((PASS + 1))
    check_optional riscv64-elf-gdb "--version" "macOS 专用 riscv64 gdb，可选：brew install riscv64-elf-gdb"
else
    miss_required "gdb / gdb-multiarch" "见 docs/environment.md"
fi

echo
echo "--- Rust (可选分支，见 docs/rust-track.md) ---"
check_optional rustc "--version" "curl https://sh.rustup.rs | sh"
check_optional cargo "--version" "随 rustc 一起安装"

echo
echo "--- Docker (如果你打算用容器方式，可跳过原生工具链检查结果) ---"
check_optional docker "--version" "见 docs/environment.md 方式一"

echo
echo "=== 汇总 ==="
echo "通过: $PASS   缺失(必需): $FAIL   缺失(可选): $MISSING_OPTIONAL"

if [ "$FAIL" -gt 0 ]; then
    echo
    echo "有必需工具缺失。可以选择："
    echo "  1) 按上面每一行给出的命令单独安装缺失项"
    echo "  2) 或者直接用 Docker：make setup && make run ARCH=x86_64（不需要装任何原生工具链）"
    exit 1
fi

echo
echo "环境检查通过，可以开始 labs/lab00-environment 了。"
exit 0
