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

/* TODO 1：实现 pipe_occupancy(pi)。
 *
 * 返回值：这个管道当前缓冲区里有多少字节数据。
 *
 * 就一行：pi->nwrite - pi->nread。整段计算的正确性依赖"无符号回绕"那个
 * 论证——pipe.h 里 struct pipe 的注释已经完整讲过为什么这个减法在
 * nwrite/nread 溢出之后依然成立，这里不重复。
 *
 * 单独拎成一个函数而不是到处写 pi->nwrite - pi->nread，是因为下面
 * pipe_read/pipe_write 各有两处要用到它，写成函数确保这份"减法为什么对"
 * 的论证只需要在一个地方成立。取名 occupancy 而不是 count/size，是为了
 * 跟"缓冲区容量"（PIPEBUF）区分开。 */
static uint32_t pipe_occupancy(const struct pipe *pi)
{
    (void)pi;
    return 0;
}

/* TODO 2：实现 pipe_alloc()。
 *
 * 返回值：一个可用的管道，字段已经初始化好；没有空闲槽位时返回 NULL。
 *
 * 步骤：
 *   1. 遍历 pipe_table[NPIPE]，找一个 inuse == 0 的槽位。
 *   2. 显式把 nread/nwrite 清零，把 nread_open/nwrite_open 都设成 1。
 *   3. 最后才把 inuse 设成 1，返回这个槽位的地址。
 *   4. 遍历完都没找到空闲槽位，返回 NULL。
 *
 * 步骤 2 的"显式清零"不能省：这个槽位可能是上一个管道用过又被回收的,
 * nread/nwrite 停在上一轮结束时的值上。不清零的后果很具体——occupancy
 * 一上来就不是 0，新管道凭空"含有"上一个管道残留的字节数，第一次 read
 * 会从 data[] 里捞出陈旧数据。
 *
 * 步骤 3 的顺序（先填好内容、最后才宣布可用）单核关中断下其实无所谓,
 * 但养成这个习惯是值得的——它在多核上是必须的（否则另一个 CPU 可能看到
 * inuse=1 而字段还没写完）。 */
struct pipe *pipe_alloc(void)
{
    return NULL;
}

/* TODO 3：实现 pipe_read(pi, dst, n)。
 *
 * 返回值：实际读到的字节数（可能小于 n），或者写端已全关且缓冲区已空时
 * 返回 0（EOF）。pi 无效（NULL 或 !inuse）要 panic——见函数体已有的检查。
 *
 * 核心是一个等待循环 + 一次搬运：
 *
 *   1. `while (pipe_occupancy(pi) == 0) { ... }`：
 *        - 循环体内先查 pi->nwrite_open == 0，是的话直接 return 0（EOF）。
 *        - 否则调用 yield()，让出 CPU，醒来后回到 while 重新判断。
 *      两个条件的检查顺序不重要（都是纯读取），但必须每次醒来都重新查——
 *      这就是为什么它是 while 而不是 if。pipe.h 顶部模块注释和这里都在
 *      说同一件事：为什么 yield() 忙等在本 Lab 里足够、以及它的代价是
 *      什么，不在这里重复展开。
 *
 *   2. 循环退出时 occupancy > 0，说明有数据。计算 want = min(n, occupancy)，
 *      然后一个字节一个字节地从 `pi->data[pi->nread % PIPEBUF]` 搬到
 *      dst[i]，每搬一个字节 pi->nread++。搬完 want 个字节后返回 want。
 *
 * 逐字节搬运（而不是先算出连续段长度再 memcpy）是故意的：数据在环形
 * 缓冲区里可能跨过尾部回到头部，那样需要两次 memcpy 加边界判断，而
 * 本 Lab 的调用者（sh.c 的 readline、ulib）每次也只读 1 字节，优化没有
 * 实际收益。 */
int pipe_read(struct pipe *pi, char *dst, uint32_t n)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_read: 管道无效——fd 表里存了一个已经被回收的管道");
    }

    (void)dst;
    (void)n;
    return -1;
}

/* TODO 4：实现 pipe_write(pi, src, n)。
 *
 * 返回值：成功写完全部 n 字节返回 n；读端已全关（不管是开始时还是写到
 * 一半）返回 -1。pi 无效要 panic——见函数体已有的检查。
 *
 * 对 src 里的每个字节 i（从 0 到 n-1）依次做：
 *
 *   1. 先查 pi->nread_open == 0——是的话直接 return -1。这一步覆盖两种
 *      情况：一开始就没有读端，以及写到一半读端跑了（`yes | head -1`
 *      场景）。已经写进去的 i 个字节就这么丢了，这是本 Lab 故意接受的
 *      语义缺口（没有 SIGPIPE 机制），README 的"已知限制"里有记录，不用
 *      在这里另外处理。
 *
 *   2. `while (pipe_occupancy(pi) == PIPEBUF) { ... }`：缓冲区满了就等。
 *      循环体内同样要查 pi->nread_open == 0（是的话 return -1）、否则
 *      yield()。这个检查看似和第 1 步重复，但覆盖的是不同的失败窗口——
 *      一个满的管道遇上读端退出时 occupancy 会永远停在 PIPEBUF，循环
 *      出不去，第 1 步的检查再也执行不到，等待循环必须自己有退出条件。
 *
 *   3. 循环退出后 occupancy < PIPEBUF，写入 `pi->data[pi->nwrite % PIPEBUF]
 *      = src[i]`，然后 pi->nwrite++。
 *
 * 全部 n 个字节写完后返回 n。注意本函数不做"短写"——POSIX 允许返回小于
 * n 的正数，但本 Lab 的 ulib 不检查返回值，短写会让输出被静默截断，
 * 所以这里选择"要么全部写完，要么返回 -1"，把复杂度留在内核而不是甩给
 * 用户程序。 */
int pipe_write(struct pipe *pi, const char *src, uint32_t n)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_write: 管道无效——fd 表里存了一个已经被回收的管道");
    }

    (void)src;
    (void)n;
    return -1;
}

/* TODO 5：实现 pipe_close(pi, writable)。
 *
 * 参数：writable 非 0 表示关的是写端，为 0 表示关的是读端。pi 无效、或者
 * 对应那一侧的计数已经是 0（同一个 fd 被关了两次）都要 panic——见函数体
 * 已有的检查框架，把计数递减的两行填进对应分支即可。
 *
 * 步骤：
 *   1. writable 非 0：pi->nwrite_open--；否则：pi->nread_open--。
 *   2. 递减之后，如果 nread_open == 0 且 nwrite_open == 0，说明两端都没人
 *      用了，把 pi->inuse 置 0，回收这个槽位。
 *
 * 回收时缓冲区里可能还剩着字节——直接丢，不用管。这跟"写端关了但读端还
 * 在"的情况（pipe_read 里 EOF 判断的那一支）截然不同：这里是"没有任何 fd
 * 指着这个管道"，剩下的字节已经不可达。分不清这两种情况（"引用计数到 0"
 * 和"某一端到 0"）会在 `echo hi | cat` 这种"写端早退"场景下丢数据。
 *
 * 本函数不碰任何 fd 表——调用者（sys_close / sys_exit_proc）负责把自己
 * 那个表项清成 FD_NONE，这里只管理 struct pipe 自己的引用计数。 */
void pipe_close(struct pipe *pi, int writable)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_close: 管道无效——同一个 fd 被关了两次？");
    }

    (void)writable;
}

/* TODO 6：实现 pipe_dup(pi, writable)。
 *
 * 参数：writable 非 0 表示多了一个 fd 指向写端，为 0 表示指向读端。
 * pi 无效要 panic——见函数体已有的检查。
 *
 * 就一行：writable 非 0 时 pi->nwrite_open++，否则 pi->nread_open++。
 *
 * 两个调用点，都在 proc.c 里：sys_dup（用户程序显式复制一个 fd）和
 * sys_fork（子进程继承父进程整张 fd 表）。fork 那一处最容易漏——
 * struct proc 的结构体赋值会把 pipe 指针一起拷过去，代码看起来"已经
 * 对了"，但如果忘了对每个指向管道的 fd 调一次 pipe_dup()，计数就没加,
 * 子进程一 close 就会把计数减到 0，父进程手上那一端跟着失效。proc.c 里
 * fork 的 fd 表拷贝那段有对应的提醒。 */
void pipe_dup(struct pipe *pi, int writable)
{
    if (pi == NULL || !pi->inuse) {
        panic("pipe_dup: 管道无效");
    }

    (void)writable;
}
