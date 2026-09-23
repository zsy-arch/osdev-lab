/* Lab8：文件系统层对外的接口。
 *
 * 这一层之下是 blk.h（只有 blk_init/blk_read 两个函数，把 ATA PIO 和
 * virtio-blk 的差异全部吸收掉）；这一层之上是 trap.c 里的 sys_open/
 * sys_read/sys_close（把 inode 号包装成进程私有的文件描述符）。
 *
 * 接口里一律用 inode 号（uint32_t inum）而不是某种 "struct inode *"
 * 句柄，是刻意的：本 Lab 的文件系统是只读的，没有 in-core inode 缓存、
 * 没有引用计数、没有锁，"打开一个文件"这件事不需要在内核里分配任何
 * 长期存在的对象——inode 号本身就是一个完备的、无需管理生命周期的句柄,
 * 每次用到时按号从磁盘读一次即可。真实内核必须有 in-core inode（否则
 * 每次 read 都要多读一次 inode 块，而且没有地方挂锁和脏标记），那一层
 * 是写路径出现之后才真正必要的，见 README 的简化清单。
 */
#ifndef OSDEV_FS_H
#define OSDEV_FS_H

#include "types.h"

/* 挂载：读超级块、校验魔数、做一次位图自检。失败直接 panic()——
 * 见 blk.h 里关于"为什么磁盘错误不往上报错误码"的说明。 */
void fs_init(void);

/* 在根目录里按名字查找，返回 inode 号；找不到返回 0。
 *
 * 返回 0 表示"没找到"而不是用 -1：inode 号 0 在格式层面就永久保留作
 * "无效"（见 fs_format.h 里 ROOTINO 的注释），所以 0 是一个天然的、
 * 不会跟任何合法 inode 号冲突的失败值，不需要额外约定一个负数。
 *
 * 只查根目录、不解析路径：本 Lab 的镜像是扁平的，没有子目录。"解析
 * a/b/c 这样的路径"是在这个函数外面套一层循环（每一级用上一级返回的
 * 目录 inode 继续查），机制上不新增任何东西，留作挑战任务。 */
uint32_t fs_lookup(const char *name);

/* 返回 inode 号对应文件的字节数。inum 无效时 panic()。 */
uint32_t fs_size(uint32_t inum);

/* 从文件 inum 的偏移 off 处读最多 n 字节到 dst，返回实际读到的字节数。
 *
 * 返回值可能小于 n：读到文件末尾就停。off 已经在文件末尾或之后时返回
 * 0——这是 POSIX read(2) 表示 EOF 的方式（不是错误，是"没有更多数据"），
 * 用户程序据此判断"读完了"，见 user_prog.S 的读取循环。 */
uint32_t fs_read(uint32_t inum, uint32_t off, void *dst, uint32_t n);

#endif /* OSDEV_FS_H */
