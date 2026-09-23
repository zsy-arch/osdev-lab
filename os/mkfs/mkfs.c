/* Lab9：mkfs——把若干个宿主文件打包成一个本课程格式的文件系统镜像。
 *
 *   用法: mkfs <输出镜像> [要放进去的文件...]
 *   例子: mkfs fs.img fsroot/motd.txt build/user/sh build/user/ls
 *
 * 这个程序*不是*内核的一部分，它跑在你的开发机上，用宿主的 libc（所以
 * 可以放心用 fopen/fread/malloc，这是本课程唯一一个允许这样做的 .c 文件）。
 * 它的角色相当于真实系统里的 mkfs.ext4：把一块裸磁盘初始化成某种文件
 * 系统格式。内核侧只负责*读*这个格式（本 Lab 的文件系统是只读的），
 * 创建格式的工作交给宿主工具——这是教学内核的常规分工，好处是"造镜像"
 * 这一步可以用完整的标准库、可以随时用 hexdump 检查结果，不必先在内核
 * 里把写路径实现出来才能验证读路径。
 *
 * 为什么参数是"宿主文件列表"而不是把内容硬编码在这个 .c 里：Lab8 写下
 * 这个设计时给的理由是"Lab9 要做 ELF 加载器和 shell，需要把编译好的用户
 * 程序二进制放进镜像里"（xv6 的 mkfs 就是这么用的：mkfs fs.img README
 * user/_cat user/_echo ...）。这件事现在发生了：本 Lab 的 Makefile 往
 * 这个命令行后面加了六个 ELF 文件，mkfs.c 本身一行没改。
 *
 * 值得注意的是"一行没改"具体是靠什么做到的——不是运气，是两处刻意的
 * 泛化：add_host_file() 按 BSIZE 分块流式读取（不假设文件小到能一次读
 * 完），inode_append() 用 NDIRECT 循环而不是展开成 8 个赋值（不假设
 * NDIRECT 是多少）。这两处如果当时图省事写成了特化版本，NDIRECT 从 8
 * 变到 60 就会要求改代码。"为下一步留好接口"落到实处是这种程度的具体，
 * 不是一句口号。
 *
 * 错误处理取向：任何一步失败都直接报错退出（die()），不做部分恢复。
 * mkfs 是构建流程里的一环，失败就该让 make 停下来，生成一个"一半对一半
 * 错"的镜像比直接失败糟糕得多——那种镜像在内核里的表现是各种难以归因的
 * 解析错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "fs_format.h"

/* 整个镜像先在内存里拼好，最后一次性写盘。256KiB 的镜像完全放得下（Lab8
 * 是 32KiB），这样做的好处是可以随意回头修改已经"写"过的块（比如最后才
 * 知道总共用了多少块、需要回填位图），不必操心文件偏移的 seek。真实 mkfs
 * 处理几百 GB 的磁盘时当然不能这样，必须流式写入 + 精确 seek。
 *
 * 这是一个静态数组而不是 malloc：静态数组在镜像尺寸失控时会撞上链接期
 * 或加载期的限制（几百 MB 的 .bss 会让链接器或 OS 直接拒绝），而 malloc
 * 会安静地成功然后吃掉内存。对一个构建工具来说，前者是更好的失败方式。 */
static unsigned char img[FS_NBLOCKS * BSIZE];

/* 下一个可分配的数据块号。从 FS_DATASTART 开始线性往后发，不复用——
 * 本 Lab 的 mkfs 只在"从零造一个新镜像"这一种场景下运行，没有删除文件、
 * 没有空洞，最简单的 bump 分配器就够了。 */
static uint32_t next_free_block = FS_DATASTART;

/* 下一个可分配的 inode 号。从 ROOTINO 开始——ROOTINO(=1) 本身会被
 * 第一次 alloc_inode() 拿走给根目录用，0 号永久保留作"无效"，见
 * fs_format.h 里 ROOTINO 的注释。 */
static uint32_t next_free_inode = ROOTINO;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mkfs: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* 返回块 n 在内存镜像里的起始地址。所有对镜像的读写都通过这个函数，
 * 顺手把"块号越界"这个错误集中在一个地方检查掉。 */
static unsigned char *block(uint32_t n)
{
    if (n >= FS_NBLOCKS) {
        die("块号 %u 越界（镜像只有 %u 块）——镜像太小，放不下要打包的文件。"
            "改大 fs_format.h 里的 FS_NBLOCKS，注意它同时是内核侧静态断言的依据",
            n, (unsigned)FS_NBLOCKS);
    }
    return img + (size_t)n * BSIZE;
}

/* 返回 inode 号 inum 对应的 dinode 在镜像里的地址。
 *
 * inode 号 -> 位置的换算就是这两行：块号 = inodestart + inum/每块个数，
 * 块内下标 = inum % 每块个数。这个换算在 mkfs 和内核 fs.c 的
 * read_inode() 里各写一遍，两边必须一致——这是"同一份格式、两个独立
 * 实现"的必然结果，也是文件系统 bug 的经典产地（一边算错就表现为读出
 * 来的 inode 内容是隔壁 inode 的）。本 Lab 用同一份 fs_format.h 提供
 * INODES_PER_BLOCK 和 FS_INODESTART，把可能不一致的部分压到最小。 */
static struct dinode *inode_at(uint32_t inum)
{
    if (inum == 0 || inum >= FS_NINODES) {
        die("inode 号 %u 无效（有效范围 1..%u）", inum, (unsigned)FS_NINODES - 1);
    }
    uint32_t blk = FS_INODESTART + inum / INODES_PER_BLOCK;
    uint32_t idx = inum % INODES_PER_BLOCK;
    return (struct dinode *)(block(blk) + idx * sizeof(struct dinode));
}

static uint32_t alloc_block(void)
{
    if (next_free_block >= FS_NBLOCKS) {
        die("数据块用完了（镜像共 %u 块）", (unsigned)FS_NBLOCKS);
    }
    return next_free_block++;
}

static uint32_t alloc_inode(uint16_t type)
{
    if (next_free_inode >= FS_NINODES) {
        die("inode 用完了（共 %u 个，其中 0 号保留）", (unsigned)FS_NINODES);
    }
    uint32_t inum = next_free_inode++;
    struct dinode *di = inode_at(inum);
    di->type = type;
    di->nlink = 1;
    di->size = 0;
    return inum;
}

/* 在位图里把块 n 标记为已占用。
 *
 * 位序约定：块 n 对应第 n/8 个字节的第 n%8 位（最低位是 bit 0）。这个
 * 约定必须跟内核侧的一致——内核的挂载自检会重算一遍同样的位图再逐字节
 * 比对（见 fs.c 的 fs_check_bitmap()），位序搞反的话自检会立刻失败,
 * 不会悄悄放过。 */
static void bitmap_mark(uint32_t n)
{
    unsigned char *bm = block(FS_BITMAPSTART);
    bm[n / 8] |= (unsigned char)(1u << (n % 8));
}

/* 往 inode 里追加 len 字节数据，按需分配数据块。
 *
 * 这个函数是"文件偏移 -> 块号"这个映射的写侧，跟内核 fs_read() 的读侧
 * 正好对称：读侧算 off/BSIZE 得到第几个直接块、off%BSIZE 得到块内偏移；
 * 写侧这里按当前 size 算出该往哪个块的哪个位置放。两侧用的是同一套算术,
 * 这也是为什么"文件大小正好是 BSIZE 整数倍"值得专门放一个测试文件进去：
 * 那是两侧算术唯一会分歧的边界（写侧写满最后一块就停，读侧必须不去读
 * 下一个不存在的块）。 */
static void inode_append(uint32_t inum, const void *data, uint32_t len)
{
    struct dinode *di = inode_at(inum);
    const unsigned char *src = data;

    while (len > 0) {
        uint32_t off = di->size;
        uint32_t bi = off / BSIZE;          /* 第几个直接块 */
        uint32_t boff = off % BSIZE;        /* 块内偏移 */

        if (bi >= NDIRECT) {
            die("文件超过单文件上限 %u 字节（NDIRECT=%u 个直接块 × %u 字节/块）。"
                "本 Lab 不实现间接块，见 fs_format.h 里 NDIRECT 的注释",
                (unsigned)(NDIRECT * BSIZE), (unsigned)NDIRECT, (unsigned)BSIZE);
        }

        if (di->addrs[bi] == 0) {
            di->addrs[bi] = alloc_block();
        }

        uint32_t n = BSIZE - boff;          /* 这一块还能放多少 */
        if (n > len) {
            n = len;
        }
        memcpy(block(di->addrs[bi]) + boff, src, n);

        di->size += n;
        src += n;
        len -= n;
    }
}

/* 往目录 dir_inum 里加一个目录项，指向 inode target_inum、名字 name。 */
static void dir_add(uint32_t dir_inum, uint32_t target_inum, const char *name)
{
    if (strlen(name) > DIRSIZ) {
        die("文件名 \"%s\" 超过 DIRSIZ=%u 个字符", name, (unsigned)DIRSIZ);
    }

    struct dirent de;
    memset(&de, 0, sizeof(de));
    de.inum = (uint16_t)target_inum;
    /* 故意用 strncpy 而不是 strcpy：名字正好 DIRSIZ 个字符时不写结尾的
     * '\0'（name 字段就这么宽），这正是 fs_format.h 里说的"名字不保证
     * NUL 结尾"的来源。strncpy 在源串短于 n 时会用 '\0' 补满剩余空间，
     * 刚好是我们要的行为（配合上面的 memset 双保险）。 */
    strncpy(de.name, name, DIRSIZ);

    inode_append(dir_inum, &de, sizeof(de));
}

/* 从路径里取出文件名部分：fsroot/motd.txt -> motd.txt。
 *
 * 镜像里存的是文件名，不是宿主路径——镜像里的根目录是扁平的（本 Lab 不
 * 支持子目录，见 README 的简化清单），把宿主的目录结构带进去没有意义,
 * 而且 "build/user/sh" 这样的路径嵌套深一点就会撞上 DIRSIZ 上限。
 *
 * 这一步在 Lab9 变得更要紧了：用户程序的 ELF 文件是从 build/x86_64/user/
 * 这样的构建目录里喂进来的，而 shell 里敲的是 ls、镜像里的名字必须是
 * ls。basename 这一下正是把"构建产物的位置"和"运行时的名字"解耦开。 */
static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void add_host_file(uint32_t root_inum, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        die("打不开输入文件 %s", path);
    }

    uint32_t inum = alloc_inode(T_FILE);

    /* 一块一块地读进来再 append，而不是先 fseek 到末尾问出文件大小再一次
     * 性 malloc：这样对"文件大小"没有任何前置假设，管道/字符设备之类
     * 不可 seek 的输入也能用（Lab9 可能会想直接喂一个 objcopy 的输出）。 */
    unsigned char buf[BSIZE];
    size_t n;
    uint32_t total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        inode_append(inum, buf, (uint32_t)n);
        total += (uint32_t)n;
    }
    if (ferror(f)) {
        die("读 %s 出错", path);
    }
    fclose(f);

    const char *name = basename_of(path);
    dir_add(root_inum, inum, name);

    struct dinode *di = inode_at(inum);
    /* 打印每个文件占了哪几个块——这行输出在调试内核读路径时非常有用：
     * 内核读文件读出乱码时，第一件要确认的事就是"镜像里这个文件到底在
     * 哪几块"，有这行输出就不需要再去 hexdump 镜像反推。
     *
     * Lab8 这里是把每个块号都列出来，因为 NDIRECT=8、最多 8 个数字。本
     * Lab 的 NDIRECT 是 60，一个 20KB 的 ELF 就有 40 个块号，逐个列出来
     * 会把 mkfs 的输出淹掉。改成打印区间：alloc_block() 是个 bump 分配器,
     * 同一个文件的块必然连续，所以 "块 18..29" 跟列出 12 个数字携带的
     * 信息完全相同。
     *
     * 顺带说一句为什么不干脆不打：这行输出是"镜像里的布局"唯一的人可读
     * 记录，而排查读路径 bug 时最常见的第一个问题就是"内核读的块号对
     * 不对"。构建工具多打一行确定性的布局信息，成本几乎为零。 */
    uint32_t nblk = 0;
    while (nblk < NDIRECT && di->addrs[nblk] != 0) {
        nblk++;
    }
    printf("  inode %2u  %-12s %6u 字节  ", inum, name, total);
    if (nblk == 0) {
        printf("(无数据块)\n");
    } else if (nblk == 1) {
        printf("块 %u\n", di->addrs[0]);
    } else {
        printf("块 %u..%u (%u 块)\n", di->addrs[0], di->addrs[nblk - 1], nblk);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <输出镜像> [要放进镜像的文件...]\n", argv[0]);
        return 2;
    }

    const char *out_path = argv[1];

    memset(img, 0, sizeof(img));

    /* 根目录必须是第一个被分配的 inode，这样它的 inode 号才会是 ROOTINO
     * ——内核挂载时直接从 ROOTINO 开始查找，不需要在超级块里多存一个
     * "根目录 inode 号"字段（真实文件系统有时会存，本 Lab 用固定约定）。 */
    uint32_t root = alloc_inode(T_DIR);
    if (root != ROOTINO) {
        die("内部错误：根目录 inode 号是 %u，应该是 ROOTINO=%u",
            root, (unsigned)ROOTINO);
    }

    /* "." 和 ".." 都指向根目录自己——根目录的父目录就是它自己，这是
     * POSIX 的约定（/.. 等于 /）。本 Lab 的内核不解析路径、不支持子目录,
     * 这两项严格来说用不上，但它们是目录格式的一部分：不写的话，镜像用
     * 真实的 fsck 类工具检查会被判为损坏的目录，而且挑战任务里要做子目录
     * 时会发现格式里少了东西。写进去的成本是 64 字节。 */
    dir_add(root, root, ".");
    dir_add(root, root, "..");

    printf("mkfs: 生成 %s\n", out_path);
    for (int i = 2; i < argc; i++) {
        add_host_file(root, argv[i]);
    }

    /* 超级块最后写：nblocks 之类的值现在才全部确定（虽然本 Lab 的布局是
     * 静态的，先写也一样，但"元数据最后落盘"是有意为之的顺序——真实文件
     * 系统必须这样做，因为超级块是挂载的唯一入口，它先落盘、数据后落盘
     * 的话，中途掉电会得到一个"声称有效但内容不全"的文件系统。这正是
     * 日志/journaling 要解决的问题，见 README 的一致性讨论）。 */
    struct superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.nblocks = FS_NBLOCKS;
    sb.ninodes = FS_NINODES;
    sb.bitmapstart = FS_BITMAPSTART;
    sb.inodestart = FS_INODESTART;
    sb.datastart = FS_DATASTART;
    memcpy(block(0), &sb, sizeof(sb));

    /* 位图：先标元数据块（超级块 + 位图自己 + inode 表），再标已分配的
     * 数据块。分两个循环写、而不是合成一个 "0..next_free_block" 的循环,
     * 是为了让"哪些块属于元数据"在代码里看得见——内核侧的挂载自检也按
     * 这两类分别重算（见 fs.c 的 fs_check_bitmap()）。 */
    for (uint32_t b = 0; b < FS_DATASTART; b++) {
        bitmap_mark(b);
    }
    for (uint32_t b = FS_DATASTART; b < next_free_block; b++) {
        bitmap_mark(b);
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        die("打不开输出文件 %s", out_path);
    }
    if (fwrite(img, 1, sizeof(img), out) != sizeof(img)) {
        die("写 %s 失败", out_path);
    }
    if (fclose(out) != 0) {
        die("关闭 %s 失败", out_path);
    }

    printf("mkfs: 共 %u 块 × %u 字节 = %u 字节，已用 %u 块，inode 用了 %u/%u\n",
           (unsigned)FS_NBLOCKS, (unsigned)BSIZE, (unsigned)sizeof(img),
           next_free_block, next_free_inode - 1, (unsigned)FS_NINODES - 1);
    return 0;
}
