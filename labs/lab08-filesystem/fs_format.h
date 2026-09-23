/* Lab8：磁盘上的文件系统格式——mkfs（跑在你开发机上的普通程序）和两个
 * 架构的内核（跑在 QEMU 里的裸机代码）共用这一份定义。
 *
 * 为什么这个文件放在 Lab 根目录，而不是像 proc.h/syscall.h 那样每个架构
 * 目录下各放一份：proc.h 描述的是"某个架构的寄存器现场长什么样"，天然
 * 架构相关；这份文件描述的是"磁盘上的字节怎么排列"，跟 CPU 架构完全
 * 无关——同一个 fs.img 必须能被 x86_64 内核和 riscv64 内核以*完全相同*
 * 的方式解释，否则"文件系统"这个抽象根本不成立。所以它只能有一份，由
 * 三个使用者（mkfs / x86_64 内核 / riscv64 内核）共同 include。
 *
 * 也没有放进 src/common/include/：那个目录是 Lab1-10 全课程共享的公共
 * 头文件，放进去意味着 Lab1-7 的编译命令里也会出现这个路径，而它们跟
 * 文件系统毫无关系。"本 Lab 独有、但本 Lab 内部跨架构共享"的东西，放在
 * Lab 根目录是最准确的位置（各 Makefile 里加一条 -I$(LAB_ROOT) 即可）。
 *
 * 所有多字节整数都按小端序存放。两个目标架构（x86_64、riscv64）都是小端，
 * 开发机（x86_64/arm64 macOS、x86_64 Linux）也都是小端，所以本课程直接
 * memcpy 结构体、不做字节序转换。真实的、要求镜像跨大小端可移植的文件
 * 系统必须在读写时显式转换（参考 Linux ext4 的 le32_to_cpu() 系列宏），
 * 这是本课程明确简化掉的一环——不是忘了，而是在本课程的全部目标平台上
 * 这个转换恒等于空操作，引入它只会让每一次字段访问都多一层噪音。
 */
#ifndef OSDEV_FS_FORMAT_H
#define OSDEV_FS_FORMAT_H

/* mkfs 跑在开发机上，用宿主的标准库；内核跑在裸机上，用本课程自己的
 * types.h。两边都需要 uint32_t 这类定宽整数，但来源不同。
 *
 * __STDC_HOSTED__ 是 C 标准规定的预定义宏：宿主环境（有完整标准库）下
 * 是 1，独立环境（freestanding）下是 0。我们编译内核时一直在用的
 * -ffreestanding 这个 flag，除了"别假设有 libc"之外，正是把这个宏置成
 * 0——所以不需要额外发明一个 -DOSDEV_MKFS 之类的自定义宏来区分两种编译
 * 环境，标准已经提供了准确的判据。这也是一个顺手可验证的小实验：
 *   echo '__STDC_HOSTED__' | gcc -E -ffreestanding -  # 输出 0
 *   echo '__STDC_HOSTED__' | gcc -E -                 # 输出 1
 */
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
#include <stdint.h>
#else
#include "types.h"
#endif

/* 块大小 512 字节：跟本 Lab 两个块设备驱动的硬件扇区大小一致——x86_64
 * 的 ATA PIO 一次读一个 512 字节扇区，riscv64 的 virtio-blk 也以 512
 * 字节为单位寻址。取一致的值，"文件系统的块"和"设备的扇区"就是一一对应
 * 关系，blk_read() 不需要做任何拆分/合并。真实文件系统的块通常是 4KiB
 * （等于页大小，便于直接映射进页缓存），代价是一次文件系统块读写要对应
 * 多个扇区，那层映射本课程不引入。 */
#define BSIZE 512

/* 魔数 "LAB8"（小端存放，所以字节序列是 'L','A','B','8'）。
 *
 * 超级块第一个字段就是魔数，这是文件系统格式的通用做法：挂载时先读这 4
 * 个字节，不匹配就立刻拒绝，而不是硬着头皮把一堆随机字节当 inode 表解析
 * ——后者的表现是"文件系统看起来挂上了，但读出来的每个文件大小都是几个
 * GB、inode 类型是 0x7f3a 这种不存在的值"，比直接报错难查得多。本 Lab
 * 的 fs_init() 会实测这条路径（见 fs.c 里对未格式化磁盘的处理）。 */
#define FS_MAGIC 0x3842414Cu /* 'L'=0x4C 'A'=0x41 'B'=0x42 '8'=0x38 */

/* 磁盘布局（块号）：
 *
 *   块 0        超级块（sb）
 *   块 1        空闲块位图（1 块 = 512 字节 = 4096 位，够描述 4096 个块；
 *               本 Lab 的镜像只有 64 块，只有低 64 位有意义，bit i == 1
 *               表示块 i 已被占用；i >= nblocks 的位是无意义的填充，
 *               恒为 0，见 mkfs.c 的说明）
 *   块 2..3     inode 表（2 块 × 8 个 inode/块 = 16 个 inode）
 *   块 4..      数据块（目录内容、文件内容）
 *
 * 这个顺序不是随便排的：超级块必须在最前面（挂载时唯一一个"不知道布局
 * 也能找到"的块，其余所有块的位置都是从它里面读出来的），位图和 inode
 * 表属于元数据、放在数据区之前，是为了让"元数据区"和"数据区"各自连续，
 * 便于一次性读取/校验。真实文件系统（ext2/xv6）也是这个大结构。
 *
 * 下面这些常量同时被 mkfs 和内核使用，但内核*不*信任它们——内核挂载时
 * 从超级块里读实际值（见 fs.c 的 fs_init()），这些宏只用于 mkfs 生成
 * 镜像、以及内核侧的静态断言。理由：如果内核硬编码布局、只把超级块当
 * 魔数校验用，那超级块里的 inodestart/datastart 这些字段就是死字段,
 * "自描述的元数据"这个文件系统设计要点就体现不出来了。 */
#define FS_NBLOCKS  64  /* 整个镜像 64 × 512B = 32KiB */
#define FS_NINODES  16
#define FS_BITMAPSTART 1
#define FS_INODESTART  2
#define FS_DATASTART   4

struct superblock {
    uint32_t magic;       /* 必须等于 FS_MAGIC */
    uint32_t nblocks;     /* 整个文件系统的总块数 */
    uint32_t ninodes;     /* inode 总数 */
    uint32_t bitmapstart; /* 空闲块位图的起始块号 */
    uint32_t inodestart;  /* inode 表的起始块号 */
    uint32_t datastart;   /* 数据区的起始块号 */
};

/* inode 类型。0 表示这个 inode 槽位未被使用——跟 dirent.inum==0 表示
 * "空目录项"是同一套"0 即无效"的约定（见下面 ROOTINO 的注释）。 */
#define T_DIR  1
#define T_FILE 2

/* 一个 inode 最多直接指向 8 个数据块 => 最大文件 8 × 512B = 4096 字节。
 *
 * 真实文件系统在直接块用完之后还有一级/二级/三级间接块（xv6 有一级，
 * ext2 有三级），让单文件上限从几 KiB 涨到几 GiB。本 Lab 只做直接块：
 * 间接块的实现是"再多一次 blk_read()、把那一块当成 uint32_t 数组"，
 * 概念上并不新——新的是"文件偏移 -> 块号"这个映射从一次查表变成了
 * 多级查表，而本 Lab 的重点是把"块设备 -> inode -> 目录 -> 文件描述符"
 * 这条完整链路第一次打通。间接块留作挑战任务（见 README）。 */
#define NDIRECT 8

/* 磁盘上的 inode。dinode 的 d 是 "disk" ——强调这是*磁盘上*的格式，
 * 跟内存里的运行时表示（真实内核会有一份带锁、带引用计数、带脏标记的
 * in-core inode）是两个不同的结构。本 Lab 的文件系统是只读的，不需要
 * in-core inode 那些字段，所以只有这一份 dinode，读出来直接用。 */
struct dinode {
    uint16_t type;              /* T_DIR / T_FILE，0=未使用 */
    uint16_t nlink;             /* 硬链接数——本 Lab 不支持 link()，
                                 * 恒为 1，留着是为了让格式跟真实文件
                                 * 系统对得上，也给挑战任务留位置 */
    uint32_t size;              /* 文件字节数（不是块数） */
    uint32_t addrs[NDIRECT];    /* 直接块的块号，0 表示该槽位未使用 */
    /* 手工填充到 64 字节：让 BSIZE/sizeof(dinode) 正好是整数（8），
     * 一个块装 8 个 inode，inode 号到"块号+块内偏移"的换算就是两次整数
     * 运算，不会出现"一个 inode 跨越两个块"这种要拼接读取的情况。
     * 靠编译器自然对齐得到的 sizeof 是 40，64-40=24 字节填充。
     * 显式写出这个填充、而不是依赖 __attribute__((aligned(64)))，是因为
     * 磁盘格式必须是"字节级确定"的：填充字节的存在和数量是格式的一部分,
     * 应该在格式定义里看得见，不该藏在一个属性里让读者自己算。 */
    uint8_t  pad[24];
};

/* 目录项：inode 号 + 文件名。
 *
 * name 不保证以 '\0' 结尾——正好 DIRSIZ 个字符的名字会占满整个字段。
 * 这是 xv6/ext2 系目录项的经典约定，也是一个真实的踩坑点：用 strcmp()
 * 比较这种名字会越界读到下一个目录项的 inum 字节。fs.c 里专门写了
 * name_eq() 而不是直接用 strcmp()，见那边的注释。 */
#define DIRSIZ 30

struct dirent {
    uint16_t inum;        /* 0 表示这个目录项是空的 */
    char     name[DIRSIZ];
};

/* 根目录的 inode 号固定为 1。
 *
 * 为什么不是 0：0 被用来表示"无效/空"（dinode.type==0 是空槽位，
 * dirent.inum==0 是空目录项，dinode.addrs[i]==0 是未使用的块槽位）。
 * 如果根目录是 0 号，"inum 0"就同时可能表示"空目录项"和"指向根目录的
 * 目录项"，解析时无法区分。让有效 inode 号从 1 开始，0 永久保留作
 * "无效"，是文件系统设计里几乎一致的选择（ext2 的 EXT2_BAD_INO=1、
 * 根目录=2，也是把小号留给特殊用途）。
 *
 * 代价：inode 表里 0 号那个槽位（64 字节）永远浪费。用 16 个 inode 的
 * 本 Lab 里这是 1/16 的元数据空间，完全可以接受——换来的是"0 即无效"
 * 这个贯穿整个格式的、不需要额外标志位的一致约定。 */
#define ROOTINO 1

/* 一个块能放多少个 dirent / dinode——fs.c 和 mkfs.c 都要用，定义在这里
 * 保证两边算出来的是同一个数。 */
#define DIRENTS_PER_BLOCK (BSIZE / sizeof(struct dirent))
#define INODES_PER_BLOCK  (BSIZE / sizeof(struct dinode))

/* 静态断言：把上面那些"正好整除"的断言交给编译器检查，而不是写在注释里
 * 靠读者相信。改动 DIRSIZ/NDIRECT/pad 而忘了同步调整时，这里会直接编译
 * 失败——比"镜像能生成、内核也能挂载，但读出来的目录项错位"好查得多。
 *
 * _Static_assert 是 C11 的关键字，不需要 #include <assert.h>，在
 * -ffreestanding 下同样可用（freestanding 只是不保证标准库函数，不影响
 * 语言本身的关键字）——所以 mkfs 和内核可以共用这几行。 */
_Static_assert(sizeof(struct dinode) == 64,
               "dinode 必须正好 64 字节（BSIZE 的整数分之一），否则 inode 会跨块");
_Static_assert(sizeof(struct dirent) == 32,
               "dirent 必须正好 32 字节，否则目录项会跨块错位");
_Static_assert(BSIZE % sizeof(struct dinode) == 0,
               "BSIZE 必须能被 dinode 大小整除");
_Static_assert(BSIZE % sizeof(struct dirent) == 0,
               "BSIZE 必须能被 dirent 大小整除");
_Static_assert(sizeof(struct superblock) <= BSIZE,
               "超级块必须能装进一个块");
_Static_assert(FS_INODESTART + (FS_NINODES / (BSIZE / 64)) <= FS_DATASTART,
               "inode 表的块数必须放得进 [FS_INODESTART, FS_DATASTART) 这个区间");

#endif /* OSDEV_FS_FORMAT_H */
