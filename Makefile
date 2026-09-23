# osdev-lab 顶层 Makefile
#
# 用法:
#   make setup                         构建/检查环境（Docker 镜像 或 原生工具链检查）
#   make run ARCH=x86_64 [LAB=lab01-boot-hello] [VARIANT=solution]
#   make run ARCH=riscv64
#   make debug ARCH=x86_64 [LAB=...]   QEMU -s -S + gdb
#   make test ARCH=x86_64 [LAB=...]    跑单个 Lab 的自动验收测试
#   make test-all                      跑全部 Lab 在两个架构上的测试（CI 用）
#   make clean                         清理全部 Lab 的构建产物
#   make docker-shell                  进入 Docker 开发容器交互式 shell
#
# ARCH 没有默认值，必须显式指定（run/debug/test 三个目标），
# 这是有意为之：本课程强调双架构对照，不想让人无意识地一直只跑一个架构。

SHELL := /bin/bash
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

LAB ?= lab01-boot-hello
VARIANT ?= solution
TIMEOUT ?= 15

DOCKER_IMAGE := osdev-lab:latest
USE_DOCKER ?= 0

.PHONY: setup run debug test test-all clean docker-build docker-shell help check-arch

help:
	@echo "osdev-lab 顶层命令:"
	@echo "  make setup                         构建/检查环境"
	@echo "  make run ARCH=x86_64|riscv64        运行当前 Lab (默认 LAB=$(LAB))"
	@echo "  make debug ARCH=x86_64|riscv64       QEMU + GDB 调试"
	@echo "  make test ARCH=x86_64|riscv64        跑当前 Lab 自动验收测试"
	@echo "  make test-all                        跑全部 Lab 两个架构的测试"
	@echo "  make clean                           清理构建产物"
	@echo "  make docker-shell                    进入 Docker 开发容器"
	@echo ""
	@echo "指定 Lab / 变体: make run ARCH=x86_64 LAB=lab03-physical-memory VARIANT=starter"
	@echo "详见 README.md"

check-arch:
ifndef ARCH
	$(error 请指定 ARCH=x86_64 或 ARCH=riscv64，例如: make run ARCH=x86_64)
endif
ifneq ($(ARCH),x86_64)
ifneq ($(ARCH),riscv64)
	$(error ARCH 必须是 x86_64 或 riscv64，收到的是: $(ARCH))
endif
endif

setup:
ifeq ($(USE_DOCKER),1)
	docker build -t $(DOCKER_IMAGE) -f docker/Dockerfile .
	@echo "Docker 镜像构建完成。之后可以用 make <target> USE_DOCKER=1 在容器里跑，或 make docker-shell 进入交互式容器。"
else
	bash scripts/check-env.sh
endif

run: check-arch
ifeq ($(USE_DOCKER),1)
	docker run --rm -it -v "$(ROOT_DIR):/workspace" -w /workspace $(DOCKER_IMAGE) \
		bash -c "cd labs/$(LAB) && make ARCH=$(ARCH) VARIANT=$(VARIANT) build && bash ../../scripts/run-qemu.sh ARCH=$(ARCH) LAB=$(LAB) VARIANT=$(VARIANT)"
else
	cd labs/$(LAB) && $(MAKE) ARCH=$(ARCH) VARIANT=$(VARIANT) build
	bash scripts/run-qemu.sh ARCH=$(ARCH) LAB=$(LAB) VARIANT=$(VARIANT)
endif

debug: check-arch
ifeq ($(USE_DOCKER),1)
	@echo "注意: Docker 模式下的交互式 GDB 调试需要容器内 GDB 直连，建议改用: make docker-shell 然后在容器内运行 bash scripts/debug-gdb.sh ARCH=$(ARCH) LAB=$(LAB)"
else
	cd labs/$(LAB) && $(MAKE) ARCH=$(ARCH) VARIANT=$(VARIANT) build
	bash scripts/debug-gdb.sh ARCH=$(ARCH) LAB=$(LAB) VARIANT=$(VARIANT)
endif

test: check-arch
ifeq ($(USE_DOCKER),1)
	docker run --rm -v "$(ROOT_DIR):/workspace" -w /workspace $(DOCKER_IMAGE) \
		bash scripts/test-lab.sh ARCH=$(ARCH) LAB=$(LAB) VARIANT=$(VARIANT) TIMEOUT=$(TIMEOUT)
else
	bash scripts/test-lab.sh ARCH=$(ARCH) LAB=$(LAB) VARIANT=$(VARIANT) TIMEOUT=$(TIMEOUT)
endif

test-all:
	@bash scripts/run-all-tests.sh

clean:
	@for d in labs/*/; do \
		if [ -f "$$d/Makefile" ]; then $(MAKE) -C "$$d" clean; fi; \
	done
	@echo "已清理全部 Lab 构建产物。"

docker-build:
	docker build -t $(DOCKER_IMAGE) -f docker/Dockerfile .

docker-shell: docker-build
	docker run --rm -it -v "$(ROOT_DIR):/workspace" -w /workspace $(DOCKER_IMAGE) bash
