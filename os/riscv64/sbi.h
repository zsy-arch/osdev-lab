/* Lab5 riscv64：SBI（Supervisor Binary Interface）调用的最小封装——
 * 本课程第一次用到 ecall。
 *
 * 为什么定时器不能像 x86_64 的 PIT 那样直接编程硬件：QEMU 的
 * riscv64 virt 平台上，CLINT（Core Local Interruptor，管 mtime/
 * mtimecmp 这两个跟定时器相关的 MMIO 寄存器）属于 M-mode 的地址空间，
 * S-mode（本课程内核运行的特权级，见 Lab1 boot.S 里从 M-mode 切到
 * S-mode 那一步）架构上不允许直接访问——RISC-V Privileged
 * Architecture 规范把 CLINT 划给了 M-mode 软件（这里是 QEMU
 * `-bios default` 加载的 OpenSBI 固件）管理，S-mode 内核必须通过
 * SBI（M-mode 固件对 S-mode 暴露的服务调用接口，跟 x86 BIOS/UEFI
 * 对操作系统暴露服务是类似的角色，但用 ecall 而不是软中断/函数表）
 * 请求 M-mode 代劳。
 *
 * SBI 二进制调用约定（RISC-V SBI Specification "Binary Encoding"
 * 一节）：
 *   a7 = EID（Extension ID）
 *   a6 = FID（Function ID，同一个 extension 下可能有多个函数）
 *   a0-a5 = 最多 6 个参数
 *   ecall
 *   返回时 a0 = 错误码（SBI_SUCCESS=0 等），a1 = 返回值（如果有）
 *
 * TIME extension（EID=0x54494D45，即 ASCII "TIME"）只有一个函数
 * sbi_set_timer（FID=0）：设置 mtimecmp 为参数给的绝对时间值，M-mode
 * 收到后会在那个时刻触发一次 Machine Timer Interrupt 并委托
 * （delegate）成 Supervisor Timer Interrupt 转发给 S-mode——这是
 * "S-mode 请 M-mode 代为编程 CLINT"这件事在 SBI 层面的具体体现。
 * 参考：riscv-sbi-doc "TIME Extension"一节；OpenSBI 是这个 extension
 * 在 QEMU virt 平台上的具体实现者。 */
#ifndef OSDEV_SBI_H
#define OSDEV_SBI_H

#include "types.h"

/* 请求 M-mode 在 time CSR 的值达到 stime_value 时触发一次 Supervisor
 * Timer Interrupt。stime_value 是*绝对*时间值（不是"再过多久"的相对
 * 值），调用方需要自己读当前 time CSR 再加上想要的间隔——本 Lab在
 * kernel_main.c 里就是这么用的。 */
void sbi_set_timer(uint64_t stime_value);

#endif /* OSDEV_SBI_H */
