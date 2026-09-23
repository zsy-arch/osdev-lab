/* Lab9：只读文件系统层——把"块设备上的字节"解释成"目录里的文件"。
 *
 * 相对 Lab8，这个文件的改动全部由磁盘格式的扩容引起（见 fs_format.h）：
 * 位图自检不能再用一个 uint64_t 当集合了，见下面 struct blkset 的说明。
 * fs_lookup() 另外学会了跳过开头的 '/'，这样 shell 里写 /ls 和 ls 都能
 * 找到同一个文件——本 Lab 仍然没有真正的路径解析，见那边的注释。
 *
 * 这个文件在 starter/x86_64/ 和 starter/riscv64/ 下*逐字节相同*（Makefile
 * 的 check-shared-iface 规则用 cmp 守着 starter 和 solution 两对，改了一边
 * 忘了另一边会直接构建失败）。这不是巧合，是本 Lab 想让你亲手确认的一件事：
 * 文件系统逻辑跟 CPU 架构完全无关。它只通过 blk_read() 碰硬件，而
 * blk_read() 背后是两套毫无共同点的驱动（ATA PIO vs virtio-blk，见 blk.h
 * 的对照）。一旦块设备抽象立住了，上面所有的层就都是纯粹的数据结构操作。
 *
 * 所以下面这几个 TODO，你在哪个架构下做都是同一份答案。**做完一个架构，
 * 这个文件可以直接拷到另一个架构的目录下**，然后 make check-shared-iface
 * 验一下——这个动作本身就是本 Lab 的结论之一，值得真的做一次。真正需要为
 * 第二个架构重写的只有块设备驱动那一个文件（ide.c / virtio.c）。
 *
 * 为什么两个架构各放一份而不是抽到 src/common/ 共享一份：共享的话就没法
 * 给两条独立的路线各留一份待填空的骨架了。这份"刻意的重复 + 机械化的
 * 一致性检查"是本课程处理这类冲突的固定手法（参考 syscall.h 在两个架构
 * 目录下各有一份）。
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

/* 位图自检用的"块集合"类型。
 *
 * Lab8 这里是一个 uint64_t，加一条静态断言：
 *
 *     _Static_assert(FS_NBLOCKS <= 64,
 *                    "fs_check_bitmap() 用 uint64_t 表示块集合，"
 *                    "FS_NBLOCKS 超过 64 就得改成真正的位图");
 *
 * 那条断言当时就写明了它的用途：让"把镜像改大"这个动作立刻在编译期撞上
 * 这条假设。本 Lab 把 FS_NBLOCKS 从 64 提到 512（见 fs_format.h 里关于
 * 为什么必须扩容的说明），于是它准时炸了——这是它唯一的一次工作机会，
 * 也正是它存在的全部意义。
 *
 * 值得想清楚没有这条断言会发生什么：mask |= (uint64_t)1 << b，当 b >= 64
 * 时移位量超过了类型宽度，这在 C 里是*未定义行为*。在 x86_64 上它的实际
 * 表现是 shl 指令只取移位量的低 6 位，也就是 1 << 64 得到 1、1 << 65 得到
 * 2……集合里的块号静默地按 64 取模折叠了。后果是自检*照样通过*：折叠是
 * 确定性的，两边（inode 引用 / 磁盘位图）用同一个错误公式折叠出同一个
 * 错误的 uint64_t，比较结果相等。一个本该报错的损坏镜像会被判为健康，
 * 而且在 riscv64 上折叠方式还可能不同（不同的移位指令语义），变成一个
 * 架构相关的假阴性。
 *
 * "断言把一个静默的错误答案变成一次响亮的编译失败"——这是断言最有价值的
 * 形态。注意它必须写在*假设被使用的地方*（fs.c，用 uint64_t 的这里），
 * 而不是格式定义里：FS_NBLOCKS <= 64 从来不是格式的要求，只是这一处实现
 * 手法的要求。断言放错位置会变成对格式的无端限制。
 *
 * 换成按字节寻址的位数组之后，容量不再跟某个整数类型的宽度绑死，上限变成
 * "位图本身有多少块"这个格式层面的真实约束——那条断言已经搬到
 * fs_format.h 里了（FS_NBLOCKS <= BSIZE * 8）。 */
#define BLKSET_BYTES ((FS_NBLOCKS + 7) / 8)

struct blkset {
    uint8_t bits[BLKSET_BYTES];
};

/* 位序约定跟磁盘位图完全一致：块 b 是第 b/8 字节的第 b%8 位，最低位是
 * bit 0。刻意跟磁盘格式取同一个约定，read_bitmap_set() 就可以直接把磁盘
 * 上那 64 个字节 memcpy 进来，不需要逐位搬运——两处约定一致带来的化简，
 * 反过来也是"如果哪天想改内存里的位序"的一个明确的代价提示。 */
static int blkset_test(const struct blkset *s, uint32_t b)
{
    if (b >= FS_NBLOCKS) {
        kprintf("blkset_test: 块号 %u 超出集合容量 %u\n", b, (uint32_t)FS_NBLOCKS);
        panic("blkset_test: 块号越界");
    }
    return (s->bits[b / 8] >> (b % 8)) & 1u;
}

static void blkset_set(struct blkset *s, uint32_t b)
{
    if (b >= FS_NBLOCKS) {
        kprintf("blkset_set: 块号 %u 超出集合容量 %u\n", b, (uint32_t)FS_NBLOCKS);
        panic("blkset_set: 块号越界");
    }
    s->bits[b / 8] |= (uint8_t)(1u << (b % 8));
}

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
/* TODO 1：实现 read_inode。
 *
 * 为什么：inode 号是文件系统内部唯一的"文件身份"，而 inode 本身躺在磁盘
 * 上的 inode 表里。这个函数是"号 -> 内容"的唯一入口，后面 fs_size /
 * fs_read / fs_lookup / 挂载自检全都经过它。
 *
 * 要做三件事：
 *   1. 没挂载就 panic（检查 fs_mounted）——把"忘了调 fs_init()"变成一句
 *      明确的错误，而不是"每个文件都查不到"。
 *   2. 校验 inum 落在 1..sb.ninodes-1，越界先 kprintf 出具体数值再 panic。
 *   3. 换算出块号和块内下标，blk_read() 出那一块，memcpy 出第 idx 个
 *      dinode 到 *out。
 *
 * 换算公式（跟 mkfs.c 的 inode_at() 必须一致，两边都从 fs_format.h 取
 * INODES_PER_BLOCK，算错的症状是"读出来的是隔壁 inode 的内容"）：
 *     块号     = sb.inodestart + inum / INODES_PER_BLOCK
 *     块内下标 = inum % INODES_PER_BLOCK
 *
 * 注意用 sb.inodestart（从磁盘超级块读出来的）而不是 FS_INODESTART
 * （编译期常量）：布局信息的唯一权威来源是磁盘上的超级块，内核不硬编码
 * 布局。用常量的话超级块里那几个 *start 字段就成了死字段，写了没人读。
 *
 * 缓冲区必须是栈上的局部数组（uint8_t buf[BSIZE]），不能是 static——
 * 理由见上面这段文档注释里关于时钟中断 + yield() 的说明。
 *
 * 提示：
 * static void read_inode(uint32_t inum, struct dinode *out)
 * {
 *     if (!fs_mounted) {
 *         panic("read_inode: 文件系统还没挂载，先调用 fs_init()");
 *     }
 *     if (inum == 0 || inum >= sb.ninodes) {
 *         kprintf("read_inode: inode 号 %u 无效（有效范围 1..%u）\n",
 *                 inum, sb.ninodes - 1);
 *         panic("read_inode: inode 号越界");
 *     }
 *
 *     uint32_t blk = sb.inodestart + inum / INODES_PER_BLOCK;
 *     uint32_t idx = inum % INODES_PER_BLOCK;
 *
 *     uint8_t buf[BSIZE];
 *     blk_read(blk, buf);
 *     memcpy(out, buf + idx * sizeof(struct dinode), sizeof(struct dinode));
 * }
 */

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
/* TODO 2：实现 name_eq。
 *
 * 为什么：目录项的名字是定宽 DIRSIZ 字符数组，名字正好 DIRSIZ 长时*没有*
 * 结尾 '\0'，所以不能把它交给 strcmp（会一路读进下一个目录项的 inum
 * 字节）。上面那段文档注释完整解释了为什么手写一个循环比 strncmp 更能
 * 把这个约束表达出来——先读它，再动手。
 *
 * 循环写法（i 从 0 到 DIRSIZ-1）：
 *   - want[i] == '\0'：want 结束了。只有 disk_name[i] 也是 '\0' 才相等
 *     （此时 i < DIRSIZ，读 disk_name[i] 一定还在 name 字段内部，安全）。
 *   - disk_name[i] != want[i]：不相等，直接返回 0。
 *   - 循环正常跑完（前 DIRSIZ 个字符全同、want 一直没结束）：只有 want
 *     正好 DIRSIZ 个字符长才算相等，也就是 return want[DIRSIZ] == '\0'。
 *
 * 最后那个 want[DIRSIZ] 的下标看着危险，其实安全：能走到那里说明 want
 * 至少有 DIRSIZ 个非 '\0' 字符，第 DIRSIZ 个下标要么是 '\0' 要么是更多
 * 字符，都在 want 这个 C 字符串的合法范围内。
 *
 * 提示：
 * static int name_eq(const char *disk_name, const char *want)
 * {
 *     for (uint32_t i = 0; i < DIRSIZ; i++) {
 *         if (want[i] == '\0') {
 *             return disk_name[i] == '\0';
 *         }
 *         if (disk_name[i] != want[i]) {
 *             return 0;
 *         }
 *     }
 *     return want[DIRSIZ] == '\0';
 * }
 */

/* TODO 3：实现 fs_size。
 *
 * 为什么：一个三行的热身，但它逼你确认 read_inode() 的契约——调用者提供
 * 缓冲区（栈上的 struct dinode），被调用者填进去。写完这个再去写 fs_read
 * 会顺很多。
 *
 * 提示：
 * uint32_t fs_size(uint32_t inum)
 * {
 *     struct dinode di;
 *     read_inode(inum, &di);
 *     return di.size;
 * }
 */

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
/* TODO 4：实现 fs_read。这是整个 Lab 里最值得慢慢写的一个函数。
 *
 * 为什么：它是"文件内偏移 -> 磁盘块号 + 块内偏移"这个翻译的唯一实现，
 * 上面那段文档注释里的两个边界条件就是这个 Lab 的考点本身，fsroot/ 下
 * 专门放了 hello.txt（98 字节，不足一块）和 exact.txt（正好 512 字节）
 * 两个文件来打这两个边界。
 *
 * 顺序：
 *   1. read_inode() 拿到 dinode；di.type == 0 说明读了个空槽位，kprintf
 *      出 inum 再 panic（元数据不一致，不是用户错误）。
 *   2. off >= di.size 直接 return 0（EOF）。**必须在做减法之前判断**：
 *      off 和 size 都是无符号数，off > size 时 size - off 会下溢成一个
 *      巨大的正数，循环会一直读下去。这是这类偏移计算里最常见的 bug。
 *   3. 把 n 截断到 di.size - off（边界 1：按 inode.size 截断，不是"读满
 *      整块"，否则最后一块的填充字节会被当成文件内容交给用户程序）。
 *   4. while (done < n) 循环——**循环条件是剩余字节数，不是"还有块可读"**
 *      （边界 2）。每轮：
 *        pos  = off + done
 *        bi   = pos / BSIZE   第几个直接块
 *        boff = pos % BSIZE   块内偏移
 *        bi >= NDIRECT        -> kprintf + panic（size 与 addrs 不一致）
 *        di.addrs[bi] == 0    -> kprintf + panic（文件中间有洞）
 *        blk_read(addr, buf) 到栈上的 buf，再取
 *        chunk = min(BSIZE - boff, n - done) 字节 memcpy 到 out + done。
 *   5. 返回 done。
 *
 * 那两个 panic 都不要写成"静默返回短读"：短读会让上层以为"文件就这么
 * 长"，把一个元数据损坏问题伪装成正常结果。
 *
 * 返回 0 表示 EOF，跟 POSIX read(2) 一致，不是错误——用户程序靠它结束
 * 读取循环。
 *
 * 提示：
 * uint32_t fs_read(uint32_t inum, uint32_t off, void *dst, uint32_t n)
 * {
 *     struct dinode di;
 *     read_inode(inum, &di);
 *
 *     if (di.type == 0) {
 *         kprintf("fs_read: inode %u 是空槽位（type==0）\n", inum);
 *         panic("fs_read: 读了一个未分配的 inode，文件系统元数据不一致");
 *     }
 *
 *     if (off >= di.size) {
 *         return 0;
 *     }
 *     uint32_t remaining = di.size - off;
 *     if (n > remaining) {
 *         n = remaining;
 *     }
 *
 *     uint8_t *out = dst;
 *     uint32_t done = 0;
 *     uint8_t buf[BSIZE];
 *
 *     while (done < n) {
 *         uint32_t pos = off + done;
 *         uint32_t bi = pos / BSIZE;
 *         uint32_t boff = pos % BSIZE;
 *
 *         if (bi >= NDIRECT) {
 *             kprintf("fs_read: inode %u 的偏移 %u 落在第 %u 个直接块，超出 NDIRECT=%u\n",
 *                     inum, pos, bi, (uint32_t)NDIRECT);
 *             panic("fs_read: 文件偏移超出直接块能表达的范围（inode.size 与 addrs 不一致）");
 *         }
 *
 *         uint32_t addr = di.addrs[bi];
 *         if (addr == 0) {
 *             kprintf("fs_read: inode %u 的第 %u 个直接块是 0（文件中间有洞）\n",
 *                     inum, bi);
 *             panic("fs_read: inode 在 size 范围内有未分配的块，镜像不一致");
 *         }
 *
 *         blk_read(addr, buf);
 *
 *         uint32_t chunk = BSIZE - boff;
 *         if (chunk > n - done) {
 *             chunk = n - done;
 *         }
 *         memcpy(out + done, buf + boff, chunk);
 *         done += chunk;
 *     }
 *
 *     return done;
 * }
 */

/* 在根目录里按名字查找。找不到返回 0（0 是永久保留的无效 inode 号，
 * 见 fs_format.h 里 ROOTINO 的注释）。
 *
 * Lab9 相对 Lab8 只多了一件事：跳过开头的 '/'。这样 shell 里敲 /ls 和
 * ls 都能找到同一个文件，用户程序里写 open("/motd.txt") 也能工作。
 *
 * 这*不是*路径解析，区别要说清楚：真正的路径解析是一个循环——按 '/' 切
 * 分，每一段在"当前目录的 inode"里查找，把结果作为下一段的起点，还要
 * 处理 "." ".." 、连续的斜杠、结尾的斜杠、符号链接、以及"中间某段不是
 * 目录"的错误。本 Lab 的目录结构是扁平的（只有根目录），所以"/ls" 里
 * 那个 '/' 唯一的含义就是"从根开始"，而这里本来就只从根开始找，于是跳过
 * 它就够了。给 "/a/b" 这种输入，本函数会拿 "a/b" 这整个字符串去跟目录项
 * 比较，然后返回 0（找不到）——不是崩溃，但也不是正确的语义。
 *
 * 之所以要点明这一点：跳过一个字符和实现路径解析在代码量上差了两个数量
 * 级，但在"看起来能用"上差别很小，很容易让人以为已经支持子目录了。分层
 * 路径解析是本 Lab 的挑战任务（见 README）。 */
uint32_t fs_lookup(const char *name)
{
    /* 允许若干个前导 '/'："//ls" 在 POSIX 里也是合法写法（"/" 开头的
     * 连续斜杠会被折叠）。用 while 而不是一个 if，代价是零，省掉的是
     * 一类"多打了一个斜杠就找不到文件"的困惑。 */
    while (*name == '/') {
        name++;
    }

    /* 全是斜杠（"/"、"//"）指的是根目录本身。不特判的话会拿空字符串去跟
     * 每个目录项比较、一个都匹配不上、返回 0（找不到）——而 ls / 是个很
     * 自然的输入，让它报"没有这个文件"很别扭。
     *
     * 注意根目录里*确实*有一个名字叫 "." 的目录项指向 ROOTINO，所以这一
     * 步严格说是冗余的（fs_lookup(".") 本来就能查到根）。写出来是为了让
     * "空路径 = 根"这个约定不依赖"mkfs 恰好写了 . 这一项"。 */
    if (*name == '\0') {
        return ROOTINO;
    }

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
     * 目录只有十几项，完全无所谓；真实内核正是为了消掉这个重复开销才
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
 * 分成两个独立的辅助函数、各自在内部用完就释放那个 512 字节块缓冲区，
 * 而不是在一个函数里同时持有两个：内核栈是有限的，见 read_inode() 关于
 * 栈预算的说明。 */
/* TODO 5：实现 collect_referenced_blocks。
 *
 * 为什么：算出"按 inode 的说法，哪些块被占用了"这个集合，作为跟磁盘位图
 * 比对的一方。Lab8 这里返回一个 uint64_t 当位集合；Lab9 把结果写进调用者
 * 提供的 struct blkset *out（原因见上面 struct blkset 的说明：FS_NBLOCKS
 * 已经涨到 512，装不进一个整数类型了）。
 *
 * 三部分：
 *   0. memset(out, 0, sizeof(*out)) 清零——struct blkset 是栈上局部变量，
 *      不清零的话前面的垃圾值会被误当成"已置位"。
 *   1. 元数据块恒定被占用：[0, sb.datastart) 全部 blkset_set(out, b)
 *      （超级块 + 位图 + inode 表）。
 *   2. 遍历 inum 从 ROOTINO 到 sb.ninodes-1，read_inode() 每一个；
 *      di.type == 0 跳过；否则遍历它的 NDIRECT 个 addrs：
 *        a == 0                             -> 跳过
 *        a < datastart 或 a >= nblocks      -> kprintf + panic（越界引用）
 *        blkset_test(out, a) 已经为真        -> kprintf + panic（两个 inode
 *                                              引用同一块，block aliasing）
 *        否则 blkset_set(out, a)。
 *
 * 为什么扫整张 inode 表、而不是从根目录递归下去：本 Lab 目录是扁平的，
 * 两种走法结果相同，但扫全表还能发现"被分配了却没有任何目录项指向它"
 * （孤儿 inode）所占用的块——真实 fsck 也是扫全表 + 另外单独查可达性。
 *
 * 提示：
 * static void collect_referenced_blocks(struct blkset *out)
 * {
 *     memset(out, 0, sizeof(*out));
 *     for (uint32_t b = 0; b < sb.datastart; b++) {
 *         blkset_set(out, b);
 *     }
 *
 *     for (uint32_t inum = ROOTINO; inum < sb.ninodes; inum++) {
 *         struct dinode di;
 *         read_inode(inum, &di);
 *         if (di.type == 0) {
 *             continue;
 *         }
 *         for (uint32_t i = 0; i < NDIRECT; i++) {
 *             uint32_t a = di.addrs[i];
 *             if (a == 0) {
 *                 continue;
 *             }
 *             if (a < sb.datastart || a >= sb.nblocks) {
 *                 kprintf("fs 自检: inode %u 引用了非法块号 %u（合法数据块范围 [%u, %u)）\n",
 *                         inum, a, sb.datastart, sb.nblocks);
 *                 panic("fs 自检: inode 引用了数据区以外的块");
 *             }
 *             if (blkset_test(out, a)) {
 *                 kprintf("fs 自检: 块 %u 被两个 inode 同时引用（inode %u 又引用了它）\n",
 *                         a, inum);
 *                 panic("fs 自检: 数据块被重复引用（block aliasing）");
 *             }
 *             blkset_set(out, a);
 *         }
 *     }
 * }
 */

/* TODO 6：实现 read_bitmap_set。
 *
 * 为什么：把磁盘上的位图块读成同样形式的 struct blkset，作为比对的
 * 另一方。顺带校验一件格式层面的事。
 *
 * 三部分：
 *   1. blk_read(sb.bitmapstart, buf) 到栈上缓冲区，然后 memset(out, 0,
 *      sizeof(*out)) 清零，memcpy(out->bits, buf, BLKSET_BYTES) 整段拷贝。
 *      **能这样整段拷贝的前提是 struct blkset 的位序约定跟磁盘位图完全
 *      一致**（块 b 对应第 b/8 字节的第 b%8 位，最低位是 bit 0）——这必须
 *      跟 mkfs.c 的 bitmap_mark() 完全一致，不一致的话这个自检会失败，
 *      这正是它该有的行为：位序分歧必须在挂载时就发现，不该等到有人
 *      实现写路径时才暴露。
 *   2. 补一条静态断言 _Static_assert(BLKSET_BYTES <= BSIZE, ...)：位图
 *      只有一块，这条断言守的是"这次 memcpy 不越界读 buf"（跟 fs_format.h
 *      里 FS_NBLOCKS <= BSIZE * 8 是同一件事的两种写法，两处守的是两个
 *      不同的东西）。
 *   3. sb.nblocks 以外的位应该全是 0（见 fs_format.h 的位图说明）。遍历
 *      整个 BSIZE 字节 × 8 位，跳过 b < sb.nblocks 的，剩下的一旦发现
 *      置位就 kprintf + panic。这些位不影响任何功能，但它们非 0 说明
 *      mkfs 和格式定义对"位图有多长"的理解不一致，值得当成错误。
 *
 * 提示：
 * static void read_bitmap_set(struct blkset *out)
 * {
 *     uint8_t buf[BSIZE];
 *     blk_read(sb.bitmapstart, buf);
 *
 *     memset(out, 0, sizeof(*out));
 *     memcpy(out->bits, buf, BLKSET_BYTES);
 *
 *     _Static_assert(BLKSET_BYTES <= BSIZE,
 *                    "块集合比一个位图块还大，read_bitmap_set() 的 memcpy 会越界");
 *
 *     for (uint32_t byte = 0; byte < BSIZE; byte++) {
 *         for (uint32_t bit = 0; bit < 8; bit++) {
 *             uint32_t b = byte * 8 + bit;
 *             if (b < sb.nblocks) {
 *                 continue;
 *             }
 *             if (buf[byte] & (1u << bit)) {
 *                 kprintf("fs 自检: 位图里第 %u 位被置位，但总共只有 %u 块\n",
 *                         b, sb.nblocks);
 *                 panic("fs 自检: 位图在 nblocks 之外有置位");
 *             }
 *         }
 *     }
 * }
 */

/* 把上面两个集合比一下。相等就打一行 self-check ok（tests/expect-*.txt
 * 里有这一行，格式照抄）；不相等就把**差异的两个方向**分别列出来再
 * panic——只说"不一致"是没法排查的：
 *   位图标了但没有 inode 引用的块（空间泄露）
 *   被 inode 引用但位图没标的块（会被重复分配）
 * 真实 fsck 报告长的就是这个样子。
 *
 * 两个 struct blkset（各 64 字节，Lab8 是两个 uint64_t，16 字节）都在
 * 栈上，加起来 128 字节。栈预算见 read_inode() 的说明：本 Lab 把内核栈
 * 从 1 页加到了 2 页，这点开销完全不构成压力。 */
static void fs_check_bitmap(void)
{
    struct blkset referenced;
    struct blkset on_disk;

    collect_referenced_blocks(&referenced);
    read_bitmap_set(&on_disk);

    /* struct blkset 里只有一个定长数组，没有 padding 问题，可以直接
     * memcmp 整块比较，不用逐位循环。 */
    if (memcmp(&referenced, &on_disk, sizeof(referenced)) != 0) {
        kprintf("fs 自检失败: 位图 vs inode 引用不一致\n");
        kprintf("  位图标了但没有 inode 引用的块(空间泄露):");
        for (uint32_t b = 0; b < sb.nblocks; b++) {
            if (blkset_test(&on_disk, b) && !blkset_test(&referenced, b)) {
                kprintf(" %u", b);
            }
        }
        kprintf("\n  被 inode 引用但位图没标的块(会被重复分配):");
        for (uint32_t b = 0; b < sb.nblocks; b++) {
            if (blkset_test(&referenced, b) && !blkset_test(&on_disk, b)) {
                kprintf(" %u", b);
            }
        }
        kprintf("\n");
        panic("fs 自检: 位图与 inode 引用不一致，镜像已损坏（用 mkfs 重新生成）");
    }

    uint32_t used = 0;
    for (uint32_t b = 0; b < sb.nblocks; b++) {
        if (blkset_test(&on_disk, b)) {
            used++;
        }
    }
    kprintf("fs: bitmap self-check ok, %u/%u blocks in use\n", used, sb.nblocks);
}

/* 这是整个文件系统的入口。"挂载"这个动作的实质就是：读块 0、确认它真的
 * 是一个本课程格式的超级块、把布局信息记下来，然后允许上层开始用 inode
 * 号说话。
 *
 * Lab9 相对 Lab8 在这个函数里没有新的决策点：镜像变大、inode 变大都只是
 * 常量和格式定义的变化，挂载流程本身不变。唯一值得一提的是魔数不匹配时
 * 多了一条诊断：如果读到的正好是 Lab8 的魔数，说明挂错了镜像，这种情况
 * 单独提示比让人对着一串十六进制自己猜要快。 */
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
        kprintf("fs_init: 超级块魔数不对：读到 0x%x，期望 0x%x ('LAB9')\n",
                sb.magic, (uint32_t)FS_MAGIC);
        kprintf("  最可能的原因：给 QEMU 挂的磁盘镜像不是 mkfs 生成的，或者根本没挂上。\n");
        kprintf("  先确认 build/fs.img 存在且是 262144 字节：make fs.img\n");
        /* 一个 Lab9 特有的可能：读到 0x3842414c（'LAB8'）说明挂的是 Lab8
         * 的镜像。本 Lab 的 inode 是 256 字节而 Lab8 是 64 字节，格式不
         * 兼容——这正是魔数要跟着格式一起改的原因，见 fs_format.h。 */
        if (sb.magic == 0x3842414Cu) {
            kprintf("  读到的是 'LAB8'：这是 Lab8 的镜像，格式不兼容（inode 大小变了）。\n");
            kprintf("  在 lab09 目录下重新生成：make fs.img\n");
        }
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
