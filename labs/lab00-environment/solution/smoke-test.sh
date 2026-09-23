#!/usr/bin/env bash
# 环境烟雾测试：不涉及任何 Lab1+ 的引导协议知识，只验证：
#   1. 交叉编译器能把一段裸机汇编组装成 ELF
#   2. QEMU 能把这个 ELF 当 -kernel 加载起来执行
#   3. 串口输出通路是通的
#
# riscv64 侧：QEMU 的 -kernel 参数对 riscv64 virt 机器支持直接加载任意裸机 ELF
# (OpenSBI 固件加载后跳转过去)，不需要 Multiboot2/GRUB 那一套。
# x86_64 侧不一样：QEMU 的 -kernel 不认任意裸机 ELF（实测报
# "Error loading uncompressed kernel without PVH ELF Note"），但它认 Multiboot1
# 魔数头，所以这里手写一个最小 Multiboot1 header 来让 -kernel 直接加载，不需要
# 真的装 GRUB 生成 ISO——这个不对称也是留给你直觉上体会"Lab1 里 x86_64 为什么
# 要多装 GRUB 这一层"的一个前置对比。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# macOS 默认没有 GNU timeout（BSD 不带，brew install coreutils 后才有 gtimeout）。
# 三档回退：timeout（Linux/装了 coreutils 的 macOS）-> gtimeout（macOS + coreutils）
# -> 都没有就靠 QEMU 自己的 -no-reboot + 后台运行 N 秒后 kill 兜底。
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi

run_with_timeout() {
    local secs="$1"; shift
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" "${secs}s" "$@" 2>&1 || true
        return
    fi
    # 无 timeout/gtimeout 时的手动兜底：后台跑，睡够时间后强制 kill。
    "$@" > "$WORK_DIR/qemu_out.log" 2>&1 &
    local qemu_pid=$!
    ( sleep "$secs"; kill "$qemu_pid" 2>/dev/null || true ) &
    local killer_pid=$!
    wait "$qemu_pid" 2>/dev/null || true
    kill "$killer_pid" 2>/dev/null || true
    cat "$WORK_DIR/qemu_out.log"
}

echo "=== riscv64 烟雾测试 ==="

RISCV_GCC=""
for candidate in riscv64-elf-gcc riscv64-unknown-elf-gcc riscv64-linux-gnu-gcc; do
    if command -v "$candidate" >/dev/null 2>&1; then RISCV_GCC="$candidate"; break; fi
done

if [ -z "$RISCV_GCC" ]; then
    echo "跳过：没找到 riscv64 交叉编译器（见 docs/environment.md 安装）" >&2
    exit 1
else
    cat > "$WORK_DIR/smoke_riscv64.S" <<'EOF'
.section .text
.global _start
_start:
    /* riscv64 QEMU virt 机器的 UART16550 MMIO 基址是 0x10000000，
     * 直接往这个地址写字节就是往串口发字符（细节在 Lab1 才展开讲）。 */
    li   t0, 0x10000000
    li   t1, 'O'
    sb   t1, 0(t0)
    li   t1, 'K'
    sb   t1, 0(t0)
    li   t1, '\n'
    sb   t1, 0(t0)
1:
    wfi
    j    1b
EOF
    # 不能只用 -Ttext=0x80200000 走链接器默认脚本：较新的 riscv64-elf-gcc 会带
    # 一个 .riscv.attributes 元数据 section，默认脚本把它和 .text 揉进同一个
    # PT_LOAD segment、还按 4K 对齐塞到 .text 前面，导致整个 LOAD segment 的
    # VirtAddr 变成 0x801ff000（比预期少 0x1000）——QEMU/OpenSBI 严格按 ELF
    # LOAD segment 的地址加载，于是真正跑起来的代码其实是 .riscv.attributes
    # 的内容，串口自然什么都不输出。用显式链接脚本把这个 section 丢掉即可。
    cat > "$WORK_DIR/smoke_riscv64.ld" <<'EOF'
ENTRY(_start)
SECTIONS {
    . = 0x80200000;
    .text : { *(.text) }
    /DISCARD/ : { *(.riscv.attributes) *(.comment) }
}
EOF
    "$RISCV_GCC" -nostdlib -static -march=rv64gc -mabi=lp64d \
        -T "$WORK_DIR/smoke_riscv64.ld" -o "$WORK_DIR/smoke_riscv64.elf" "$WORK_DIR/smoke_riscv64.S"

    OUTPUT="$(run_with_timeout 3 qemu-system-riscv64 -machine virt -bios default \
        -kernel "$WORK_DIR/smoke_riscv64.elf" -display none -serial mon:stdio -no-reboot)"

    if echo "$OUTPUT" | grep -q "OK"; then
        echo "[PASS] riscv64: QEMU 输出包含 'OK'"
    else
        echo "[FAIL] riscv64: 没有看到期望输出，实际输出："
        echo "$OUTPUT"
        exit 1
    fi
fi

echo
echo "=== x86_64 烟雾测试 ==="
echo "说明：跟 riscv64 不一样，QEMU 的 x86_64 -kernel 参数不能直接吃一个任意的裸机"
echo "ELF——实测直接扔一个没有 Multiboot 头的 ELF 进去，QEMU 会报"
echo "'Error loading uncompressed kernel without PVH ELF Note' 而不是真的执行它。"
echo "QEMU 内置认识的是 Multiboot1 魔数头，所以这里手写一个最小 Multiboot1 header"
echo "（仅 3 个 long：magic/flags/checksum），不需要装 GRUB 就能让 -kernel 直接加载。"
echo "Lab1 的正式内容会展开讲 Multiboot2 + GRUB 的完整流程，这里只是最小验证。"

X86_CC="gcc"
if command -v x86_64-elf-gcc >/dev/null 2>&1; then
    X86_CC="x86_64-elf-gcc"
fi

cat > "$WORK_DIR/smoke_x86_64.S" <<'EOF'
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x0
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .text
.global _start
.code32
_start:
    /* 简化起见，这里直接假设已经处于 32 位保护模式（QEMU 认出 Multiboot1 头后
     * 会帮你做到这一点），往 0x3F8（COM1 数据端口）写字节。
     * 完整的 x86_64 boot 流程（含到 64 位长模式的切换）是 Lab1 的正式内容。 */
    mov  $0x3F8, %dx
    mov  $'O', %al
    out  %al, (%dx)
    mov  $'K', %al
    out  %al, (%dx)
    mov  $'\n', %al
    out  %al, (%dx)
1:
    hlt
    jmp  1b
EOF

cat > "$WORK_DIR/smoke_x86_64.ld" <<'EOF'
ENTRY(_start)
SECTIONS {
    . = 0x100000;
    .multiboot : { *(.multiboot) }
    .text : { *(.text) }
    /DISCARD/ : { *(.comment) *(.eh_frame) }
}
EOF

"$X86_CC" -m32 -ffreestanding -nostdlib -c -o "$WORK_DIR/smoke_x86_64.o" "$WORK_DIR/smoke_x86_64.S"
LD="ld"
if command -v x86_64-elf-ld >/dev/null 2>&1; then LD="x86_64-elf-ld"; fi
"$LD" -m elf_i386 -T "$WORK_DIR/smoke_x86_64.ld" -o "$WORK_DIR/smoke_x86_64.elf" "$WORK_DIR/smoke_x86_64.o"

OUTPUT="$(run_with_timeout 3 qemu-system-x86_64 -kernel "$WORK_DIR/smoke_x86_64.elf" \
    -display none -serial stdio -no-reboot)"

if echo "$OUTPUT" | grep -q "OK"; then
    echo "[PASS] x86_64: QEMU 输出包含 'OK'"
else
    echo "[FAIL] x86_64: 没有看到期望输出，实际输出："
    echo "$OUTPUT"
    exit 1
fi

echo
echo "两个架构烟雾测试都通过，工具链基本可用。进入 labs/lab01-boot-hello 开始正式内容。"
