#!/usr/bin/env bash
# Lab0 的自动验收测试：不涉及 QEMU 串口输出比对（还没有内核代码），
# 验收标准是环境本身——重新跑一次 scripts/check-env.sh，退出码 0 表示通过。
# 见 tests/expect.txt 里对"通过"的文字定义。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

bash "$ROOT_DIR/scripts/check-env.sh"
EXIT_CODE=$?

if [ "$EXIT_CODE" = "0" ]; then
    echo "[PASS] lab00-environment: check-env.sh 通过，必需工具链齐全"
else
    echo "[FAIL] lab00-environment: check-env.sh 未通过 (exit=$EXIT_CODE)，见上面输出里的 [MISS] 项" >&2
fi

exit "$EXIT_CODE"
