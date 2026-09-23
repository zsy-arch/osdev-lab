#!/usr/bin/env bash
# 启动 QEMU 运行本目录构建出的内核镜像。
#
# 用法:
#   bash scripts/run-qemu.sh ARCH=x86_64
#   bash scripts/run-qemu.sh ARCH=riscv64 DEBUG=1
#   bash scripts/run-qemu.sh ARCH=x86_64 MONITOR=1
#
# 参数（KEY=VALUE 形式的位置参数，风格跟顶层 Makefile 调用方式统一）:
#   ARCH      x86_64 | riscv64                (必需)
#   DEBUG     1 时加 -s -S，配合 gdb 使用        (默认 0)
#   MONITOR   1 时开启 telnet monitor:4444       (默认 0)
#   TIMEOUT   自动退出秒数，0 表示不超时          (默认 0)
#   DISK      磁盘镜像路径，覆盖自动探测；NONE 表示强制不挂盘 (默认自动)
#
# 磁盘：默认自动探测 $ARCH/build/fs.img，存在就挂上。两个架构挂盘方式
# 完全不同：
#   x86_64  : -drive ...,if=ide  挂成传统 ATA 盘，内核用 PIO 端口读它
#   riscv64 : -drive ...,if=none + -device virtio-blk-device 挂成 virtio
#             设备，内核用 MMIO + 描述符环读它
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCH=""
DEBUG=0
MONITOR=0
TIMEOUT=0
DISK=""

for arg in "$@"; do
    case "$arg" in
        ARCH=*) ARCH="${arg#ARCH=}" ;;
        DEBUG=*) DEBUG="${arg#DEBUG=}" ;;
        MONITOR=*) MONITOR="${arg#MONITOR=}" ;;
        TIMEOUT=*) TIMEOUT="${arg#TIMEOUT=}" ;;
        DISK=*) DISK="${arg#DISK=}" ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "$ARCH" ]; then
    echo "用法: $0 ARCH=x86_64|riscv64 [DEBUG=1] [MONITOR=1] [TIMEOUT=秒数]" >&2
    exit 2
fi

BUILD_DIR="$ROOT_DIR/$ARCH/build"

case "$ARCH" in
    x86_64)
        KERNEL_ELF="$BUILD_DIR/kernel.elf"
        ISO="$BUILD_DIR/os.iso"
        if [ ! -f "$ISO" ] && [ ! -f "$KERNEL_ELF" ]; then
            echo "没有找到构建产物，先运行: make build ARCH=x86_64" >&2
            exit 1
        fi
        QEMU_BIN=qemu-system-x86_64
        QEMU_ARGS=(-serial stdio -display none -no-reboot -no-shutdown)
        if [ -f "$ISO" ]; then
            QEMU_ARGS+=(-cdrom "$ISO")
        else
            QEMU_ARGS+=(-kernel "$KERNEL_ELF")
        fi
        ;;
    riscv64)
        KERNEL_ELF="$BUILD_DIR/kernel.elf"
        if [ ! -f "$KERNEL_ELF" ]; then
            echo "没有找到构建产物，先运行: make build ARCH=riscv64" >&2
            exit 1
        fi
        QEMU_BIN=qemu-system-riscv64
        QEMU_ARGS=(-machine virt -bios default -kernel "$KERNEL_ELF" -serial mon:stdio -display none -no-reboot)
        ;;
    *)
        echo "不支持的 ARCH: $ARCH (只支持 x86_64 / riscv64)" >&2
        exit 2
        ;;
esac

if [ -z "$DISK" ]; then
    if [ -f "$BUILD_DIR/fs.img" ]; then
        DISK="$BUILD_DIR/fs.img"
    else
        DISK="NONE"
    fi
fi

if [ "$DISK" != "NONE" ]; then
    if [ ! -f "$DISK" ]; then
        echo "指定的磁盘镜像不存在: $DISK" >&2
        echo "  生成方式: make fs.img ARCH=$ARCH" >&2
        exit 1
    fi
    case "$ARCH" in
        x86_64)
            # if=ide 接到传统 ATA 控制器上，index=0 是主总线主盘；-cdrom
            # 挂的是 IDE 从盘（index=2），两者不冲突。
            QEMU_ARGS+=(-drive "file=$DISK,format=raw,index=0,if=ide")
            ;;
        riscv64)
            # riscv virt 机器没有 ATA 控制器，用 virtio-mmio。
            #   force-legacy=false: 要 virtio 版本 2（现代接口），否则
            #     Version 寄存器读出来是 1（legacy），驱动按 v2 寄存器布局
            #     读会全部落空。
            #   bus=virtio-mmio-bus.0: 把盘钉在 0x10001000——不指定 bus 时
            #     QEMU 从高往低分配槽位，只挂一块盘会落在
            #     virtio-mmio-bus.7（0x10008000），跟驱动/教程里假设的
            #     第一槽位对不上。
            QEMU_ARGS+=(-drive "file=$DISK,format=raw,if=none,id=d0")
            QEMU_ARGS+=(-global virtio-mmio.force-legacy=false)
            QEMU_ARGS+=(-device virtio-blk-device,drive=d0,bus=virtio-mmio-bus.0)
            ;;
    esac
fi

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "找不到 $QEMU_BIN，请安装 QEMU（Linux: apt install qemu-system-x86 qemu-system-misc | macOS: brew install qemu）" >&2
    exit 1
fi

if [ "$DEBUG" = "1" ]; then
    QEMU_ARGS+=(-s -S)
    echo ">>> DEBUG 模式：QEMU 已挂起等待 GDB 连接 localhost:1234" >&2
fi

if [ "$MONITOR" = "1" ]; then
    QEMU_ARGS+=(-monitor telnet:127.0.0.1:4444,server,nowait)
    echo ">>> Monitor 已开启：telnet 127.0.0.1 4444" >&2
fi

echo ">>> $QEMU_BIN ${QEMU_ARGS[*]}" >&2

if [ "$TIMEOUT" != "0" ]; then
    # 用 timeout 包一层：QEMU 用 -serial stdio 起来后，如果内核没打印期望
    # 的字符串就会一直占着终端等下一个字节，不包超时会永远挂着不退出。
    if command -v timeout >/dev/null 2>&1; then
        exec timeout "${TIMEOUT}s" "$QEMU_BIN" "${QEMU_ARGS[@]}"
    elif command -v gtimeout >/dev/null 2>&1; then
        # macOS 默认没有 GNU timeout，brew install coreutils 后叫 gtimeout
        exec gtimeout "${TIMEOUT}s" "$QEMU_BIN" "${QEMU_ARGS[@]}"
    else
        echo "错误: 未找到 timeout/gtimeout，无法对 QEMU 施加 ${TIMEOUT}s 超时。" >&2
        echo "  macOS: brew install coreutils (装完后命令叫 gtimeout)" >&2
        echo "  Linux: timeout 是 coreutils 自带的，一般已经有了" >&2
        exit 1
    fi
else
    exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
fi
