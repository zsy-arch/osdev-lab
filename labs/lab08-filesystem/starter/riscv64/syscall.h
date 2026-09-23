/* Lab6：系统调用号约定，与 x86_64 一侧内容完全相同——见
 * ../x86_64/syscall.h 顶部注释，为什么两边各放一份而不是共用
 * src/common 里的一份文件（分发机制完全不同，只有调用号数值本身是
 * 可以共用的约定）。数值必须跟 x86_64 那份保持一致，因为 user_prog.S
 * （riscv64 版）里同样要手写字面量，不能 #include 这个头
 * ——独立编译单元的约束和 x86_64 侧完全一样。
 *
 * Lab7 新增三个号：fork/exec/wait，跟 x86_64 版本同一个理由（见那边
 * 头文件注释）——没有 fork，proc_table 里永远只有 kernel_main.c 创建
 * 的那一个初始进程，看不出"进程"和"调度"这两个概念在动态创建/销毁
 * 场景下的样子。
 *
 * Lab8 新增三个号：open/read/close。为什么是这三个、而不是一个
 * "read_file(名字, 偏移, 缓冲区, 长度)"就够了，见 x86_64 那份头文件里的
 * 完整展开——核心是本 Lab 要亲手建立的正是*文件描述符*这个抽象：open()
 * 把"名字 -> inode"的解析和"当前读到哪里"的状态一次性搬进内核，交给用户
 * 程序的只是一个小整数。这个设计决定跟架构完全无关，两边逐字相同。 */
#ifndef OSDEV_SYSCALL_H
#define OSDEV_SYSCALL_H

#define SYS_WRITE 1
#define SYS_EXIT  2
#define SYS_FORK  3
#define SYS_EXEC  4
#define SYS_WAIT  5
#define SYS_OPEN  6
#define SYS_READ  7
#define SYS_CLOSE 8

#endif /* OSDEV_SYSCALL_H */
