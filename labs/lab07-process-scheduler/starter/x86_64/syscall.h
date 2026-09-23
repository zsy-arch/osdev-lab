/* Lab6：系统调用号约定，x86_64/riscv64 共用同一份定义（放在各架构目录
 * 下而不是 src/common/include，是因为 Lab6 的 syscall.h/syscall.c 整体
 * 还没到"两个架构共享同一份实现"的程度——分发方式（syscall/sysret vs
 * ecall/sret）、参数寄存器完全不同，只有"调用号数值"本身是可以共用的
 * 约定，为了不在 src/common 里放一个"其实只被两个几乎独立的实现各自
 * include 一份"的文件，直接在两边各放一份内容相同的 syscall.h，跟
 * pit.h/sbi.h 只属于各自架构目录是同一个道理）。
 *
 * 只定义两个号：sys_write 是 ROADMAP 明确要求的"最小系统调用"，
 * sys_exit 是让用户程序能够主动结束执行、把控制权交还内核，而不是
 * 执行到 user_prog.S 末尾后掉进未定义的后续内存——没有 exit，
 * "用户程序正常结束"这件事没有一种干净的表达方式。
 *
 * Lab7 新增三个号：fork/exec/wait，是本 Lab 要演示的"进程"这个抽象
 * 真正需要的最小系统调用集合——没有 fork，proc_table 里永远只有
 * kernel_main.c 创建的那一两个初始进程，看不出"进程"和"调度"这两个
 * 概念在动态创建/销毁场景下的样子。 */
#ifndef OSDEV_SYSCALL_H
#define OSDEV_SYSCALL_H

#define SYS_WRITE 1
#define SYS_EXIT  2
#define SYS_FORK  3
#define SYS_EXEC  4
#define SYS_WAIT  5

#endif /* OSDEV_SYSCALL_H */
