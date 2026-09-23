#!/usr/bin/env bash
# 构建 -> 在 QEMU 里跑 -> 抓取串口输出 -> 跟 tests/expect-<arch>.txt 逐行子串比对。
#
# 用法:
#   bash scripts/test.sh ARCH=x86_64
#   bash scripts/test.sh ARCH=riscv64 TIMEOUT=20
#
# tests/expect-<arch>.txt 里每一行是必须在串口输出里出现的一个子串（不要求
# 整行完全匹配，也不要求这些子串本身相邻——只要求各自都能在输出里 grep 到）。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCH=""
TIMEOUT="15"

for arg in "$@"; do
    case "$arg" in
        ARCH=*) ARCH="${arg#ARCH=}" ;;
        TIMEOUT=*) TIMEOUT="${arg#TIMEOUT=}" ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "$ARCH" ]; then
    echo "用法: $0 ARCH=x86_64|riscv64 [TIMEOUT=秒数]" >&2
    exit 2
fi

EXPECT_FILE="$ROOT_DIR/tests/expect-$ARCH.txt"
if [ ! -f "$EXPECT_FILE" ]; then
    echo "[FAIL] ($ARCH): 找不到期望输出文件 $EXPECT_FILE" >&2
    exit 1
fi

echo "==> 构建 ($ARCH)..."
if ! ( cd "$ROOT_DIR" && make ARCH="$ARCH" build >/tmp/os-build-$$.log 2>&1 ); then
    echo "[FAIL] ($ARCH): 构建失败，日志见 /tmp/os-build-$$.log" >&2
    tail -40 "/tmp/os-build-$$.log" >&2
    exit 1
fi

echo "==> 在 QEMU 里运行并捕获串口输出 (最长 ${TIMEOUT}s)..."
OUTPUT_FILE="/tmp/os-output-$$.log"
bash "$SCRIPT_DIR/run-qemu.sh" ARCH="$ARCH" TIMEOUT="$TIMEOUT" \
    > "$OUTPUT_FILE" 2>&1
QEMU_EXIT=$?

# QEMU 被 timeout 杀掉时退出码是 124，这是预期行为（内核不会自己退出
# QEMU），不代表测试失败，实际判定标准是下面的字符串匹配。
if [ "$QEMU_EXIT" != "0" ] && [ "$QEMU_EXIT" != "124" ]; then
    echo "[FAIL] ($ARCH): QEMU 异常退出 (exit=$QEMU_EXIT)，输出:" >&2
    cat "$OUTPUT_FILE" >&2
    rm -f "$OUTPUT_FILE"
    exit 1
fi

FAILED=0
while IFS= read -r expected_line; do
    [ -z "$expected_line" ] && continue
    if ! grep -qF "$expected_line" "$OUTPUT_FILE"; then
        echo "[FAIL] ($ARCH): 输出里没有找到期望的子串: \"$expected_line\"" >&2
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

echo "[PASS] ($ARCH)"
rm -f "$OUTPUT_FILE"
exit 0
