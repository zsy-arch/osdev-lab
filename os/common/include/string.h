/* 裸机环境下的最小字符串/内存函数集合。
 * 不能 #include <string.h>（那是宿主 libc 的头，裸机没有对应实现），
 * 所以这里自己实现一套同名函数，签名尽量兼容标准库，方便你以后对照。
 */
#ifndef OSDEV_STRING_H
#define OSDEV_STRING_H

#include "types.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);

#endif /* OSDEV_STRING_H */
