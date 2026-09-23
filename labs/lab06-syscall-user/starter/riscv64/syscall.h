/* Lab6：系统调用号约定，与 x86_64 一侧内容完全相同——见
 * ../x86_64/syscall.h 顶部注释，为什么两边各放一份而不是共用
 * src/common 里的一份文件（分发机制完全不同，只有调用号数值本身是
 * 可以共用的约定）。数值必须跟 x86_64 那份保持一致，因为 user_prog.S
 * （riscv64 版）里同样要手写字面量 1/2，不能 #include 这个头
 * ——独立编译单元的约束和 x86_64 侧完全一样。 */
#ifndef OSDEV_SYSCALL_H
#define OSDEV_SYSCALL_H

#define SYS_WRITE 1
#define SYS_EXIT  2

#endif /* OSDEV_SYSCALL_H */
