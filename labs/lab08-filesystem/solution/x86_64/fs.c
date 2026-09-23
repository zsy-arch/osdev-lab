/* Lab8：只读文件系统层——把"块设备上的字节"解释成"目录里的文件"。
 *
 * 这个文件在 solution/x86_64/ 和 solution/riscv64/ 下*逐字节相同*
 * （Makefile 的 check-fs-iface 规则用 cmp 守着，改了一边忘了另一边会
 * 直接构建失败）。这不是巧合，是本 Lab 想让你亲手确认的一件事：文件
 * 系统逻辑跟 CPU 架构完全无关。它只通过 blk_read() 碰硬件，而 blk_read()
 * 背后是两套毫无共同点的驱动（ATA PIO vs virtio-blk，见 blk.h 的对照）。
 * 一旦块设备抽象立住了，上面所有的层就都是纯粹的数据结构操作。
 *
 * 为什么两个架构各放一份而不是共享一份：starter/ 里这个文件是带 TODO 的
 * 骨架，两个架构的学生各自独立填写；solution/ 的两份则必须相同。如果
 * 抽到 src/common/ 共享一份，starter 就没法给两条独立的路线各留一份
 * 待填空的骨架了。这份"刻意的重复 + 机械化的一致性检查"是本课程处理
 * 这类冲突的固定手法（参考 syscall.h 在两个架构目录下各有一份）。
 *
 * 本层的全部简化（README 的简化清单会再列一遍）：
 *   - 只读。没有 create/write/unlink，因此没有块分配器、没有 inode
 *     分配器、没有日志。写路径真正难的不是"多一个函数"，而是崩溃
 *     一致性：元数据和数据的落盘顺序错了，掉电后文件系统就是坏的。
 *   - 没有 in-core inode 缓存。每次要用 inode 都从磁盘重读一次
 *     （见 fs_lookup() 里关于这个代价的注释）。
 *   - 目录是扁平的：只有根目录，不解析路径。
 *   - 没有并发保护。fs_read() 可能在任意一次 blk_read() 之后被定时器
 *     打断、切到另一个进程再进来（Lab7 的时钟处理程序会 yield()），
 *     所以这里所有的缓冲区都必须在栈上，见下面的专门说明。
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "string.h"

#include "fs_format.h"
#include "blk.h"
#include "fs.h"

/* 挂载时从磁盘读出来的超级块，之后全程只读。
 *
 * 这是本文件里唯一的可变全局状态，而且只在 fs_init() 里写一次（那时
 * 还没有任何用户进程在跑，proc_init() 尚未被调用），之后所有访问都是
 * 读。所以它不受下面"不能用静态缓冲区"那条约束的影响——那条约束针对的
 * 是"函数执行期间被反复写入的临时缓冲区"。 */
static struct superblock sb;

/* 我们是否已经挂载成功。用于把"忘了调 fs_init() 就用文件系统"这个
 * 编程错误变成一句明确的 panic，而不是让它表现成"读出来的 inode 全是
 * 0、每个文件都查不到"。 */
static int fs_mounted;

/* 位图自检用 uint64_t 当集合，因此要求总块数不超过 64。
 *
 * 这是本 Lab 镜像大小（FS_NBLOCKS=64）之下成立的简化：一个 uint64_t
 * 就能装下"哪些块被引用"这个集合，自检逻辑不需要再搞一个动态位图。
 * 静态断言写在这里而不只是注释，是为了让"把镜像改大"这个动作立刻在
 * 编译期撞上这条假设，而不是运行期悄悄算错。 */
_Static_assert(FS_NBLOCKS <= 64,
               "fs_check_bitmap() 用 uint64_t 表示块集合，FS_NBLOCKS 超过 64 就得改成真正的位图");

/* 读 inode 号 inum 对应的 dinode，拷贝到调用者提供的 *out。
 *
 * 为什么是"拷贝到调用者的缓冲区"，而不是返回一个指向静态缓冲区的指针
 * （那样写起来更短：return &cached_inode;）——这是本 Lab 一个真实的
 * 并发陷阱，不是风格洁癖：
 *
 * Lab7 的时钟中断处理程序在发现"当前有进程在跑"时会调用 yield()。
 * sys_read() -> fs_read() -> read_inode() 这整条链路都是在*进程的内核栈*
 * 上、开着中断跑的。如果 read_inode() 把结果放在一个 static 缓冲区里
 * 并返回它的地址，那么：进程 A 拿到指针、还没用完，时钟中断来了、切到
 * 进程 B，B 也调用 read_inode() 把那个 static 缓冲区改成了它自己的
 * inode，再切回 A——A 手里的指针指向的内容已经变成了 B 的 inode。表现
 * 出来是"偶发地读到别的文件的内容"，而且跟时钟频率有关，极难复现定位。
 *
 * 用"调用者给缓冲区、被调用者拷贝进去"的形式，每个调用者的 dinode 都
 * 在它自己的内核栈上，进程之间天然隔离。本文件所有需要块缓冲区的地方
 * 都遵循同一条规则：缓冲区一律是栈上的局部数组。
 *
 * 代价是每个调用者的栈帧多 64 字节（dinode）或 512 字节（块缓冲区）。
 * 进程内核栈只有一页（4KiB，见 proc.h 的 PROC_KSTACK_PAGES），所以本
 * 文件刻意保证同一时刻最多只有两层 512 字节缓冲区活着（fs_read 的块
 * 缓冲 + read_inode 的块缓冲），峰值约 1KiB，留足余量给 trap 现场和
 * 其余栈帧。 */
static void read_inode(uint32_t inum, struct dinode *out)
{
    if (!fs_mounted) {
        panic("read_inode: 文件系统还没挂载，先调用 fs_init()");
    }
    if (inum == 0 || inum >= sb.ninodes) {
        kprintf("read_inode: inode 号 %u 无效（有效范围 1..%u）\n",
                inum, sb.ninodes - 1);
        panic("read_inode: inode 号越界");
    }

    /* inode 号 -> (块号, 块内下标) 的换算。mkfs.c 里 inode_at() 做的是
     * 同一个换算——同一份格式的两个独立实现，任何一边算错都表现为"读出
     * 来的是隔壁 inode 的内容"。两边都从 fs_format.h 拿 INODES_PER_BLOCK,
     * 把可能不一致的部分压到最小。
     *
     * 块号用的是 sb.inodestart（从磁盘上的超级块读出来的），不是
     * FS_INODESTART（编译期常量）——这是"自描述元数据"的实际体现：
     * 布局信息的唯一权威来源是磁盘上的超级块，内核不硬编码布局。
     * 否则超级块里那几个 *start 字段就是死字段，写了也没人读。 */
    uint32_t blk = sb.inodestart + inum / INODES_PER_BLOCK;
    uint32_t idx = inum % INODES_PER_BLOCK;

    uint8_t buf[BSIZE];
    blk_read(blk, buf);
    memcpy(out, buf + idx * sizeof(struct dinode), sizeof(struct dinode));
}

/* 比较一个目录项里的名字和一个以 '\0' 结尾的 C 字符串。
 *
 * 为什么不能直接用 strcmp(de.name, want)：dirent.name 是一个宽度固定为
 * DIRSIZ 的字符数组，名字正好 DIRSIZ 个字符时*没有*结尾的 '\0'（见
 * fs_format.h 里 DIRSIZ 的注释）。strcmp 会一路读过 name 的末尾，进入
 * 下一个目录项的 inum 字节，甚至读出块缓冲区的边界——在最好的情况下
 * 只是比较结果错了，在最坏的情况下是一次越界读。
 *
 * 这不是理论风险：ext2/xv6 系目录项都是这种"定宽、不保证 NUL 结尾"的
 * 布局，"直接对它用 str* 函数"是这类格式上最经典的一类 bug。
 *
 * 也不能用 strncmp(de.name, want, DIRSIZ)：那样 "hello" 和 "hello.txt"
 * 会被判为不等（对），但 "hello" 和 "hello" 之后跟着垃圾字节的情况下
 * strncmp 遇到 want 的 '\0' 就停，会判为相等（也对）——strncmp 其实
 * 够用。问题在于它读 disk_name 时仍然可能越过 '\0' 继续比到第 DIRSIZ
 * 个字节；本 Lab 的 mkfs 会把 name 剩余部分清零（memset + strncpy），
 * 所以实际不会出错。这里仍然手写一个循环，是为了让"名字可能不带 '\0'"
 * 这个约束在代码里看得见，而不是依赖"mkfs 恰好把尾部清零了"这个
 * 上游实现细节——格式允许的输入，读侧就该能正确处理。 */
static int name_eq(const char *disk_name, const char *want)
{
    for (uint32_t i = 0; i < DIRSIZ; i++) {
        if (want[i] == '\0') {
            /* want 到头了：只有 disk_name 也在同一位置结束才算相等。
             * 注意 disk_name[i] 此时一定在 name 字段内部（i < DIRSIZ）,
             * 读它是安全的。 */
            return disk_name[i] == '\0';
        }
        if (disk_name[i] != want[i]) {
            return 0;
        }
    }
    /* 前 DIRSIZ 个字符逐一相同，且 want 从未结束——那么只有 want 正好
     * DIRSIZ 个字符长时才算相等。读 want[DIRSIZ] 是安全的：能走到这里
     * 说明 want 至少有 DIRSIZ 个非 '\0' 字符，第 DIRSIZ 个下标要么是
     * '\0' 要么是更多字符，都在 want 这个 C 字符串的合法范围内。 */
    return want[DIRSIZ] == '\0';
}

uint32_t fs_size(uint32_t inum)
{
    struct dinode di;
    read_inode(inum, &di);
    return di.size;
}

/* 从文件里读一段字节。这是整个文件系统层的核心：把"文件内的偏移"翻译成
 * "磁盘上的块号 + 块内偏移"。
 *
 * 两个必须做对的边界，本 Lab 专门准备了测试文件来覆盖（见 fsroot/）：
 *
 *   1. 按 inode.size 截断，而不是按"读满整块"。
 *      文件的最后一块几乎总是只用了一部分，块里剩下的字节是 mkfs 留下的
 *      零（或者更糟——如果这块以前属于别的文件，会是那个文件的残留内容）。
 *      不截断的话，read() 会把这些填充字节也交给用户程序，表现为"文件
 *      末尾多了一串 \0"，甚至泄露别的文件的数据（真实内核里这是一类
 *      信息泄露漏洞）。fsroot/hello.txt 只有 98 字节、不足一块，专门
 *      覆盖这条路径。
 *
 *   2. 文件大小正好是 BSIZE 整数倍时，不能多读一块。
 *      "还有数据要读吗"必须用剩余*字节数*判断，不能用"下一个 addrs 槽位
 *      非零吗"——写侧（mkfs）填满最后一块就停了，下一个槽位是 0；如果
 *      读侧的循环条件是"直到块用完"，就会去读块号 0（超级块！）并把它
 *      当文件内容返回。fsroot/exact.txt 正好 512 字节，专门覆盖这条。
 *
 * 返回 0 表示到达文件末尾（EOF），跟 POSIX read(2) 一致——不是错误。
 * 用户程序据此结束读取循环，见 user_prog.S。 */
uint32_t fs_read(uint32_t inum, uint32_t off, void *dst, uint32_t n)
{
    struct dinode di;
    read_inode(inum, &di);

    if (di.type == 0) {
        kprintf("fs_read: inode %u 是空槽位（type==0）\n", inum);
        panic("fs_read: 读了一个未分配的 inode，文件系统元数据不一致");
    }

    /* 边界 1/2：偏移已经到达或超过文件末尾 —— EOF。
     * 必须在做减法之前判断：off 和 size 都是无符号数，off > size 时
     * size - off 会下溢成一个巨大的正数，然后下面的循环会一直读下去。
     * 无符号下溢是这一类偏移计算里最常见的 bug 来源。 */
    if (off >= di.size) {
        return 0;
    }
    /* 边界 1：把请求长度截断到"文件里真正剩下的字节数"。 */
    uint32_t remaining = di.size - off;
    if (n > remaining) {
        n = remaining;
    }

    uint8_t *out = dst;
    uint32_t done = 0;
    uint8_t buf[BSIZE];

    /* 边界 2：循环条件是 done < n（剩余字节数），不是"还有块可读"。 */
    while (done < n) {
        uint32_t pos = off + done;
        uint32_t bi = pos / BSIZE;      /* 第几个直接块 */
        uint32_t boff = pos % BSIZE;    /* 块内偏移 */

        if (bi >= NDIRECT) {
            /* 走到这里说明 inode.size 声称的大小超过了 NDIRECT 个直接块
             * 能表达的范围——镜像本身不一致（mkfs 会在生成时就拒绝这种
             * 文件）。panic 而不是静默返回短读：短读会让上层以为"文件就
             * 这么长"，把一个元数据损坏问题伪装成正常结果。 */
            kprintf("fs_read: inode %u 的偏移 %u 落在第 %u 个直接块，超出 NDIRECT=%u\n",
                    inum, pos, bi, (uint32_t)NDIRECT);
            panic("fs_read: 文件偏移超出直接块能表达的范围（inode.size 与 addrs 不一致）");
        }

        uint32_t addr = di.addrs[bi];
        if (addr == 0) {
            /* 文件中间有"洞"（稀疏文件）。本 Lab 的 mkfs 从不产生空洞
             * （inode_append 顺序分配），所以这只可能是镜像损坏。真实
             * 文件系统支持稀疏文件，读到洞时应该返回全 0 而不是报错——
             * 那需要格式层面明确区分"0 表示未分配"和"0 号块"，本 Lab
             * 的格式没有这个区分（0 号块是超级块，永远不会分配给文件）。 */
            kprintf("fs_read: inode %u 的第 %u 个直接块是 0（文件中间有洞）\n",
                    inum, bi);
            panic("fs_read: inode 在 size 范围内有未分配的块，镜像不一致");
        }

        blk_read(addr, buf);

        uint32_t chunk = BSIZE - boff;   /* 这一块里还能取多少 */
        if (chunk > n - done) {
            chunk = n - done;
        }
        memcpy(out + done, buf + boff, chunk);
        done += chunk;
    }

    return done;
}

/* 在根目录里按名字查找。找不到返回 0（0 是永久保留的无效 inode 号，
 * 见 fs_format.h 里 ROOTINO 的注释）。 */
uint32_t fs_lookup(const char *name)
{
    struct dinode root;
    read_inode(ROOTINO, &root);
    if (root.type != T_DIR) {
        kprintf("fs_lookup: 根 inode %u 的类型是 %u，不是目录（T_DIR=%u）\n",
                (uint32_t)ROOTINO, (uint32_t)root.type, (uint32_t)T_DIR);
        panic("fs_lookup: 根 inode 不是目录，镜像布局不对");
    }

    /* 目录就是一个"内容恰好是 dirent 数组"的普通文件——所以这里直接用
     * fs_read() 去读它，不需要任何目录专用的读取代码。这是 Unix 文件
     * 系统设计里一个很漂亮的统一：目录和文件共用同一套 inode、同一套
     * 块映射、同一套读取路径，唯一的区别是 type 字段和"内容该怎么解释"。
     *
     * 代价：每次 fs_read() 调用都会重新 read_inode() 一次（多一次
     * blk_read），所以查一个名字要读 2×(目录项数) 个块。本 Lab 的根
     * 目录只有 5 项，完全无所谓；真实内核正是为了消掉这个重复开销才
     * 有 in-core inode 缓存的（见本文件顶部简化清单）。把这个代价留在
     * 代码里、并在这里点明，比提前引入一层缓存更有教学价值。 */
    for (uint32_t off = 0; off + sizeof(struct dirent) <= root.size;
         off += sizeof(struct dirent)) {
        struct dirent de;
        uint32_t got = fs_read(ROOTINO, off, &de, sizeof(de));
        if (got != sizeof(de)) {
            kprintf("fs_lookup: 读目录项只读到 %u 字节（应为 %u），目录大小 %u\n",
                    got, (uint32_t)sizeof(de), root.size);
            panic("fs_lookup: 目录大小不是 dirent 的整数倍");
        }
        if (de.inum == 0) {
            continue; /* 空目录项：本 Lab 的 mkfs 不产生，但格式允许 */
        }
        if (name_eq(de.name, name)) {
            return de.inum;
        }
    }
    return 0;
}

/* 挂载自检：重算一遍"哪些块应该被占用"，跟磁盘上的位图逐位比对。
 *
 * 为什么一个只读文件系统还要读位图：本 Lab 不分配块，位图在运行期确实
 * 没有用途。但它是格式的一部分，而"格式里有一个字段，代码从来不读它"
 * 是很容易掩盖 bug 的状态——mkfs 把位图写错了也没人发现，等到有人做
 * 挑战任务实现写路径、第一次真的去读位图分配块时才炸。
 *
 * 更重要的是，这一步就是 fsck 的核心思想的最小版本：文件系统的元数据
 * 有内在冗余（"位图说哪些块被用了"和"inode 实际引用了哪些块"必须一致），
 * 冗余就意味着可以交叉验证。真实 fsck 检查的不一致种类多得多（引用计数
 * 对不上、孤儿 inode、目录环），但都是同一个思路。
 *
 * 分成两个独立的辅助函数、再比较两个 uint64_t，而不是在一个函数里同时
 * 持有两个 512 字节缓冲区：内核栈只有一页，见 read_inode() 关于栈预算
 * 的说明。 */
static uint64_t collect_referenced_blocks(void)
{
    /* 元数据块恒定被占用：超级块 + 位图 + inode 表，即 [0, datastart)。 */
    uint64_t mask = 0;
    for (uint32_t b = 0; b < sb.datastart; b++) {
        mask |= (uint64_t)1 << b;
    }

    /* 再加上所有在用 inode 引用的数据块。遍历整个 inode 表、而不是从
     * 根目录递归下去：本 Lab 目录是扁平的，两种走法结果相同，但"扫全表"
     * 还能发现"有 inode 被分配了却没有任何目录项指向它"（孤儿 inode）
     * 所占用的块——真实 fsck 也是扫全表 + 另外单独查可达性。 */
    for (uint32_t inum = ROOTINO; inum < sb.ninodes; inum++) {
        struct dinode di;
        read_inode(inum, &di);
        if (di.type == 0) {
            continue;
        }
        for (uint32_t i = 0; i < NDIRECT; i++) {
            uint32_t a = di.addrs[i];
            if (a == 0) {
                continue;
            }
            if (a < sb.datastart || a >= sb.nblocks) {
                kprintf("fs 自检: inode %u 引用了非法块号 %u（合法数据块范围 [%u, %u)）\n",
                        inum, a, sb.datastart, sb.nblocks);
                panic("fs 自检: inode 引用了数据区以外的块");
            }
            if (mask & ((uint64_t)1 << a)) {
                kprintf("fs 自检: 块 %u 被两个 inode 同时引用（inode %u 又引用了它）\n",
                        a, inum);
                panic("fs 自检: 数据块被重复引用（block aliasing）");
            }
            mask |= (uint64_t)1 << a;
        }
    }
    return mask;
}

static uint64_t read_bitmap_mask(void)
{
    uint8_t buf[BSIZE];
    blk_read(sb.bitmapstart, buf);

    uint64_t mask = 0;
    for (uint32_t b = 0; b < sb.nblocks; b++) {
        /* 位序约定：块 b 对应第 b/8 字节的第 b%8 位（最低位是 bit 0）。
         * 必须跟 mkfs.c 的 bitmap_mark() 完全一致，否则这个自检会失败
         * ——这正是它该有的行为：位序不一致是一个必须在挂载时就发现的
         * 格式分歧，不该等到实现写路径时才暴露。 */
        if (buf[b / 8] & (1u << (b % 8))) {
            mask |= (uint64_t)1 << b;
        }
    }

    /* nblocks 以外的位应该全是 0（见 fs_format.h 的位图说明）。这些位
     * 不影响任何功能，但它们非 0 说明 mkfs 和格式定义对"位图有多长"的
     * 理解不一致，值得当成错误。 */
    for (uint32_t byte = 0; byte < BSIZE; byte++) {
        for (uint32_t bit = 0; bit < 8; bit++) {
            uint32_t b = byte * 8 + bit;
            if (b < sb.nblocks) {
                continue;
            }
            if (buf[byte] & (1u << bit)) {
                kprintf("fs 自检: 位图里第 %u 位被置位，但总共只有 %u 块\n",
                        b, sb.nblocks);
                panic("fs 自检: 位图在 nblocks 之外有置位");
            }
        }
    }
    return mask;
}

static void fs_check_bitmap(void)
{
    uint64_t referenced = collect_referenced_blocks();
    uint64_t on_disk = read_bitmap_mask();

    if (referenced != on_disk) {
        /* 把差异打出来再 panic：只说"不一致"没法排查，说清楚"哪些块
         * 位图说占用但没人引用（泄露）/哪些块被引用但位图没标（会被
         * 重复分配）"才能直接定位。 */
        uint64_t leaked = on_disk & ~referenced;
        uint64_t missing = referenced & ~on_disk;
        kprintf("fs 自检失败: 位图 vs inode 引用不一致\n");
        kprintf("  位图标了但没有 inode 引用的块(空间泄露):");
        for (uint32_t b = 0; b < sb.nblocks; b++) {
            if (leaked & ((uint64_t)1 << b)) {
                kprintf(" %u", b);
            }
        }
        kprintf("\n  被 inode 引用但位图没标的块(会被重复分配):");
        for (uint32_t b = 0; b < sb.nblocks; b++) {
            if (missing & ((uint64_t)1 << b)) {
                kprintf(" %u", b);
            }
        }
        kprintf("\n");
        panic("fs 自检: 位图与 inode 引用不一致，镜像已损坏（用 mkfs 重新生成）");
    }

    uint32_t used = 0;
    for (uint32_t b = 0; b < sb.nblocks; b++) {
        if (on_disk & ((uint64_t)1 << b)) {
            used++;
        }
    }
    kprintf("fs: bitmap self-check ok, %u/%u blocks in use\n", used, sb.nblocks);
}

void fs_init(void)
{
    /* 超级块是"不知道布局也能找到"的唯一一块——它固定在块 0。其余所有
     * 位置（位图/inode 表/数据区在哪）都从它里面读出来。 */
    uint8_t buf[BSIZE];
    blk_read(0, buf);
    memcpy(&sb, buf, sizeof(sb));

    if (sb.magic != FS_MAGIC) {
        /* 这是本 Lab 最常见的一类失败，所以提示写得很具体：读到的魔数
         * 是什么、应该是什么、以及最可能的原因。
         *
         * 读到全 0 通常意味着"QEMU 挂的是一个空文件/没挂对镜像"；读到
         * 别的内容通常意味着"挂上了但不是本课程格式的镜像"。不带任何
         * 提示直接 panic("bad magic") 的话，这两种情况看起来一模一样。 */
        /* 注意用 %x 而不是 %08x：本课程的 kprintf 不支持宽度修饰符
         * （见 console.h 的说明），写 %08x 会被原样打印出来。 */
        kprintf("fs_init: 超级块魔数不对：读到 0x%x，期望 0x%x ('LAB8')\n",
                sb.magic, (uint32_t)FS_MAGIC);
        kprintf("  最可能的原因：给 QEMU 挂的磁盘镜像不是 mkfs 生成的，或者根本没挂上。\n");
        kprintf("  先确认 build/fs.img 存在且是 32768 字节：make fs.img\n");
        panic("fs_init: 磁盘上没有本课程格式的文件系统");
    }

    /* 超级块自身的合理性检查。磁盘上的元数据是"不可信输入"——它可能来自
     * 一个版本不同的 mkfs、一个被截断的镜像，或者一个完全无关的文件恰好
     * 前 4 字节撞上了魔数。不检查就直接用这些值去算块号，会得到越界的
     * blk_read()（表现为读到随机内容或驱动层 panic），归因会绕一大圈。
     *
     * 真实内核对超级块的校验比这严格得多（ext4 还有 checksum），而且是
     * 安全边界的一部分：挂载一个恶意构造的镜像不应该能让内核越界访问。 */
    if (sb.nblocks == 0 || sb.nblocks > FS_NBLOCKS) {
        kprintf("fs_init: 超级块声称有 %u 块，超出本内核支持的上限 %u\n",
                sb.nblocks, (uint32_t)FS_NBLOCKS);
        panic("fs_init: 超级块的块数不合理");
    }
    if (sb.ninodes == 0 || sb.ninodes > FS_NINODES) {
        kprintf("fs_init: 超级块声称有 %u 个 inode，超出上限 %u\n",
                sb.ninodes, (uint32_t)FS_NINODES);
        panic("fs_init: 超级块的 inode 数不合理");
    }
    if (!(sb.bitmapstart > 0 && sb.bitmapstart < sb.inodestart &&
          sb.inodestart < sb.datastart && sb.datastart <= sb.nblocks)) {
        kprintf("fs_init: 超级块布局不合理: bitmap=%u inode=%u data=%u nblocks=%u\n",
                sb.bitmapstart, sb.inodestart, sb.datastart, sb.nblocks);
        panic("fs_init: 超级块的布局字段次序不对（应满足 0 < bitmap < inode < data <= nblocks）");
    }

    fs_mounted = 1;

    kprintf("fs: mounted, magic ok, %u blocks, %u inodes, data from block %u\n",
            sb.nblocks, sb.ninodes, sb.datastart);

    /* 自检放在 fs_mounted=1 之后：它内部要调用 read_inode()，而
     * read_inode() 会检查挂载标志。顺序反过来会在自检的第一次
     * read_inode() 上 panic("还没挂载")——一个只在初始化顺序写错时
     * 才会出现、但很容易误导人的假故障。 */
    fs_check_bitmap();
}
