/* Lab9：极简 libc——用户程序能用的全部东西就是这个文件里声明的这些。
 *
 * 分两部分：
 *   - 系统调用包装：一个 C 函数名对应一条陷入内核的指令序列。实现不在
 *     .c 里，在 usys_x86_64.S / usys_riscv64.S——这是本 Lab 唯一必须
 *     按架构分开写的用户态代码。
 *   - 纯用户态的工具函数：strlen/memcpy/fputs 之类，实现在 ulib.c 里，
 *     两个架构共用同一份源码。
 *
 * 这个分界就是 libc 的本质分界。真实的 glibc 有几百万行，但它的"必须
 * 按架构写汇编"的部分（sysdeps/unix/sysv/linux/<arch>/）相对很小，绝大
 * 部分是架构无关的 C。本 Lab 的比例夸张地体现了这一点：几十行汇编 +
 * 一百多行 C。
 *
 * 为什么用户程序不能直接用内核的 src/common/string.c（里面已经有一份
 * memcpy/strlen 了）：因为那是*内核的*代码，编译进内核的二进制、跑在
 * 内核地址空间。用户程序是独立的 ELF，链接时只链自己的 .o，拿不到内核
 * 里的符号——它们运行在不同的地址空间，内核那份 memcpy 的地址在用户
 * 页表里根本没有映射。"同一个函数要在两边各有一份实现"不是重复劳动，
 * 是两个独立程序的必然结果。这一点想清楚了，"用户态 libc"为什么必须
 * 存在也就清楚了。
 */
#ifndef OSDEV_USER_H
#define OSDEV_USER_H

#include "types.h"

/* ------------------------------------------------------------------ */
/* 系统调用包装（实现在 usys_<arch>.S）                                */
/* ------------------------------------------------------------------ */

/* 往 fd 写 n 字节，返回实际写入的字节数，出错返回负数。
 *
 * fd 0/1/2 在进程创建时就被接到控制台上（见内核 proc.c），所以
 * write(1, ...) 能直接打印——但 fd 1 不是"控制台"的同义词，它只是
 * fd 表里的第 1 项，shell 可以把它换成管道或文件（见 syscall.h 里
 * SYS_DUP 的注释）。用户程序写 write(1, ...) 时不需要知道它通向哪里,
 * 这正是 fd 抽象要达到的效果。 */
int write(int fd, const void *buf, int n);

/* 结束当前进程。不返回——标上 noreturn 让编译器知道这一点，否则
 * main() 末尾会被编译器补上一段永远执行不到的返回序列，而且
 * "exit 之后还有代码"这种真正的错误也不会有警告。 */
void exit(int code) __attribute__((noreturn));

/* 创建子进程。父进程里返回子进程 pid，子进程里返回 0，失败返回负数。
 * "一次调用两次返回"是 fork 最反直觉的地方，见 Lab7。 */
int fork(void);

/* 用 path 指向的可执行文件替换当前进程的地址空间。
 *
 * 成功时*不返回*——当前进程的代码和数据都已经被换掉了，没有"返回点"
 * 可言。只有失败时才返回（负数），所以 exec 之后紧跟一行错误处理是
 * 正确的写法，不是多余的防御：
 *
 *     exec(path, argv);
 *     puts("exec failed\n");   // 只有失败才会执行到这里
 *     exit(1);
 *
 * argv 是一个以 NULL 结尾的字符串指针数组，argv[0] 按惯例是程序名。
 * 内核会把它拷到新地址空间的栈上，然后让新程序的 _start 从栈上取出
 * argc/argv——见 exec.c 和 usys_<arch>.S 里关于栈布局的说明。 */
int exec(const char *path, char **argv);

/* 等待任意一个子进程结束，返回它的 pid；没有子进程时返回负数。
 *
 * 本 Lab 的 wait 是*阻塞*的（Lab7 那版会在没有子进程结束时立刻返回）,
 * 因为 shell 必须等命令跑完才能打下一个提示符。拿不到退出码是本 Lab
 * 的简化：POSIX 的 wait(&status) 通过指针回传，加上它需要内核在进程
 * 结构里多存一个字段，见 README 的简化清单。 */
int wait(void);

/* 按名字打开文件，返回 fd；失败返回负数。
 *
 * 没有 flags 参数（POSIX 是 open(path, O_RDONLY|...)）：本 Lab 的文件
 * 系统是只读的，唯一合法的 flags 就是 O_RDONLY，一个恒定值的参数不如
 * 不要。目录也能打开——ls 就是靠 open(".") + read() 直接读出目录里的
 * dirent 字节，见 ls.c。 */
int open(const char *path);

/* 从 fd 读最多 n 字节。返回实际读到的字节数；返回 0 表示 EOF。
 *
 * 三种 fd 的 EOF 语义不一样，这是 read 最容易踩的地方：
 *   - 文件：读到 inode.size 就返回 0，确定且可重复。
 *   - 管道：只有当所有写端都关闭*且*管道里没有剩余数据时才返回 0。
 *     写端没关而管道空着的时候，read 会阻塞等待，不会返回 0——否则
 *     `cat f | grep x` 里的 grep 会在 cat 还没来得及写第一个字节时
 *     就以为读完了。
 *   - 控制台：永远不返回 0。串口没有"文件结束"这个概念，没人敲键盘时
 *     read 就一直阻塞。这一点在自动化测试里很要紧：交互式 shell 读
 *     控制台会永久阻塞，而不是读到 EOF 后疯狂重试。 */
int read(int fd, void *buf, int n);

/* 关闭 fd。管道的引用计数在这里递减——写端全关了，读端才会读到 EOF。 */
int close(int fd);

/* 创建管道。fds[0] 是读端，fds[1] 是写端。成功返回 0。
 *
 * 为什么是"传进来一个数组"而不是"返回两个 fd"：C 函数只能返回一个值。
 * POSIX 的选择是通过指针回传两个，本 Lab 照抄。 */
int pipe(int fds[2]);

/* 复制 fd 到当前进程*最小*的空闲 fd 号上，返回那个新号。
 * "最小"这条语义是 shell 做重定向的基础，见 syscall.h 里 SYS_DUP。 */
int dup(int fd);

/* ------------------------------------------------------------------ */
/* 纯用户态工具函数（实现在 ulib.c）                                   */
/* ------------------------------------------------------------------ */

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);

/* 往 fd 写一个以 '\0' 结尾的字符串（不自动加换行）。 */
void fputs(int fd, const char *s);

/* fputs(1, s) 的简写。名字跟标准库的 puts 一样，但行为有一处不同：
 * 标准库的 puts 会自动补一个换行，这里不补。
 *
 * 刻意不补的理由：自动补换行让"拼接输出"变得别扭（echo 要在多个参数
 * 之间打空格、只在最后打一个换行），而且本 Lab 的 cat/grep 是按块转发
 * 字节的，任何"自动加东西"的输出函数都用不上。名字保持 puts 是为了
 * 眼熟，但这个差异值得记住——它正是"极简 libc"和真 libc 的距离。 */
void puts(const char *s);

/* 往 fd 写一个十进制整数（支持负数）。ls 用它打印文件大小。 */
void fputd(int fd, int v);

#endif /* OSDEV_USER_H */
