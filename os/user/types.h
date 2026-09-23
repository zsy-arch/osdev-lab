/* Lab9：用户程序侧的基础类型定义。
 *
 * 为什么用户程序不直接用 src/common/include/types.h（内核那份）：
 *
 * 那个目录里除了 types.h 还有 console.h / kalloc.h / pagetable.h /
 * panic.h ——全是内核内部接口。一旦在用户程序的编译命令里加上
 * -Isrc/common/include，用户程序就*可以*写 #include "kalloc.h" 然后调用
 * kalloc()。它当然链接不过（用户程序不跟内核链接在一起），所以不是安全
 * 问题；但它是一个结构问题：能 include 就意味着边界只存在于"大家别这么
 * 写"的默契里，而不存在于构建系统里。
 *
 * 用户态和内核态的边界是本 Lab 最重要的那条线。这条线在硬件上由特权级
 * 保证，在源码组织上就应该由"能看见哪些头文件"保证——用户程序能看见的
 * 只有 user/ 目录，加上 Lab 根目录下那些描述*外部约定*的格式定义
 * （fs_format.h 定义磁盘上的字节，syscall.h 定义系统调用号）。这两类
 * 东西天然要跨越边界，其余一律不跨。
 *
 * 代价是这个文件跟内核那份 types.h 有重复。这是刻意的重复：它们碰巧长得
 * 一样，但没有任何理由必须保持一样——内核那份将来要加 volatile 相关的
 * 东西、原子类型、per-CPU 相关的宏，用户态这份不需要跟着长。把"现在
 * 内容相同"误认为"应该共享"，是这类重复最常见的误判方向。
 */
#ifndef OSDEV_USER_TYPES_H
#define OSDEV_USER_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long      uint64_t;
typedef signed long        int64_t;

typedef uint64_t           size_t;
typedef int64_t            ssize_t;
typedef uint64_t           uintptr_t;

#define NULL ((void *)0)

/* 跟内核那份 types.h 一样要处理 C23：bool/true/false 在 C23 里是关键字,
 * 再 typedef 就是编译错误。见那边的注释。 */
#if __STDC_VERSION__ < 202311L
#define true  1
#define false 0
typedef _Bool bool;
#endif

#endif /* OSDEV_USER_TYPES_H */
