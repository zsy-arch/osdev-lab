#!/usr/bin/env bash
# 跑指定 Lab 的自动验收测试：构建 solution -> 在 QEMU 里跑 -> 抓取串口输出 -> 用期望字符串比对。
#
# 用法:
#   bash scripts/test-lab.sh ARCH=x86_64 LAB=lab01-boot-hello
#   bash scripts/test-lab.sh ARCH=riscv64 LAB=lab01-boot-hello
#
# 每个 Lab 的 tests/ 目录下有一个 expect.txt，包含一行或多行必须在串口输出里
# 按顺序出现的子串（不要求整行完全匹配，只要求包含）。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCH=""
LAB=""
VARIANT="solution"
TIMEOUT="15"

for arg in "$@"; do
    case "$arg" in
        ARCH=*) ARCH="${arg#ARCH=}" ;;
        LAB=*) LAB="${arg#LAB=}" ;;
        VARIANT=*) VARIANT="${arg#VARIANT=}" ;;
        TIMEOUT=*) TIMEOUT="${arg#TIMEOUT=}" ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "$ARCH" ] || [ -z "$LAB" ]; then
    echo "用法: $0 ARCH=x86_64|riscv64 LAB=labXX-name [VARIANT=solution] [TIMEOUT=秒数]" >&2
    exit 2
fi

LAB_DIR="$ROOT_DIR/labs/$LAB"
EXPECT_FILE="$LAB_DIR/tests/expect-$ARCH.txt"
if [ ! -f "$EXPECT_FILE" ]; then
    EXPECT_FILE="$LAB_DIR/tests/expect.txt"
fi

if [ ! -f "$EXPECT_FILE" ]; then
    echo "[FAIL] $LAB ($ARCH): 找不到期望输出文件 (tests/expect-$ARCH.txt 或 tests/expect.txt)" >&2
    exit 1
fi

echo "==> 构建 $LAB ($VARIANT/$ARCH)..."
if ! ( cd "$LAB_DIR" && make VARIANT="$VARIANT" ARCH="$ARCH" build >/tmp/osdev-lab-build-$$.log 2>&1 ); then
    echo "[FAIL] $LAB ($ARCH): 构建失败，日志见 /tmp/osdev-lab-build-$$.log" >&2
    tail -40 "/tmp/osdev-lab-build-$$.log" >&2
    exit 1
fi

echo "==> 在 QEMU 里运行并捕获串口输出 (最长 ${TIMEOUT}s)..."
OUTPUT_FILE="/tmp/osdev-lab-output-$$.log"
bash "$SCRIPT_DIR/run-qemu.sh" ARCH="$ARCH" LAB="$LAB" VARIANT="$VARIANT" TIMEOUT="$TIMEOUT" \
    > "$OUTPUT_FILE" 2>&1
QEMU_EXIT=$?

# QEMU 被 timeout 杀掉时退出码是 124，这是预期行为（我们的内核不会自己退出 QEMU），
# 不代表测试失败，实际判定标准是下面的字符串匹配。
if [ "$QEMU_EXIT" != "0" ] && [ "$QEMU_EXIT" != "124" ]; then
    echo "[FAIL] $LAB ($ARCH): QEMU 异常退出 (exit=$QEMU_EXIT)，输出:" >&2
    cat "$OUTPUT_FILE" >&2
    rm -f "$OUTPUT_FILE"
    exit 1
fi

FAILED=0
while IFS= read -r expected_line; do
    [ -z "$expected_line" ] && continue
    if ! grep -qF "$expected_line" "$OUTPUT_FILE"; then
        echo "[FAIL] $LAB ($ARCH): 输出里没有找到期望的子串: \"$expected_line\"" >&2
        FAILED=1
    fi
done < "$EXPECT_FILE"

if [ "$FAILED" != "0" ]; then
    echo "--- 实际串口输出 ---" >&2
    cat "$OUTPUT_FILE" >&2
    echo "--------------------" >&2
    rm -f "$OUTPUT_FILE"
    exit 1
fi

echo "[PASS] $LAB ($ARCH)"
rm -f "$OUTPUT_FILE"
exit 0
