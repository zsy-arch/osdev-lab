/* 跨架构共享的基础类型定义。
 * 裸机环境下没有 <stdint.h> 之类的宿主头文件可用（除非编译器自带 freestanding
 * 版本的 <stdint.h>，GCC/Clang 通常有，但为了完全掌控、不依赖任何隐藏假设，
 * 本课程自己定义一套最小类型集）。
 */
#ifndef OSDEV_TYPES_H
#define OSDEV_TYPES_H

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
typedef int64_t            intptr_t;

#define NULL ((void *)0)

/* C23（__STDC_VERSION__ >= 202311L）把 bool/true/false 变成了真正的关键字，
 * 这时候再 `typedef _Bool bool` 或 `#define true 1` 都是编译错误——不是
 * 猜测，是 2026 年新装的 GCC 16 默认就是 C23，实测触发过
 * "'bool' cannot be defined via 'typedef'"。C11/C17 下它们仍然只是
 * <stdbool.h> 提供的宏/typedef，本课程不含 <stdbool.h>，需要自己补上。 */
#if __STDC_VERSION__ < 202311L
#define true  1
#define false 0
typedef _Bool bool;
#endif

#define UINT64_MAX 0xFFFFFFFFFFFFFFFFUL
#define UINT32_MAX 0xFFFFFFFFU

#endif /* OSDEV_TYPES_H */
