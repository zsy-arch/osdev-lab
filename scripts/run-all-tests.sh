#!/usr/bin/env bash
# 跑全部已实现 Lab 在两个架构上的自动验收测试。CI 的入口脚本。
#
# 一个 Lab 被认为"已实现"要求它的 tests/ 目录下有 expect.txt 或
# expect-<arch>.txt，且 solution/<arch>/ 下有 Makefile。没有实现的 Lab
# 会被跳过并在汇总里标出来，不算失败（方便课程内容还在增量编写时 CI 不整体挂红）。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LABS_DIR="$ROOT_DIR/labs"

ARCHES=(x86_64 riscv64)
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0
FAILED_LIST=()

for lab_path in "$LABS_DIR"/lab*/; do
    lab="$(basename "$lab_path")"
    for arch in "${ARCHES[@]}"; do
        HAS_EXPECT=0
        if [ -f "$lab_path/tests/expect-$arch.txt" ] || [ -f "$lab_path/tests/expect.txt" ]; then
            HAS_EXPECT=1
        fi
        HAS_SOLUTION=0
        if [ -f "$lab_path/solution/$arch/Makefile" ] || [ -f "$lab_path/Makefile" ]; then
            HAS_SOLUTION=1
        fi

        if [ "$HAS_EXPECT" != "1" ] || [ "$HAS_SOLUTION" != "1" ]; then
            echo "[SKIP] $lab ($arch): 尚未提供测试或 solution 构建规则"
            SKIPPED=$((SKIPPED + 1))
            continue
        fi

        TOTAL=$((TOTAL + 1))
        if bash "$SCRIPT_DIR/test-lab.sh" ARCH="$arch" LAB="$lab" VARIANT=solution; then
            PASSED=$((PASSED + 1))
        else
            FAILED=$((FAILED + 1))
            FAILED_LIST+=("$lab ($arch)")
        fi
    done
done

echo
echo "=== 汇总: 通过 $PASSED / 运行 $TOTAL / 跳过 $SKIPPED ==="
if [ "$FAILED" -gt 0 ]; then
    echo "失败的 Lab:"
    for f in "${FAILED_LIST[@]}"; do
        echo "  - $f"
    done
    exit 1
fi

echo "全部通过。"
exit 0
