/* Lab9：管道的实现。设计上的取舍全部写在 pipe.h 里，这里只讲实现。
 *
 * ── 这个文件在两个架构下逐字节相同 ────────────────────────────────
 *
 * 跟 fs.c / exec.c 一样，x86_64/ 和 riscv64/ 目录下各有一份完全相同的
 * 拷贝，由 `make check-shared-iface` 机械守卫。这一次连那一个 #if 都
 * 没有——管道里没有任何东西跟架构有关：一个字节数组、两个计数器、
 * yield()。这是本 Lab 里架构无关性最彻底的一个文件。
 *
 * 值得停下来想一秒的是*为什么*它能这么干净。管道不碰页表、不碰中断、
 * 不碰特权级，它唯一依赖的内核机制是"让出 CPU 并在之后被唤醒"，而那件事
 * 已经被 yield() 抽象掉了。反过来说，一个模块能不能做到架构无关，取决于
 * 它下面那层接口是不是已经把差异吸干净了——本课程从 Lab4 的 pagetable.h
 * 开始一直在铺这条路，到这里收获。
 */
#include "types.h"
#include "pipe.h"
#include "proc.h"
#include "panic.h"

/* 管道表。
 *
 * 静态数组、编译期定长，跟 proc_table 一样的做法：本课程到现在为止没有
 * 任何"可变大小的内核对象"，一切都是开机时就定下来的。这不只是省事——
 * 它也意味着内核永远不会因为"内存不够分配管道"而失败，失败只有"用完了"
 * 一种，而那是一个确定的、可复现的上限。
 *
 * 8 的来源：sh 每条管道命令建 1 个管道，命令跑完就关。同时存在多个管道
 * 需要多级管道（a | b | c），而本 Lab 的 sh 只支持一级（见 sh.c 的
 * parse()）。所以实际上永远只有 1 个在用，8 是给挑战任务（多级管道）
 * 留的余量。
 *
 * NPIPE 定义在这里、而不是 pipe.h 里，因为外面没人需要知道它：接口是
 * "给我一个管道"（pipe_alloc），不是"给我第几号管道"。这跟 PIPEBUF 的
 * 处境不同——那个值影响 struct pipe 的大小，必须在头文件里。 */
#define NPIPE 8
static struct pipe pipe_table[NPIPE];

/* 当前缓冲区里有多少字节。
 *
 * 单独拎成一个函数而不是到处写 pi->nwrite - pi->nread，是因为这个减法的
 * 正确性依赖"无符号回绕"那个论证（见 pipe.h 里 struct pipe 的注释），
 * 而那个论证只想写一遍。取名 occupancy 而不是 count/size，是为了跟
 * "缓冲区容量"（PIPEBUF）区分开。 */
static uint32_t pipe_occupancy(const struct pipe *pi)
{
    return pi->nwrite - pi->nread;
}

struct pipe *pipe_alloc(void)
{
    for (int i = 0; i < NPIPE; i++) {
        struct pipe *pi = &pipe_table[i];
        if (pi->inuse) {
            continue;
        }

        /* 显式清零，不依赖"静态数组本来就是 0"。
         *
         * 第一次分配确实是 0（.bss），但这个槽位被用过一轮再回收之后就
         * 不是了——nread/nwrite 停在上一个管道结束时的值上。不清零的后果
         * 很具体：occupancy 一上来就不是 0，新管道凭空"含有"上一个管道
         * 残留的字节数，第一次 read 会从 data[] 里捞出陈旧数据。
         *
         * 也可以在 pipe_close() 释放时清，效果一样。选在分配时清，是因为
         * "拿到手的东西一定是干净的"这个不变量只需要看一个地方就能确认,
         * 而释放路径有多个出口（下面 pipe_close 的两个计数分支）。 */
        pi->nread = 0;
        pi->nwrite = 0;
        pi->nread_open = 1;
        pi->nwrite_open = 1;

        /* inuse 最后设置。单核关中断下顺序无所谓，但写成"先把内容准备好,
         * 再宣布它可用"是一个值得养成的习惯——这个顺序在多核上是必须的
         * （否则另一个 CPU 可能看到 inuse=1 而字段还没写完）。 */
        pi->inuse = 1;
        return pi;
    }

    return NULL;
}

int pipe_read(struct pipe *pi, char *dst, uint32_t n)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_read: 管道无效——fd 表里存了一个已经被回收的管道");
    }

    /* 等到"有数据"或者"再也不会有数据"。
     *
     * 循环条件里两个判断的*顺序*不重要（都是纯读取），但两个条件都必须
     * 在每次醒来后重新检查，这就是为什么它是 while 而不是 if + yield。
     *
     * 为什么 yield() 就够了、不需要真正的 sleep/wakeup：yield() 把本进程
     * 标成 RUNNABLE 并切回调度器，调度器接着跑别人；下次轮到本进程时从
     * yield() 后面继续，重新检查条件。代价是这个进程在等待期间会被反复
     * 调度、反复检查、反复让出——纯粹的忙等，浪费的是 CPU 时间而不是
     * 正确性。真实内核在这里会把进程标成 SLEEPING 并挂到这个管道的等待
     * 队列上，由 pipe_write 显式唤醒（README 的挑战任务）。
     *
     * 本 Lab 能忍受忙等，是因为管道两端都是活跃进程，等待时间是"另一个
     * 进程跑到它下一次 write 为止"，通常几百微秒量级。真正不能忍的是
     * 下面 FD_CONSOLE 的读——那要等人打字，可能是几秒，见 trap.c。 */
    while (pipe_occupancy(pi) == 0) {
        if (pi->nwrite_open == 0) {
            /* 写端全关且缓冲区已空：EOF。
             *
             * 这个 return 0 是 `cat /motd.txt | grep lab` 能正常结束的
             * 全部原因：cat 退出 → 它的写端 fd 被关 → sh 也关掉自己手上
             * 那份 → nwrite_open 归零 → grep 的 read 返回 0 → grep 的
             * 读取循环退出 → grep 退出。一条完整的因果链，中间任何一个
             * close 漏掉都会让 grep 永远挂住。 */
            return 0;
        }
        yield();
    }

    /* 有数据了，搬走 min(n, 现有量)。
     *
     * 一个字节一个字节地搬，而不是算出"连续段长度"再 memcpy：数据在环形
     * 缓冲区里可能跨过尾部回到头部，那样需要两次 memcpy 加边界判断。本 Lab
     * 的 read 每次只搬 1 字节（sh.c 的 readline 和 ulib 都是逐字节读），
     * 优化没有意义，而循环少三行边界代码。 */
    uint32_t avail = pipe_occupancy(pi);
    uint32_t want = (n < avail) ? n : avail;

    for (uint32_t i = 0; i < want; i++) {
        dst[i] = pi->data[pi->nread % PIPEBUF];
        pi->nread++;
    }

    return (int)want;
}

int pipe_write(struct pipe *pi, const char *src, uint32_t n)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_write: 管道无效——fd 表里存了一个已经被回收的管道");
    }

    for (uint32_t i = 0; i < n; i++) {
        /* 每写一个字节之前都重新检查读端还在不在。
         *
         * 放在循环*里面*而不是函数开头，是为了同时覆盖两种情况：一开始
         * 就没有读端（写进去没人看，直接报错），以及写到一半读端跑了
         * （`yes | head -1`：head 读够一行就退出，yes 还在写）。后者如果
         * 不检查，yes 会在下面那个 while 里永远等一个再也不会腾出来的
         * 空位——挂死。
         *
         * 已经写进去的 i 个字节就这么丢了，返回值也不告诉调用者写了多少。
         * 这是 xv6 的做法，本 Lab 照抄，理由是没有更好的选择：POSIX 在
         * 这种情况下给写进程发 SIGPIPE（默认动作是杀掉它），而本 Lab 没有
         * 信号机制。返回 -1 之后 ulib 的 fputs 不看返回值，于是 yes 会
         * 接着往下跑、下一次 write 再拿到 -1……直到它自己退出。这是一个
         * 真实的语义缺口，README 的"已知限制"里有记录。 */
        if (pi->nread_open == 0) {
            return -1;
        }

        /* 满了，等读端腾地方。
         *
         * nread_open 的检查在这个 while 里又出现了一次，跟上面那个不是
         * 重复：上面那个只在"有空位、正常往下写"的路径上生效，而一个满的
         * 管道遇上读端退出时 occupancy 会永远停在 PIPEBUF，循环出不去，
         * 外层那个检查再也执行不到。等待循环里必须自己有一份退出条件，
         * 这是所有阻塞循环的通例：每一个能让你停下来的理由，都得在你
         * 停下来的那个循环里被检查。 */
        while (pipe_occupancy(pi) == PIPEBUF) {
            if (pi->nread_open == 0) {
                return -1;
            }
            yield();
        }

        pi->data[pi->nwrite % PIPEBUF] = src[i];
        pi->nwrite++;
    }

    return (int)n;
}

void pipe_close(struct pipe *pi, int writable)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_close: 管道无效——同一个 fd 被关了两次？");
    }

    if (writable) {
        if (pi->nwrite_open == 0) {
            panic("pipe_close: 写端计数已经是 0 了——某处多减了一次");
        }
        pi->nwrite_open--;
    } else {
        if (pi->nread_open == 0) {
            panic("pipe_close: 读端计数已经是 0 了——某处多减了一次");
        }
        pi->nread_open--;
    }

    /* 两端都没人用了，回收槽位。
     *
     * 缓冲区里可能还剩着字节——不管，直接丢。没有任何 fd 指着这个管道，
     * 那些字节再也没人能读到，它们是不可达的。这跟"写端关了但读端还在"
     * 那种情况截然不同：后者缓冲区里的数据必须交付完（pipe_read 先查
     * occupancy 再查 nwrite_open 就是为了这个）。
     *
     * 这两种情况的区别，就是"引用计数到 0"和"某一端到 0"的区别。分不清
     * 这一点，写出来的管道就会在 `echo hi | cat` 这种"写端早退"的场景下
     * 丢数据——而这恰好是管道最常见的用法。 */
    if (pi->nread_open == 0 && pi->nwrite_open == 0) {
        pi->inuse = 0;
    }
}

void pipe_dup(struct pipe *pi, int writable)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_dup: 管道无效");
    }

    if (writable) {
        pi->nwrite_open++;
    } else {
        pi->nread_open++;
    }
}
