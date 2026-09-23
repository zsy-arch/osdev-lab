/* 架构无关的格式化输出实现，建立在每个架构提供的 console_putc() 之上。
 *
 * 用了 <stdarg.h>：这个头文件不是宿主 libc 提供的，是编译器（GCC/Clang）
 * 自带的内建头，专门配合 -ffreestanding 场景使用，声明的都是 __builtin_va_*
 * 系列的包装，不会拉入任何需要链接的库函数，裸机环境放心用。
 */
#include <stdarg.h>
#include "console.h"

void console_puts(const char *s)
{
    while (*s) {
        console_putc(*s++);
    }
}

void console_puts_line(const char *s)
{
    console_puts(s);
    console_putc('\n');
}

static void print_uint(uint64_t v, unsigned base, bool uppercase)
{
    char buf[32];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        console_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0) {
        console_putc(buf[--i]);
    }
}

static void print_int(int64_t v)
{
    if (v < 0) {
        console_putc('-');
        /* 注意：INT64_MIN 取负会溢出，本课程内核不会打印那种极端值，
         * 教学实现里不特殊处理这一个边界情况。 */
        print_uint((uint64_t)(-v), 10, false);
    } else {
        print_uint((uint64_t)v, 10, false);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            console_putc(*p);
            continue;
        }
        p++;

        /* l 长度修饰符：下一个字符还是 d/u/x/X 才生效，只是告诉 va_arg
         * 该按 64 位取参数——不单独消费一个"格式字符"，跟 d/u/x/X
         * 共用同一个 switch 分支里的 long/int 分叉。 */
        bool is_long = false;
        if (*p == 'l') {
            is_long = true;
            p++;
        }

        switch (*p) {
            case 'd': {
                int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                print_int(v);
                break;
            }
            case 'u': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
                print_uint(v, 10, false);
                break;
            }
            case 'x': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
                print_uint(v, 16, false);
                break;
            }
            case 'X': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
                print_uint(v, 16, true);
                break;
            }
            case 'p': {
                uintptr_t v = va_arg(ap, uintptr_t);
                console_puts("0x");
                print_uint(v, 16, false);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                console_puts(s ? s : "(null)");
                break;
            }
            case 'c': {
                int c = va_arg(ap, int);
                console_putc((char)c);
                break;
            }
            case '%':
                console_putc('%');
                break;
            case '\0':
                /* 格式串以裸 '%' 结尾，没有后续字符，直接结束不再多读一个字符 */
                va_end(ap);
                return;
            default:
                console_putc('%');
                console_putc(*p);
                break;
        }
    }

    va_end(ap);
}
