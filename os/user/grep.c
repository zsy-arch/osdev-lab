/* Lab9：grep —— 输出包含指定子串的行。
 *
 * 这个程序在本 Lab 里的角色是"管道的下游"。`cat motd.txt | grep the` 这条
 * 命令要成立，grep 必须：
 *
 *   - 在没有文件名参数时从 fd 0 读（否则管道接不上）
 *   - 能处理"一次 read 返回的数据里有好几行"和"一行被切成两次 read"
 *     两种情况（管道和文件都会这样）
 *
 * 第二点是本文件的全部难度所在。read 给的是字节流，不是行；"行"这个概念
 * 完全由用户程序自己从字节流里切出来。这是 Unix I/O 最基础的一条性质,
 * 而它是 libc 里 fgets/getline 存在的唯一理由——那些函数做的就是这里手写
 * 的行缓冲。
 *
 * 我们只做固定子串匹配，不做正则
 * ----------------------------
 * 真正的 grep 是 global regular expression print，g/re/p 来自 ed 编辑器的
 * 命令。正则引擎（哪怕只支持 . * ^ $）是一个独立的话题，跟操作系统无关,
 * 而且一个够用的实现会比本文件其余部分加起来还长。固定子串匹配已经足够
 * 让管道的作用显现出来，这是本 Lab 关心的部分。
 *
 * 真实世界里固定子串也是常用的：grep -F（fgrep）就是这个模式，而且因为
 * 不用编译正则，它比默认模式快。
 */
#include "user.h"

/* 行缓冲区。
 *
 * 512 字节是一个够用但确实有上限的选择，而这个上限有可观察的后果：超过
 * 511 字节的行会被截断成两行输出。真实的 grep 用动态增长的缓冲区（getline
 * 会 realloc），我们没有 malloc，所以只能定长。
 *
 * 关键是"超长"这件事必须被明确处理，而不是让它变成缓冲区溢出。下面的
 * flush 逻辑在缓冲区满时强制当成一行处理掉，是刻意的选择：宁可输出被
 * 切断（可见的、局部的错误），也不要写出缓冲区（不可见的、会破坏无关
 * 数据的错误）。
 *
 * 放在 .bss 的理由跟 cat.c 里的 buf 一样：用户栈只有一个页。这里有两个
 * 512 字节的缓冲区，加起来占掉用户栈的四分之一——这正是不能放栈上的
 * 那类大小。 */
static char line[512];
static int  linelen;

static char rbuf[512];

/* 判断 hay 里是否含有子串 needle。
 *
 * 朴素算法：对 hay 的每个起始位置试着匹配一遍。最坏情况 O(n*m)。
 * 不用 KMP 之类的线性算法，理由跟不做正则一样——这不是本 Lab 的主题,
 * 而朴素算法在这里的实际表现完全够（行长 512、模式串通常几个字符）。
 *
 * 一个容易写错的边界：needle 为空串时应该匹配任何 hay（包括空的 hay）。
 * 下面的写法天然满足：外层 i 从 0 开始，内层循环一次都不执行，j 直接
 * 等于 0 == needle 的长度，立刻返回 1。空模式串匹配一切也是真实 grep
 * 的行为（`grep ""` 输出所有行）。 */
static int contains(const char *hay, const char *needle)
{
    /* 空模式串匹配一切，包括空的 hay。单独特判，而不是指望主循环碰巧
     * 处理对——把"空串"塞进主循环的条件里能写出来，但那个条件会变成
     * `hay[i] != '\0' || needle[0] == '\0'` 这种要盯着看一会儿才能确认
     * 的形状。边界条件用一行显式的 return 表达，比让它藏在循环条件里好。
     *
     * 空模式匹配一切也是真实 grep 的行为（`grep ""` 输出所有行）。 */
    if (needle[0] == '\0') {
        return 1;
    }

    for (int i = 0; hay[i] != '\0'; i++) {
        int j = 0;
        while (needle[j] != '\0' && hay[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return 1;   /* 整个 needle 都匹配上了 */
        }
    }
    return 0;
}

/* 上面内层循环读 hay[i + j] 时，i + j 会不会越过 hay 的末尾？不会，而
 * 原因值得说清楚，因为它是 C 字符串处理里少见的"边界自动成立"：
 *
 * 循环条件是 `needle[j] != '\0' && hay[i + j] == needle[j]`。假设
 * hay[i + j] 正好是终止符，那么它要等于 needle[j] 才能继续——而此时
 * needle[j] 已经被前半个条件保证不是 '\0'，所以相等不可能成立，循环
 * 在这里停下。也就是说 hay 的 '\0' 自动充当了哨兵，读到它就一定终止,
 * 绝不会读到它之后。
 *
 * 成立的前提是"needle 里不可能含 '\0'"——这在 C 字符串里是定义使然。
 * 换成带长度的字节串（可以含 0 字节）这个保证立刻失效，就必须显式检查
 * 边界了。 */

/* 处理攒好的一行。line 里此时是不含 '\n' 的内容，不保证有终止符——
 * 由调用方在调用前补好。 */
static void emit_if_match(const char *pattern)
{
    if (contains(line, pattern)) {
        write(1, line, linelen);
        write(1, "\n", 1);
    }
    linelen = 0;
}

/* 把一个 fd 读完，逐行匹配。 */
static int grep_fd(int fd, const char *pattern)
{
    int n;

    while ((n = read(fd, rbuf, sizeof(rbuf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (rbuf[i] == '\n') {
                /* 一行结束。补终止符再交给匹配函数——line 里攒的是裸
                 * 字节，contains 要的是 C 字符串。
                 *
                 * linelen 最大是 sizeof(line) - 1（见下面的满缓冲处理），
                 * 所以 line[linelen] 这个下标始终合法。这个不变量是靠
                 * 下面那个 if 维持的，不是凑巧。 */
                line[linelen] = '\0';
                emit_if_match(pattern);
                continue;
            }

            line[linelen++] = rbuf[i];

            /* 缓冲区满了但还没遇到 '\n'：强制当一行处理掉。
             *
             * 注意判断条件是 >= sizeof(line) - 1，留出最后一格给终止符。
             * 写成 >= sizeof(line) 就会在下一行的 line[linelen] = '\0'
             * 时越界一个字节——经典的 off-by-one，而且因为只越界一个字节、
             * 越过去的又是 .bss 里紧跟着的变量，症状可能是"某个无关的
             * 全局变量偶尔变成 0"。
             *
             * 这类 bug 在本课程前面的 Lab 里出现过同形的一次（Lab7 的
             * 共享内核栈问题），共同点是：破坏发生在正确的代码看起来
             * 完全无关的地方。定长缓冲区 + 手写索引的组合几乎总是要在
             * 这里付一次代价，所以条件要写得能一眼核对。 */
            if (linelen >= (int)sizeof(line) - 1) {
                line[linelen] = '\0';
                emit_if_match(pattern);
            }
        }
    }

    if (n < 0) {
        fputs(2, "grep: read error\n");
        return -1;
    }

    /* EOF 时缓冲区里可能还攒着没有换行符结尾的最后一行。
     *
     * 文本文件按惯例以 '\n' 结尾，所以这段代码在正常文件上不会执行。但
     * 它必须存在，因为：
     *
     *   - 管道的上游可能没有以换行结尾（`echo -n` 那种，或者我们的
     *     某个程序写了半行就退出）。
     *   - 用户可以手工造出没有末尾换行的文件。
     *
     * 漏掉这一段的症状是"最后一行匹配了却没输出"，而且只在没有末尾换行
     * 的输入上出现——测试数据规规矩矩的话永远发现不了。这就是
     * fsroot/exact.txt 那个文件存在的一部分理由（见它的内容）。 */
    if (linelen > 0) {
        line[linelen] = '\0';
        emit_if_match(pattern);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        /* 用法信息写 fd 2。跟 cat 里一样，现在看不出区别，但 `grep |
         * something` 的时候这条信息应该出现在屏幕上而不是流进管道。 */
        fputs(2, "usage: grep pattern [file ...]\n");
        return 1;
    }

    const char *pattern = argv[1];

    /* 没给文件名：从标准输入读。管道的下游走的就是这条路径。 */
    if (argc == 2) {
        return grep_fd(0, pattern) < 0 ? 1 : 0;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        int fd = open(argv[i]);
        if (fd < 0) {
            fputs(2, "grep: cannot open ");
            fputs(2, argv[i]);
            fputs(2, "\n");
            rc = 1;
            continue;
        }
        /* 每个文件开始前把行缓冲清空。
         *
         * 上一个文件如果以不完整的行结尾，那半行已经在 EOF 时被 flush
         * 掉了，所以 linelen 此时应该是 0。显式清零是防御性的：它让
         * grep_fd 的行为不依赖调用顺序。
         *
         * 真实的 grep 在这里还会输出 "文件名:" 前缀（多文件时），我们
         * 不做，因为那会让管道场景和多文件场景的输出格式不一致，而
         * 自动化测试要比对精确的字节。 */
        linelen = 0;
        if (grep_fd(fd, pattern) < 0) {
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
