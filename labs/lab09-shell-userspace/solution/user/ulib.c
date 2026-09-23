/* Lab9：用户态工具函数——libc 里架构无关的那一半。
 *
 * 这个文件和 src/common/string.c 的内容高度相似，但它们是两个不同程序
 * 里的两份独立代码，不是重复。原因见 user.h 开头的说明。
 *
 * 为什么 memset/memcpy 必须存在，即使没人显式调用它们
 * --------------------------------------------------
 * 这一点是本文件最反直觉的地方，而且可以当场验证。看这段代码：
 *
 *     struct big { long v[64]; };
 *     struct big g1, g2;
 *     void copy_struct(void)  { g1 = g2; }
 *     void clear_struct(void) { struct big z = {0}; g1 = z; }
 *
 * 用本课程给用户程序的那套 flag 编译（-ffreestanding -O2 …），反汇编:
 *
 *     $ x86_64-elf-objdump -dr sc.o | grep -E 'call|R_X86_64_PLT32'
 *       13: e8 00 00 00 00   call ...
 *           14: R_X86_64_PLT32  memcpy-0x4
 *       30: e8 00 00 00 00   call ...
 *           31: R_X86_64_PLT32  memset-0x4
 *
 * 源码里一个 memcpy 都没写，目标文件里却有两个对它的未定义引用。原因是
 * C 语言的结构体赋值被允许编译成任何等效的实现，而"任何等效实现"里最
 * 划算的那个就是调库函数。
 *
 * 关键是 -ffreestanding 并*不*阻止这件事。这个 flag 的含义常被误解成
 * "编译器不再依赖标准库"，实际它只表示"没有宿主环境，不假定 main 之前
 * 有 libc 初始化，只保证 freestanding 头文件可用"。C 标准明确规定,
 * 即使在 freestanding 实现里，编译器仍然可以为内存拷贝/清零生成对
 * memcpy/memset/memmove/memcmp 的调用——这四个函数是 freestanding
 * 环境也必须自己提供的。
 *
 * 所以 ulib.c 里的 memset/memcpy 有双重身份：既是给程序调用的库函数,
 * 也是编译器的隐式依赖。链接时少了它们，报的错是
 * "undefined reference to `memcpy'"，指向一行只写了 a = b 的代码——
 * 第一次见到很难反应过来。
 *
 * 关于 -fno-tree-loop-distribute-patterns
 * --------------------------------------
 * 这个 flag 加在用户程序的编译命令里（见 Makefile 的 UCFLAGS），属于
 * 预防性措施，不是在修当前存在的 bug。它值得单独说明，因为它是自举代码
 * 一类风险的代表。
 *
 * GCC 的 loop distribution pass 会识别"循环在做的事等于一次库函数调用"
 * 并替换掉。它确实会这么干，可以验证（用 x86_64-elf-gcc 16.2.0）:
 *
 *     int a[100][100];
 *     void zero_nest(int n, int m) {
 *         for (int i = 0; i < n; i++) for (int j = 0; j < m; j++) a[i][j] = 0;
 *     }
 *
 *     $ x86_64-elf-gcc -m64 -O3 -c ld1.c && objdump -dr ld1.o
 *       38: call ...
 *           39: R_X86_64_PLT32  memset-0x4        ← 内层循环变成了 memset
 *     $ x86_64-elf-gcc -m64 -O3 -fno-tree-loop-distribute-patterns -c ld1.c
 *       （没有任何 call）
 *
 * 危险在于：如果这个替换发生在 memset *自己*的循环体上，memset 的第一条
 * 指令就成了 call memset，无穷递归直到栈溢出。这是自举代码的经典形状
 * ——优化依据的是标准库函数的语义，而你正在*实现*那个函数。
 *
 * 诚实地说：本课程这套 flag 下它不会发生。实测 GCC 16.2.0 对上面 memset
 * 那种平坦的逐字节循环并不触发这个 pass（改成别的函数名也不触发，加
 * -O3 也不触发），触发它需要嵌套循环那种形状。而且 GCC 内部对
 * "在函数 X 里生成对 X 的调用"本身有防护。
 *
 * 那为什么还要加这个 flag：因为"当前这个编译器版本、这种循环写法碰巧
 * 不触发"是一个会过期的前提，而代价是零（这个 pass 对我们的用户程序
 * 没有任何性能价值）。glibc 和 Linux 内核在实现这类函数的翻译单元上
 * 同样显式关掉它，理由相同。
 *
 * 顺手记下一条更普适的判断：当"代码正确"依赖的是"优化器恰好没做某件它
 * 有权做的事"时，把那件事显式禁掉，而不是依赖它继续不做。
 *
 * 注意：不要用 __builtin_memset/__builtin_memcpy 去实现它们自己，那
 * 会直接变成对库函数的调用（builtin 在无法内联展开时的回退路径就是发出
 * 一次 call），也就是真正的无穷递归。逐字节循环是最保险的写法，性能在
 * 教学场景下无关紧要。
 */
#include "user.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    /* 转成 unsigned char 再相减：char 在 x86_64 上默认有符号、在部分
     * ARM 平台上默认无符号，直接相减会让 >0x7F 的字节比较结果随平台
     * 变化。标准要求按 unsigned char 比较。 */
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void fputs(int fd, const char *s)
{
    /* 一次 write 写完整个字符串，不是逐字符 write。
     *
     * 逐字符也能工作，但每个字符都要陷入内核一次——在管道场景下还会
     * 造成"每个字节唤醒对端一次"。更要紧的是可观察性：如果 echo 逐
     * 字符写管道，而 grep 按行读，那么调试时看到的管道内容是零碎的,
     * 很难判断到底是哪一端出了问题。
     *
     * 这也是为什么真 libc 要有缓冲：系统调用比函数调用贵得多。 */
    int n = (int)strlen(s);
    if (n > 0) {
        write(fd, s, n);
    }
}

void puts(const char *s)
{
    fputs(1, s);
}

void fputd(int fd, int v)
{
    char buf[12];  /* -2147483648 是 11 个字符 + '\0' = 12，刚好够 */
    int  i = 0;

    /* 用 unsigned 来累积绝对值。直接 v = -v 在 v == INT_MIN 时是溢出
     * （有符号溢出是 UB），而 INT_MIN 的绝对值放不进 int。这是十进制
     * 转换函数里最经典的一个边界 bug，绝大多数手写版本都有。 */
    unsigned int u;
    if (v < 0) {
        u = (unsigned int)(-(long long)v);
    } else {
        u = (unsigned int)v;
    }

    /* 低位先出，所以下面要反着写回去。 */
    do {
        buf[i++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);

    if (v < 0) {
        buf[i++] = '-';
    }

    char out[12];
    for (int j = 0; j < i; j++) {
        out[j] = buf[i - 1 - j];
    }
    write(fd, out, i);
}
