# osdev-lab 开发环境镜像
#
# 固定基础镜像版本，保证所有人构建出的工具链版本一致。
# 如果 apt 源在你所在网络环境下很慢，把下面这行取消注释，换成国内镜像源
# （例如清华 TUNA），不影响后续任何指令：
#   RUN sed -i 's|http://archive.ubuntu.com|https://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PATH=/root/.cargo/bin:$PATH

# --- 基础构建工具 + x86_64 侧全部依赖 ---
# build-essential/clang/lld: 宿主与备用编译器
# nasm: 部分 Lab 的引导扇区/16 位代码用 NASM 语法示例（主线用 GNU as，见 docs/environment.md 说明）
# qemu-system-x86 / qemu-system-misc: 提供 qemu-system-x86_64 与 qemu-system-riscv64
# grub-pc-bin/grub-common/xorriso/mtools: grub-mkrescue 制作 Multiboot2 ISO 所需
# gdb-multiarch: 一个 gdb 同时调试两种架构
# gcc-riscv64-linux-gnu/binutils-riscv64-linux-gnu: riscv64 交叉工具链
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        lld \
        nasm \
        binutils \
        gdb-multiarch \
        qemu-system-x86 \
        qemu-system-misc \
        qemu-utils \
        grub-pc-bin \
        grub-common \
        xorriso \
        mtools \
        gcc-riscv64-linux-gnu \
        binutils-riscv64-linux-gnu \
        git \
        make \
        curl \
        ca-certificates \
        python3 \
        python3-pip \
        vim \
        less \
    && rm -rf /var/lib/apt/lists/*

# --- Rust（可选分支，默认安装但不强制使用） ---
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal \
    && rustup target add x86_64-unknown-none \
    && rustup target add riscv64gc-unknown-none-elf \
    && rustup component add llvm-tools-preview \
    && cargo install cargo-binutils

WORKDIR /workspace

# 容器本身不设置 ENTRYPOINT/CMD 为特定命令，保持通用 shell，
# 方便顶层 Makefile 用 `docker run ... make ...` 的方式复用同一个镜像。
CMD ["/bin/bash"]
