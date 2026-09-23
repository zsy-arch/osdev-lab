#!/usr/bin/env bash
# 后台起 QEMU（挂起等待调试器），拉起对应架构的 GDB 并自动 target remote。
#
# 用法:
#   bash scripts/debug-gdb.sh ARCH=x86_64 LAB=lab05-interrupt-trap
#   bash scripts/debug-gdb.sh ARCH=riscv64 LAB=lab05-interrupt-trap VARIANT=starter
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCH=""
LAB=""
VARIANT="solution"

for arg in "$@"; do
    case "$arg" in
        ARCH=*) ARCH="${arg#ARCH=}" ;;
        LAB=*) LAB="${arg#LAB=}" ;;
        VARIANT=*) VARIANT="${arg#VARIANT=}" ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "$ARCH" ] || [ -z "$LAB" ]; then
    echo "用法: $0 ARCH=x86_64|riscv64 LAB=labXX-name [VARIANT=solution|starter]" >&2
    exit 2
fi

LAB_DIR="$ROOT_DIR/labs/$LAB"
KERNEL_ELF="$LAB_DIR/$VARIANT/$ARCH/build/kernel.elf"

if [ ! -f "$KERNEL_ELF" ]; then
    echo "找不到 $KERNEL_ELF，先构建: (cd $LAB_DIR && make VARIANT=$VARIANT ARCH=$ARCH)" >&2
    exit 1
fi

# 选一个能用的 GDB：优先架构专属，其次 multiarch，其次通用 gdb。
GDB_BIN=""
case "$ARCH" in
    x86_64)
        for candidate in x86_64-elf-gdb gdb-multiarch gdb; do
            if command -v "$candidate" >/dev/null 2>&1; then GDB_BIN="$candidate"; break; fi
        done
        ;;
    riscv64)
        for candidate in riscv64-elf-gdb gdb-multiarch gdb; do
            if command -v "$candidate" >/dev/null 2>&1; then GDB_BIN="$candidate"; break; fi
        done
        ;;
    *)
        echo "不支持的 ARCH: $ARCH" >&2
        exit 2
        ;;
esac

if [ -z "$GDB_BIN" ]; then
    echo "找不到可用的 GDB，见 docs/environment.md 安装 gdb-multiarch 或架构专属 gdb" >&2
    exit 1
fi

echo ">>> 后台启动 QEMU (挂起等待调试器)..." >&2
bash "$SCRIPT_DIR/run-qemu.sh" ARCH="$ARCH" LAB="$LAB" VARIANT="$VARIANT" DEBUG=1 \
    > "/tmp/osdev-lab-qemu-debug-$$.log" 2>&1 &
QEMU_PID=$!

cleanup() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

# 等待 gdbstub 端口就绪，最多等 5 秒。
READY=0
for _ in $(seq 1 50); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU 提前退出，日志:" >&2
        cat "/tmp/osdev-lab-qemu-debug-$$.log" >&2
        exit 1
    fi
    if (exec 3<>"/dev/tcp/127.0.0.1/1234") 2>/dev/null; then
        exec 3>&-
        READY=1
        break
    fi
    sleep 0.1
done

if [ "$READY" != "1" ]; then
    echo "等待 gdbstub 端口 1234 超时" >&2
    exit 1
fi

echo ">>> 使用 $GDB_BIN 连接 localhost:1234" >&2
echo ">>> 常用命令: break kernel_main / continue / info registers / x/10i \$pc" >&2
echo ">>> 退出 gdb 会自动关闭后台 QEMU" >&2

GDBINIT="/tmp/osdev-lab-gdbinit-$$"
cat > "$GDBINIT" <<'EOF'
target remote localhost:1234
EOF

"$GDB_BIN" -x "$GDBINIT" "$KERNEL_ELF"

rm -f "$GDBINIT"
