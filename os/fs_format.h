/* Lab9：磁盘上的文件系统格式——mkfs（跑在你开发机上的普通程序）、两个
 * 架构的内核、以及本 Lab 新增的用户程序（跑在 QEMU 里的裸机代码）共用
 * 这一份定义。
 *
 * 为什么这个文件放在 Lab 根目录，而不是像 proc.h/syscall.h 那样每个架构
 * 目录下各放一份：proc.h 描述的是"某个架构的寄存器现场长什么样"，天然
 * 架构相关；这份文件描述的是"磁盘上的字节怎么排列"，跟 CPU 架构完全
 * 无关——同一个 fs.img 必须能被 x86_64 内核和 riscv64 内核以*完全相同*
 * 的方式解释，否则"文件系统"这个抽象根本不成立。所以它只能有一份，由
 * 四个使用者共同 include。
 *
 * Lab8 有三个使用者（mkfs / 两个内核）；Lab9 多出来的第四个是 user/ls.c
 * ——ls 要列目录，就得知道 struct dirent 的字节布局。它*可以*在自己那边
 * 重新声明一遍，但那样就出现了同一份格式的第二个定义，两处哪天不同步，
 * 症状是 ls 输出错位的文件名而内核一切正常，极难定位。让用户程序直接
 * include 这份唯一定义，是本 Lab 顺手示范的一个工程习惯：格式定义只能
 * 有一处，跨越内核/用户态边界也不例外。真实系统里这件事由 UAPI 头文件
 * （Linux 的 include/uapi/）承担，是同一个思路的正式版本。
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

/* mkfs 跑在开发机上，用宿主的标准库；内核和用户程序跑在裸机上，用本课程
 * 自己的 types.h。三边都需要 uint32_t 这类定宽整数，但来源不同。
 *
 * 注意用户程序也走 #else 这条分支：它虽然是"用户态"的，但本课程的用户态
 * 没有 libc（Lab9 自己写的那几个系统调用包装就是全部），所以对编译器而言
 * 它跟内核一样是 freestanding 环境。"用户态"和"有标准库"是两件独立的事，
 * 这里是个现成的例子。用户程序那边的 types.h 是 user/types.h（见 Makefile
 * 里给用户程序单独加的 -I$(LAB_ROOT)/user），内容是内核 types.h 的子集。
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

/* 魔数 "LAB9"（小端存放，所以字节序列是 'L','A','B','9'）。
 *
 * 超级块第一个字段就是魔数，这是文件系统格式的通用做法：挂载时先读这 4
 * 个字节，不匹配就立刻拒绝，而不是硬着头皮把一堆随机字节当 inode 表解析
 * ——后者的表现是"文件系统看起来挂上了，但读出来的每个文件大小都是几个
 * GB、inode 类型是 0x7f3a 这种不存在的值"，比直接报错难查得多。本 Lab
 * 的 fs_init() 会实测这条路径（见 fs.c 里对未格式化磁盘的处理）。
 *
 * 为什么 Lab9 要换魔数（Lab8 是 'LAB8' = 0x3842414C）：下面的布局常量
 * 全都变了（NDIRECT 8->60、dinode 64->256 字节、镜像 32KiB->256KiB）。
 * 一个 Lab8 的 fs.img 喂给 Lab9 的内核，超级块里 nblocks=64 是能读出来
 * 的，inode 表却会按 256 字节的步长去索引一张 64 字节步长的表——挂载
 * "成功"，然后读到的每个 inode 都是垃圾。魔数跟着格式一起变，这种跨版本
 * 误用就变成挂载时一句明确的报错。
 *
 * 真实文件系统用的是"魔数不变 + 版本号/特性位"：ext2/3/4 三代共用
 * 0xEF53，靠 s_feature_incompat 里的位来判断"这个镜像用了我不认识的
 * 特性吗"，因为它们必须保证老镜像能被新内核挂载。本课程不需要向后兼容
 * （每个 Lab 自己 mkfs 自己的镜像），直接换魔数是更简单、更不容易出错
 * 的做法——但要知道这是因为需求弱，不是因为它更好。 */
#define FS_MAGIC 0x3942414Cu /* 'L'=0x4C 'A'=0x41 'B'=0x42 '9'=0x39 */

/* 磁盘布局（块号）：
 *
 *   块 0        超级块（sb）
 *   块 1        空闲块位图（1 块 = 512 字节 = 4096 位，够描述 4096 个块；
 *               本 Lab 的镜像有 512 块，只有低 512 位有意义，bit i == 1
 *               表示块 i 已被占用；i >= nblocks 的位是无意义的填充，
 *               恒为 0，见 mkfs.c 的说明）
 *   块 2..17    inode 表（16 块 × 2 个 inode/块 = 32 个 inode）
 *   块 18..     数据块（目录内容、文件内容）
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
/* Lab9 相对 Lab8 把镜像从 32KiB 放大到 256KiB，inode 从 16 个加到 32 个。
 *
 * 驱动这次扩容的是一个很具体的数字：Lab8 的单文件上限是 NDIRECT × BSIZE
 * = 8 × 512 = 4096 字节，而本 Lab 要往磁盘里放的是*可执行文件*。随手
 * 编译一个 user/sh.c，ELF 里光是 .text 就往往超过 4KiB；六个用户程序加
 * 上脚本和文本文件，总量也早就超过 32KiB 的镜像。也就是说 Lab8 那套
 * 参数不是"偏小"，而是根本装不下本 Lab 的内容——这是格式必须改的原因，
 * 不是想改。
 *
 * 一个具体的历史类比：FAT12 的 4084 个簇上限、ext2 的 2TB 上限、MBR 的
 * 2TB 上限，都不是设计者疏忽，而是"当时够用"的参数在若干年后不够用了。
 * 文件系统格式里每一个定长字段都是一次对未来规模的赌注，本 Lab 让你亲手
 * 经历一次这个赌注到期的过程——包括 fs.c 里那个 Lab8 就写好的
 * _Static_assert(FS_NBLOCKS <= 64) 在这次扩容中如何准时炸出来。 */
#define FS_NBLOCKS  512 /* 整个镜像 512 × 512B = 256KiB */
#define FS_NINODES  32
#define FS_BITMAPSTART 1
#define FS_INODESTART  2
#define FS_DATASTART   18

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

/* 一个 inode 最多直接指向 60 个数据块 => 最大文件 60 × 512B = 30720 字节。
 *
 * Lab8 这里是 8（上限 4096 字节）。为什么涨到 60 而不是别的数：本 Lab 要
 * 存的是 ELF 可执行文件，4KiB 装不下；而 60 是"让 dinode 正好 256 字节"
 * 反推出来的——256 是 BSIZE 的整数分之一（一块装 2 个 inode），这个整除
 * 关系是下面 pad[] 那段注释讲的那条不能破的约束。先定 dinode 大小、再
 * 反推 NDIRECT，而不是先挑一个好看的 NDIRECT 再去凑填充，是因为前者保证
 * 整除、后者不保证。
 *
 * 真实文件系统在直接块用完之后还有一级/二级/三级间接块（xv6 有一级，
 * ext2 有三级），让单文件上限从几 KiB 涨到几 GiB。本 Lab 仍然只做直接块
 * ——注意"加大 NDIRECT"和"加间接块"是两种不同性质的扩容：前者把 inode
 * 撑大、单文件上限线性增长，代价是每个小文件也要占掉一整个大 inode；
 * 后者 inode 大小不变、上限指数增长，代价是多一层查表。Lab9 选前者，是
 * 因为它一行常量就能改完、且本 Lab 的教学重点在 ELF 加载和 shell 上；
 * 真实系统选后者，是因为"绝大多数文件很小、极少数文件很大"这个分布让
 * 线性扩容的空间浪费无法接受。ext2 的 12 个直接块 + 三级间接，正是这个
 * 权衡的标准答案。间接块仍留作挑战任务（见 README）。 */
#define NDIRECT 60

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
    /* 手工填充到 256 字节：让 BSIZE/sizeof(dinode) 正好是整数（2），
     * 一个块装 2 个 inode，inode 号到"块号+块内偏移"的换算就是两次整数
     * 运算，不会出现"一个 inode 跨越两个块"这种要拼接读取的情况。
     * 靠编译器自然对齐得到的 sizeof 是 8 + 4*60 = 248，256-248=8 字节填充。
     * 显式写出这个填充、而不是依赖 __attribute__((aligned(256)))，是因为
     * 磁盘格式必须是"字节级确定"的：填充字节的存在和数量是格式的一部分,
     * 应该在格式定义里看得见，不该藏在一个属性里让读者自己算。
     *
     * "一个 inode 不跨块"这条约束值得说清楚为什么不能破：read_inode() 只
     * 做一次 blk_read()，然后从块内偏移处 memcpy 出 sizeof(dinode) 个
     * 字节。如果 inode 跨块，最后一个 inode 的后半截就落在下一个块里，
     * 那次 memcpy 会读到当前块缓冲区之外——一个栈上缓冲区溢出。它不会
     * 立刻崩，而是静默读到相邻栈变量的字节，表现成"最后一个 inode 的
     * addrs 末几项是垃圾块号"。整除关系保证这件事不可能发生，所以它是
     * _Static_assert 而不是运行时检查。 */
    uint8_t  pad[8];
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
 * 代价：inode 表里 0 号那个槽位（256 字节）永远浪费。用 32 个 inode 的
 * 本 Lab 里这是 1/32 的元数据空间，完全可以接受——换来的是"0 即无效"
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
_Static_assert(sizeof(struct dinode) == 256,
               "dinode 必须正好 256 字节（BSIZE 的整数分之一），否则 inode 会跨块");
_Static_assert(sizeof(struct dirent) == 32,
               "dirent 必须正好 32 字节，否则目录项会跨块错位");
_Static_assert(BSIZE % sizeof(struct dinode) == 0,
               "BSIZE 必须能被 dinode 大小整除");
_Static_assert(BSIZE % sizeof(struct dirent) == 0,
               "BSIZE 必须能被 dirent 大小整除");
_Static_assert(sizeof(struct superblock) <= BSIZE,
               "超级块必须能装进一个块");

/* inode 表要占多少块，由 FS_NINODES 和 sizeof(dinode) 决定，必须放得进
 * [FS_INODESTART, FS_DATASTART) 这个区间。
 *
 * Lab8 这一条写的是 FS_NINODES / (BSIZE / 64)——那个 64 是硬编码的 dinode
 * 大小。本 Lab 把 dinode 改成 256 字节时，那个 64 不会报错，只会算出一个
 * 偏小的块数，于是这条断言在真正越界时依然通过。改成 INODES_PER_BLOCK
 * 之后，dinode 大小再变，这条断言自动跟着变。
 *
 * 这是"把常量写进断言"的典型翻车方式：断言本身成了需要同步维护的第二处
 * 定义。断言里应该只出现被检查的量和它们之间的关系，任何"我知道这个值是
 * 多少"的字面量都是隐患——它让断言在格式演进时静默失效，而断言静默失效
 * 比没有断言更糟，因为你以为它在看着。 */
_Static_assert(FS_INODESTART + (FS_NINODES + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK
                   <= FS_DATASTART,
               "inode 表的块数必须放得进 [FS_INODESTART, FS_DATASTART) 这个区间");

/* 位图只有 1 块，能描述 BSIZE * 8 = 4096 个块。Lab8 的镜像 64 块、Lab9 的
 * 512 块都远小于它，所以位图始终是 1 块——但这个"始终"是需要被检查的：
 * 哪天把 FS_NBLOCKS 调到 8192，位图就得占 2 块，而布局里 FS_BITMAPSTART
 * 和 FS_INODESTART 之间只留了 1 块的空间，多出来的那一块会直接压在 inode
 * 表上。那种损坏的表现是"文件系统挂载正常，但某几个 inode 的内容随着
 * 磁盘使用情况变化"，属于最难查的一类 bug。 */
_Static_assert(FS_NBLOCKS <= BSIZE * 8,
               "空闲块位图只有 1 块，最多描述 BSIZE*8 个块；"
               "要支持更多块就得让位图占多块，并相应调整 FS_INODESTART");
_Static_assert(FS_BITMAPSTART + 1 <= FS_INODESTART,
               "位图必须放得进 [FS_BITMAPSTART, FS_INODESTART) 这个区间");

#endif /* OSDEV_FS_FORMAT_H */
