/* Lab8 / x86_64：ATA PIO 硬盘驱动——blk_read() 的 x86 侧实现。
 *
 * PIO = Programmed I/O：CPU 亲自把每一个字从设备寄存器里搬出来。读一个
 * 512 字节的扇区要执行 256 次 insw，每次 insw 都是一次总线事务。这是
 * 1980 年代 IBM PC/AT 就有的接口，慢得惊人（真机上几 MB/s），但它有一个
 * 对本 Lab 决定性的优点：不需要任何共享内存结构、不需要 DMA、不需要
 * 物理地址翻译，一共只用到 8 个 I/O 端口。整个驱动 100 行以内，可以逐行
 * 讲清楚每一次端口读写在跟硬件说什么。
 *
 * riscv64 那边的 virtio-blk 是另一个极端：CPU 在内存里建好描述符环，
 * 写一个寄存器通知设备，设备自己 DMA 搬数据。快得多，但要先理解三块
 * 共享内存结构的布局。两者的对照是本 Lab 的主线之一，见 blk.h。
 *
 * 本驱动的简化（README 简化清单会再列）：
 *   - 只读，不实现写命令（0x30）。
 *   - 只支持 LBA28（命令 0x20），能寻址 128 GiB，本 Lab 的镜像 32KiB。
 *   - 只用主总线主盘（primary master），不做总线/盘位枚举。
 *   - 一次一个扇区，不用 READ MULTIPLE。
 *   - 轮询而不是中断，理由见 blk.h。
 */
#include "types.h"
#include "console.h"
#include "panic.h"

#include "fs_format.h"
#include "blk.h"

/* ATA 主总线的 I/O 端口。这些地址不是探测出来的，是 PC/AT 体系约定死的
 * "legacy" 地址——跟 0x3F8 是串口、0x20/0xA0 是 PIC 一样，属于 IBM PC
 * 兼容机的历史遗产，所有 x86 虚拟机和芯片组都还在兼容它。现代系统上
 * 正确的做法是通过 PCI 配置空间找到 AHCI 控制器的 BAR，但那需要先有
 * PCI 枚举代码。 */
#define ATA_IO_BASE    0x1F0   /* 主总线的 I/O 寄存器组起始 */

#define ATA_REG_DATA   (ATA_IO_BASE + 0) /* 16 位数据口：扇区内容从这里流出 */
#define ATA_REG_ERROR  (ATA_IO_BASE + 1) /* 读：错误码（仅 ERR 置位时有意义）*/
#define ATA_REG_SECCNT (ATA_IO_BASE + 2) /* 要传输的扇区数 */
#define ATA_REG_LBA_LO (ATA_IO_BASE + 3) /* LBA 位 0..7 */
#define ATA_REG_LBA_MID (ATA_IO_BASE + 4) /* LBA 位 8..15 */
#define ATA_REG_LBA_HI (ATA_IO_BASE + 5) /* LBA 位 16..23 */
#define ATA_REG_DRIVE  (ATA_IO_BASE + 6) /* 盘选 + LBA 位 24..27 + LBA 模式位 */
#define ATA_REG_CMD    (ATA_IO_BASE + 7) /* 写：命令；读：状态（会清中断）*/

/* 0x3F6 这个端口*读写是两个不同的寄存器*——这是 ATA 接口里最容易看漏
 * 的一处设计：
 *   读 = Alternate Status（备用状态）：内容跟 0x1F7 相同，但读它*不会*
 *        清掉设备的中断挂起状态。轮询时应该读这个口——读 0x1F7 会顺带
 *        确认中断，如果以后你改成中断驱动，轮询代码就会偷偷吃掉中断，
 *        表现为"中断处理程序永远不触发"。
 *   写 = Device Control（设备控制）：里面有 nIEN 位，用来关掉设备中断。
 * 同一个地址、按方向区分功能，在老硬件上很常见（PIC 的 0x20 也是这样）。 */
#define ATA_REG_ALTSTATUS 0x3F6  /* 读 */
#define ATA_REG_DEVCTL    0x3F6  /* 写 */

/* Device Control 的 nIEN 位：置 1 = 设备不要发中断（名字里的 n 是"低有效"
 * 的意思，置位表示 *禁止*）。
 *
 * 这一位是本 Lab 在 x86_64 上踩到的第一个真实崩溃的根因，值得完整记下来
 * （README 的"常见坑"会引用这段）：
 *
 * 症状：blk_init() 打印成功，紧接着 fs_init() 什么都不打印，内核像是卡死
 * 了——但 ide.c 的每个轮询循环都有 ATA_POLL_LIMIT 上限，卡死时本该 panic，
 * 却连 panic 都没有。
 *
 * 真相：它不是卡死，是三重故障。QEMU 带 -no-reboot -no-shutdown 时三重
 * 故障会让虚拟机停在原地、不退出、不输出，和"死循环"长得一模一样（这正是
 * docs/verification-methodology.md 反复强调的那个陷阱）。按那份文档的方法，
 * 加 -d int 重跑，日志里是：
 *     v=0d e=0172 ... IP=...  （#GP，General Protection）
 *     check_exception old: 0xd new 0xd  →  v=08（#DF）  →  Triple fault
 * #GP 的错误码 0x172：bit1=1 表示"这次故障跟 IDT 有关"，索引 0x172>>3 = 46。
 * 向量 46 = PIC 重映射后的 base 32 + IRQ 14，而 IRQ 14 正是 ATA 主通道的
 * 中断线。trap.c 里 IDT_ENTRIES 是 33（只到向量 32 = IRQ 0 的定时器），
 * 46 号槽位根本不存在 —— 于是"设备读完一个扇区、好心通知了一声"直接把
 * 内核打死了。
 *
 * 这个 bug 的教育意义在于它揭示了一个反直觉的事实：**轮询式驱动也必须
 * 处理中断**。很容易以为"我不读中断、不注册处理程序，中断就与我无关"，
 * 但中断是设备主动发起的，不注册处理程序不等于不会收到——只等于收到时
 * 死得更难看。凡是让设备执行一条会"完成"的命令，就要么给它一个处理程序，
 * 要么明确告诉它别发。 */
#define ATA_DEVCTL_NIEN 0x02

/* 状态寄存器的位。*/
#define ATA_SR_BSY  0x80  /* Busy：设备正在忙，其它所有位都无意义 */
#define ATA_SR_DRDY 0x40  /* Device Ready */
#define ATA_SR_DRQ  0x08  /* Data Request：有一整块数据等着你来取 */
#define ATA_SR_ERR  0x01  /* Error：细节在 ATA_REG_ERROR */

#define ATA_CMD_READ_SECTORS 0x20  /* READ SECTOR(S)，LBA28，PIO */

/* 轮询上限。选一个"大得正常情况绝不会撞上、小得撞上时能立刻返回"的值：
 * QEMU 的虚拟盘几乎是立即完成的，真机机械盘寻道最坏几十毫秒。这里的
 * 计数不是时间，是循环次数（一次 inb 大约几百纳秒到几微秒），一百万次
 * 足够覆盖真机最坏情况。
 *
 * 为什么必须有上限：没有上限的 while (status & BSY) 在"根本没有盘"的
 * 情况下是一个死循环——内核看起来就是启动到某一行然后永远卡住，什么都
 * 不打印。有上限就能变成一句说明原因的 panic。这是嵌入式/内核代码里
 * 一条通用纪律：所有等硬件的循环都要有退出路径。 */
#define ATA_POLL_LIMIT 1000000

/* 端口 I/O 原语。跟 console_putc.c / pit.c 一样在本文件里各写一份——
 * 本课程刻意不建一个公共的 io.h，这样每个驱动文件都是自包含的，读的时候
 * 不需要在头文件之间跳。 */
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* 从 16 位端口连续读 cnt 个字到内存。rep insw 一条指令干完 256 次读，
 * 比 C 循环调 inw 快得多，而且是这类驱动的惯用写法。
 *
 * 约束说明："+D"(addr) 是 rdi 且会被指令自己递增，"+c"(cnt) 是 rcx 且
 * 会被递减，所以两个都必须写成读写（"+"）操作数；漏掉 + 号的话编译器
 * 会认为 rdi/rcx 在指令后不变，可能在后面复用这两个寄存器的旧值。
 * "memory" clobber 告诉编译器这条汇编写了内存，不能把对缓冲区的后续
 * 读取优化到它前面去。 */
static inline void insw(uint16_t port, void *addr, uint32_t cnt)
{
    __asm__ volatile("rep insw"
                     : "+D"(addr), "+c"(cnt)
                     : "d"(port)
                     : "memory");
}

/* 读 4 次备用状态口，用来"等 400 纳秒"。
 *
 * ATA 规范要求：向命令口写完命令之后，设备需要最多 400ns 才会把状态
 * 寄存器里的 BSY 位立起来。在这段窗口里读状态，读到的是*上一条*命令
 * 结束时的状态——BSY=0、DRDY=1，看起来"设备已经就绪了"，于是代码会
 * 立刻去读数据口，拿到垃圾。
 *
 * 规范给出的标准做法就是读 4 次备用状态口再看：每次 ISA I/O 访问至少
 * 100ns，4 次凑够 400ns。这是一条纯粹的时序 hack，没有任何逻辑含义，
 * 但它是 ATA 驱动里最经典的坑之一，几乎每份 osdev 教程都会踩。
 *
 * 注意这里读的是 0x3F6 而不是 0x1F7——见 ATA_REG_ALTSTATUS 的注释。 */
static void ata_400ns_delay(void)
{
    for (int i = 0; i < 4; i++) {
        (void)inb(ATA_REG_ALTSTATUS);
    }
}

/* 等到 BSY 清零，然后要求 DRQ 置位（数据已就绪）。*/
static void ata_wait_data_ready(uint32_t blkno)
{
    uint8_t status = 0;

    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        status = inb(ATA_REG_ALTSTATUS);

        /* 0xFF 意味着"总线上什么都没有"：没有设备驱动这些线时，读出来
         * 的是全 1（浮空的总线被上拉）。这是"floating bus"，是判断
         * "根本没插盘"最可靠的信号。单独拎出来报错，因为它的原因跟
         * "盘在但出错了"完全不同：前者是 QEMU 命令行没挂 -drive，
         * 后者才需要看 ERROR 寄存器。 */
        if (status == 0xFF) {
            kprintf("ide: 读块 %u 时状态寄存器全 1（0xFF）——总线上没有设备\n", blkno);
            kprintf("  几乎一定是 QEMU 没有挂上硬盘。检查 run-qemu.sh 是否传了 -drive ...,if=ide\n");
            panic("ide: ATA 主总线上没有硬盘");
        }

        if (status & ATA_SR_BSY) {
            continue;   /* 还在忙，BSY 置位时其它位都不可信 */
        }
        if (status & ATA_SR_ERR) {
            uint8_t err = inb(ATA_REG_ERROR);
            kprintf("ide: 读块 %u 出错，status=0x%x error=0x%x\n",
                    blkno, (uint32_t)status, (uint32_t)err);
            panic("ide: ATA 读扇区命令返回错误");
        }
        if (status & ATA_SR_DRQ) {
            return;     /* 数据就绪，可以搬了 */
        }
        /* BSY=0、ERR=0、DRQ=0：命令还没走到"准备好数据"这一步，继续等。*/
    }

    kprintf("ide: 等块 %u 的数据超时（轮询 %u 次），最后 status=0x%x\n",
            blkno, (uint32_t)ATA_POLL_LIMIT, (uint32_t)status);
    panic("ide: 等待 ATA 设备超时");
}

/* 等到 BSY 清零、DRDY 置位——发命令之前要求的初始状态。*/
static void ata_wait_ready_for_command(void)
{
    uint8_t status = 0;

    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        status = inb(ATA_REG_ALTSTATUS);
        if (status == 0xFF) {
            kprintf("ide: 状态寄存器全 1（0xFF）——ATA 总线上没有设备\n");
            kprintf("  检查 QEMU 命令行是否挂了 -drive file=...,if=ide\n");
            panic("ide: ATA 主总线上没有硬盘");
        }
        if (status & ATA_SR_BSY) {
            continue;
        }
        if (status & ATA_SR_DRDY) {
            return;
        }
    }

    kprintf("ide: 等设备就绪超时，最后 status=0x%x\n", (uint32_t)status);
    panic("ide: ATA 设备始终没有进入就绪状态");
}

void blk_init(void)
{
    /* 第一件事：关掉设备中断。原因见 ATA_DEVCTL_NIEN 的长注释——不关的话
     * 第一次 blk_read() 完成时设备会拉起 IRQ 14，而 IDT 里没有 46 号向量，
     * 内核当场三重故障。必须在发出任何命令*之前*写这一位。 */
    outb(ATA_REG_DEVCTL, ATA_DEVCTL_NIEN);

    /* 第二道防线：在从片 8259A 上把 IRQ 14 屏蔽掉。
     *
     * 为什么两道都要：nIEN 是"请设备别发"，PIC mask 是"就算发了也别送到
     * CPU"。前者依赖设备守规矩、也依赖这一位不被后续代码覆盖（真实驱动里
     * 软复位 SRST 就会重置 Device Control，很容易一不小心把 nIEN 清掉）；
     * 后者是 CPU 这一侧的硬性拦截。对一个"这条线上的中断我们完全不打算
     * 处理"的驱动来说，两道一起上才是正确的姿势——任何一道单独存在都是
     * 一个隐含假设。
     *
     * 为什么这段代码在 ide.c 而不在 pit.c 的 PIC 初始化里：只有 IDE 驱动
     * 知道"IRQ 14 归我管、而我不处理它"。pit.c 的 pic_remap() 刻意保留了
     * 调用前的 mask（见那里的注释），不去假设别人要什么——把"我这条线要
     * 屏蔽"的决定放在拥有这条线的驱动里，是这两处注释共同表达的同一条
     * 原则。这也保持了 pit.c 在 Lab6/7/8 之间完全没变。
     *
     * 0x21 / 0xA1 是两片 8259A 的数据口，在 ICW 序列之后读写它们就是读写
     * IMR（中断屏蔽寄存器），bit N = 1 表示屏蔽第 N 条线。IRQ 14 是从片的
     * 第 6 条线（14 - 8 = 6）。 */
    uint8_t slave_mask = inb(0xA1);
    outb(0xA1, (uint8_t)(slave_mask | (1u << 6)));

    /* 然后确认盘真的在。把"没有盘"这件事在初始化时就报出来，而不是等到
     * fs_init() 去读超级块时才发现——那时的症状是"魔数不对"，会把人
     * 引到"镜像格式错了"这条错误的排查路线上。 */
    ata_wait_ready_for_command();
    kprintf("ide: ATA primary master ready, PIO LBA28 read-only, IRQ 14 disabled\n");
}

void blk_read(uint32_t blkno, void *buf)
{
    /* 本 Lab 的块大小恰好等于 ATA 扇区大小，所以"块号"直接就是 LBA。
     * 真实文件系统的块通常是 4KiB（8 个扇区），那时这里要做一次
     * 块号 -> 扇区号的换算，并一次读 8 个扇区。 */
    _Static_assert(BSIZE == 512, "本驱动假设文件系统块大小等于 ATA 扇区大小 512");

    if (blkno >= FS_NBLOCKS) {
        kprintf("blk_read: 块号 %u 超出镜像范围（共 %u 块）\n",
                blkno, (uint32_t)FS_NBLOCKS);
        panic("blk_read: 块号越界——上层算错了块号");
    }

    ata_wait_ready_for_command();

    /* 扇区数 = 1。0 在 ATA 里表示 256 个扇区，不是 0 个——一个很容易
     * 写错的边界。 */
    outb(ATA_REG_SECCNT, 1);

    /* LBA28 被拆成四段塞进四个 8 位寄存器：低 24 位占三个独立的口，
     * 剩下的高 4 位跟"盘选"和"LBA 模式"挤在同一个字节里。这种把一个
     * 整数撕成几块塞进不相干寄存器的做法，是老硬件接口的典型特征——
     * 寄存器是一代代加上去的，只能往缝隙里塞。 */
    outb(ATA_REG_LBA_LO,  (uint8_t)(blkno & 0xFF));
    outb(ATA_REG_LBA_MID, (uint8_t)((blkno >> 8) & 0xFF));
    outb(ATA_REG_LBA_HI,  (uint8_t)((blkno >> 16) & 0xFF));

    /* 0xE0 = 0b1110_0000：
     *   bit 7 = 1  固定为 1（历史遗留）
     *   bit 6 = 1  LBA 模式（0 表示上古的 CHS 柱面/磁头/扇区寻址）
     *   bit 5 = 1  固定为 1（历史遗留）
     *   bit 4 = 0  选主盘（1 是从盘）
     *   bit 3..0   LBA 的第 24..27 位
     * 忘了置 bit 6 是个经典错误：设备会把你写进去的 LBA 当成 CHS 地址
     * 解释，读回来的是磁盘上完全不相干的位置——不报错，只是数据不对。 */
    outb(ATA_REG_DRIVE, (uint8_t)(0xE0 | ((blkno >> 24) & 0x0F)));

    outb(ATA_REG_CMD, ATA_CMD_READ_SECTORS);

    ata_400ns_delay();
    ata_wait_data_ready(blkno);

    /* 256 个 16 位字 = 512 字节。数据口是 16 位的，所以单位是字不是字节
     * ——按 512 传会读出两个扇区的量并越界写 256 字节，是这里最容易犯的
     * 错。 */
    insw(ATA_REG_DATA, buf, BSIZE / 2);
}
