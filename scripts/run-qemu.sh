#!/usr/bin/env bash
# 启动 QEMU 运行指定架构 / Lab 的内核镜像。
#
# 用法:
#   bash scripts/run-qemu.sh ARCH=x86_64 LAB=lab01-boot-hello VARIANT=solution
#   bash scripts/run-qemu.sh ARCH=riscv64 LAB=lab01-boot-hello VARIANT=solution DEBUG=1
#   bash scripts/run-qemu.sh ARCH=x86_64 LAB=lab03-physical-memory VARIANT=solution MONITOR=1
#
# 环境变量风格的参数（KEY=VALUE 形式的位置参数）是为了和顶层 Makefile 的调用方式统一，
# 顶层 Makefile 直接把 ARCH/LAB/VARIANT 等 make 变量原样转发给这个脚本。
#
# 参数:
#   ARCH      x86_64 | riscv64                (必需)
#   LAB       lab 目录名，如 lab01-boot-hello   (必需)
#   VARIANT   solution | starter                (默认 solution)
#   DEBUG     1 时加 -s -S，配合 gdb 使用        (默认 0)
#   MONITOR   1 时开启 telnet monitor:4444       (默认 0)
#   TIMEOUT   自动退出秒数，0 表示不超时          (默认 0)
#   DISK      磁盘镜像路径，覆盖自动探测；NONE 表示强制不挂盘 (默认自动)
#
# 关于磁盘：Lab8 起内核需要一块硬盘。本脚本默认自动探测
# $VARIANT/$ARCH/build/fs.img，存在就挂上，不存在就不挂——这样 Lab0-7
# 的命令行完全不变，Lab8 也不需要给 test-lab.sh 加参数（它是直接调用本
# 脚本的，没有转发额外参数的途径）。
#
# 两个架构挂盘的方式完全不同，这本身就是 Lab8 的一个知识点：
#   x86_64  : -drive ...,if=ide  挂成传统 ATA 盘，内核用 PIO 端口读它
#   riscv64 : -drive ...,if=none + -device virtio-blk-device 挂成 virtio
#             设备，内核用 MMIO + 描述符环读它
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCH=""
LAB=""
VARIANT="solution"
DEBUG=0
MONITOR=0
TIMEOUT=0
DISK=""

for arg in "$@"; do
    case "$arg" in
        ARCH=*) ARCH="${arg#ARCH=}" ;;
        LAB=*) LAB="${arg#LAB=}" ;;
        VARIANT=*) VARIANT="${arg#VARIANT=}" ;;
        DEBUG=*) DEBUG="${arg#DEBUG=}" ;;
        MONITOR=*) MONITOR="${arg#MONITOR=}" ;;
        TIMEOUT=*) TIMEOUT="${arg#TIMEOUT=}" ;;
        DISK=*) DISK="${arg#DISK=}" ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "$ARCH" ] || [ -z "$LAB" ]; then
    echo "用法: $0 ARCH=x86_64|riscv64 LAB=labXX-name [VARIANT=solution|starter] [DEBUG=1] [MONITOR=1] [TIMEOUT=秒数]" >&2
    exit 2
fi

LAB_DIR="$ROOT_DIR/labs/$LAB"
if [ ! -d "$LAB_DIR" ]; then
    echo "找不到 Lab 目录: $LAB_DIR" >&2
    exit 2
fi

BUILD_DIR="$LAB_DIR/$VARIANT/$ARCH/build"

case "$ARCH" in
    x86_64)
        KERNEL_ELF="$BUILD_DIR/kernel.elf"
        ISO="$BUILD_DIR/os.iso"
        if [ ! -f "$ISO" ] && [ ! -f "$KERNEL_ELF" ]; then
            echo "没有找到构建产物，先运行: (cd $LAB_DIR && make VARIANT=$VARIANT ARCH=x86_64)" >&2
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
            echo "没有找到构建产物，先运行: (cd $LAB_DIR && make VARIANT=$VARIANT ARCH=riscv64)" >&2
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

# 磁盘：DISK 没给就自动探测 build/fs.img。DISK=NONE 强制不挂（用于
# 演示"没有盘时内核报什么错"——Lab8 README 的排错一节会让你试一次）。
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
        echo "  生成方式: (cd $LAB_DIR && make ARCH=$ARCH fs.img)" >&2
        exit 1
    fi
    case "$ARCH" in
        x86_64)
            # if=ide 让 QEMU 把它接到传统 ATA 控制器上，内核的 ide.c 才能
            # 用 0x1F0 那组端口访问它。注意 index=0 是 ATA 主总线主盘。
            #
            # 这里的 -cdrom（上面 ISO 那段加的）和 -drive index=0 不冲突：
            # -cdrom 是 IDE 从盘（index=2，第二条总线），我们的硬盘是主盘。
            # 如果把硬盘也写成 index=2，QEMU 会直接报 "drive index 2 used
            # twice" 拒绝启动。
            QEMU_ARGS+=(-drive "file=$DISK,format=raw,index=0,if=ide")
            ;;
        riscv64)
            # riscv virt 机器上没有 ATA 控制器，用 virtio。分两步：
            # -drive if=none 只声明后端（这个文件），-device 才把它接成一个
            # 虚拟设备。virt 机器用的是 virtio-mmio 总线，所以设备名是
            # virtio-blk-device（PCI 版本叫 virtio-blk-pci，那个在这台机器
            # 上也能用但需要内核实现 PCI 枚举）。
            QEMU_ARGS+=(-drive "file=$DISK,format=raw,if=none,id=d0")

            # 下面这两个参数都不是可选的装饰，各自解决一个实测出来的坑。
            # 两者都用 `info qtree` / `-machine dumpdtb` 在本机核对过。
            #
            # (1) force-legacy=false —— 要 virtio 版本 2（现代接口）。
            #     virt 机器上 8 个 virtio-mmio 传输通道的 force-legacy 默认
            #     是 true，此时 Version 寄存器读出来是 1（legacy）。legacy
            #     接口用 QueuePFN + GuestPageSize + QueueAlign 那一套"整个
            #     队列必须是一块连续、按页对齐的区域"的老规矩；版本 2 才有
            #     QueueDescLow/High、QueueDriverLow/High、QueueDeviceLow/High
            #     这组"三块内存各自独立给地址"的寄存器，代码干净得多，也是
            #     virtio 1.0 规范正文描述的形式。用 -global 是因为这 8 个
            #     传输通道是 virt 机器自己创建的，命令行上没有它们的 id，
            #     只能按设备类型全局设置。
            #
            # (2) bus=virtio-mmio-bus.0 —— 把盘钉在 0x10001000。
            #     不指定 bus 时 QEMU *从高往低*分配槽位：只挂一块盘时它落在
            #     virtio-mmio-bus.7，也就是 0x10008000，而不是直觉上的第一个
            #     槽位 0x10001000（实测：info qtree 显示 virtio-mmio-bus.7；
            #     dumpdtb 确认 8 个槽位是 0x10001000..0x10008000，每个占
            #     0x1000）。很多教程里硬编码的 0x10001000 因此会读到一个
            #     "魔数对不上"的空槽位。显式钉住槽位让地址变成确定的，跟
            #     xv6-riscv 的做法一致。
            #
            # 注意 virtio.c 仍然会把 8 个槽位全扫一遍，不依赖这里钉的槽位——
            # 钉槽位是为了让行为确定，扫描是为了让驱动在没钉的时候也能工作，
            # 两者不重复：一个约束环境，一个约束代码。
            QEMU_ARGS+=(-global virtio-mmio.force-legacy=false)
            QEMU_ARGS+=(-device virtio-blk-device,drive=d0,bus=virtio-mmio-bus.0)
            ;;
    esac
fi

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "找不到 $QEMU_BIN，见 docs/environment.md 安装 QEMU" >&2
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
    # 用 timeout 包一层，用于自动化测试；超时退出码固定是 124（GNU coreutils timeout 的约定）。
    #
    # 这里故意不做"找不到就忽略 TIMEOUT 直接跑"的静默兜底：QEMU 用 -serial stdio
    # 起来之后，如果内核没打印期望的字符串就会一直占着终端等下一个字节，没有
    # timeout/gtimeout 包一层的话这个进程永远不会自己退出。实测在一台没装过
    # coreutils 的全新 macOS 上，`make test` 就是这样卡死超过 10 分钟一直
    # "运行中"，看起来像内核挂了，其实是脚本本身没有真的施加超时。
    if command -v timeout >/dev/null 2>&1; then
        exec timeout "${TIMEOUT}s" "$QEMU_BIN" "${QEMU_ARGS[@]}"
    elif command -v gtimeout >/dev/null 2>&1; then
        # macOS 默认没有 GNU timeout，brew install coreutils 后叫 gtimeout
        exec gtimeout "${TIMEOUT}s" "$QEMU_BIN" "${QEMU_ARGS[@]}"
    else
        echo "错误: 未找到 timeout/gtimeout，无法对 QEMU 施加 ${TIMEOUT}s 超时。" >&2
        echo "  这不是警告后可以忽略的问题：QEMU 用 -serial stdio 起来后如果内核" >&2
        echo "  没有按预期打印完就会一直挂着不退出，自动化测试会卡死。" >&2
        echo "  macOS: brew install coreutils (装完后命令叫 gtimeout)" >&2
        echo "  Linux: timeout 是 coreutils 自带的，一般已经有了" >&2
        exit 1
    fi
else
    exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
fi
