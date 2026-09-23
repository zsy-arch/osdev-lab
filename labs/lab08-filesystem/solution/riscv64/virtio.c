/* Lab8 / riscv64：virtio-blk MMIO 驱动——blk_read() 的 riscv 侧实现。
 *
 * 和 x86_64 的 ide.c 是同一个 blk.h 接口的两种完全不同的实现方式，这个
 * 对照是本 Lab 的主线之一：
 *
 *   ide.c（ATA PIO）      : CPU 写 8 个端口说"读第 N 个扇区"，然后 CPU
 *                           亲自用 256 次 insw 把数据搬进内存。
 *                           数据流：设备 -> CPU 寄存器 -> 内存。
 *   virtio.c（本文件）    : CPU 在内存里写好一张"我要什么"的描述符表，
 *                           写一个寄存器通知设备，设备自己把数据 DMA 进
 *                           内存。数据流：设备 -> 内存（CPU 不经手）。
 *
 * 第二种是现代设备的通用形态（NVMe、网卡、GPU 全是这个结构），代价是
 * 要先理解三块共享内存结构。本文件的大部分篇幅都在解释这三块结构。
 *
 * 为什么 riscv 不能照抄 ide.c：riscv 没有"端口 I/O"这个概念——它没有
 * in/out 指令，所有设备寄存器都是内存映射的（MMIO），用普通的 load/store
 * 访问。而 virt 机器上根本没有 ATA 控制器这个设备可以访问。
 *
 * 本驱动的简化（README 简化清单会再列）：
 *   - 只读，不实现写请求（VIRTIO_BLK_T_OUT）。
 *   - 队列里一次只放一个请求，放完就等它完成（不利用环形队列的并发能力）。
 *   - 轮询 used 环，不用中断（和 ide.c 一致，理由见 blk.h）。
 *   - 不做特性协商的实质内容：只把设备给的特性位原样接受一部分，
 *     不启用任何可选特性。
 */
#include "types.h"
#include "console.h"
#include "panic.h"
#include "string.h"

#include "fs_format.h"
#include "blk.h"

/* 内核虚拟地址 = 物理地址 + 这个偏移（kernel_main.c 建立的自映射）。
 * 本文件需要它做一件 ide.c 完全不需要做的事：把虚拟地址翻译成物理地址。
 *
 * 为什么必须翻译：描述符里填的地址是**设备**要用的。设备做 DMA 时不经过
 * CPU 的 MMU，它看到的是物理地址空间。内核代码手里拿的却全是虚拟地址
 * （内核跑在 0xFFFFFFC0_00000000 之上）。把虚拟地址直接填进描述符，设备
 * 会去访问一个天文数字的物理地址，结果是数据永远不出现在你以为的地方。
 *
 * 这是"CPU 视角"和"设备视角"第一次在本课程里分裂。端口 I/O 的 ide.c 没有
 * 这个问题，因为数据是 CPU 亲自搬的，从来没有第二个视角参与。 */
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000ull

static inline uintptr_t virt_to_phys(const void *va)
{
    return (uintptr_t)va - KERNEL_VIRT_BASE;
}

/* ---- virtio-mmio 传输层寄存器 ------------------------------------------
 *
 * virt 机器上有 8 个 virtio-mmio 传输通道，地址是 0x10001000 开始、每个
 * 占 0x1000（实测：qemu -machine virt,dumpdtb 导出的设备树里有 8 个
 * virtio_mmio@1000{1..8}000 节点）。每个通道可以挂一个 virtio 设备，
 * 也可以是空的。
 *
 * 下面的偏移量来自 virtio 1.0 规范第 4.2.2 节 "MMIO Device Register
 * Layout"。规范里这些寄存器全部是 32 位的，必须用 32 位访问。 */
#define VIRTIO_MMIO_BASE      0x10001000ull
#define VIRTIO_MMIO_STRIDE    0x1000ull
#define VIRTIO_MMIO_SLOTS     8

#define VIRTIO_MMIO_MAGIC_VALUE      0x000 /* 读：应该是 0x74726976 */
#define VIRTIO_MMIO_VERSION          0x004 /* 读：1=legacy，2=现代 */
#define VIRTIO_MMIO_DEVICE_ID        0x008 /* 读：设备类型，2=块设备，0=空槽 */
#define VIRTIO_MMIO_VENDOR_ID        0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010 /* 读：设备支持的特性位 */
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020 /* 写：驱动接受的特性位 */
#define VIRTIO_MMIO_QUEUE_SEL        0x030 /* 写：后续队列寄存器操作哪个队列 */
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034 /* 读：这个队列最多能有多少项 */
#define VIRTIO_MMIO_QUEUE_NUM        0x038 /* 写：我实际用多少项 */
#define VIRTIO_MMIO_QUEUE_READY      0x044 /* 写 1：这个队列配好了，可以用 */
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050 /* 写队列号：我往这个队列放了新请求 */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS           0x070 /* 读写：设备初始化状态机 */
/* 三块队列内存各自的物理地址，拆成低 32 位/高 32 位两个寄存器写入
 * （因为 MMIO 寄存器都是 32 位的，而地址是 64 位）。
 * 这组寄存器是版本 2 才有的——legacy（版本 1）只有一个 QueuePFN，要求
 * 三块内存必须连成一整块、按页对齐，见 run-qemu.sh 里 force-legacy 的注释。 */
#define VIRTIO_MMIO_QUEUE_DESC_LOW   0x080 /* 描述符表 */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH  0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090 /* available 环（驱动写、设备读）*/
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0a0 /* used 环（设备写、驱动读）*/
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4

#define VIRTIO_MAGIC        0x74726976u /* 小端的 "virt" */
#define VIRTIO_VERSION_MODERN 2u
#define VIRTIO_DEVICE_ID_BLOCK 2u

/* Status 寄存器的位。这不是一个"当前状态"的枚举，而是一组**累积**的标志：
 * 驱动按顺序一位一位往上叠，设备靠它知道驱动走到哪一步了。顺序不能乱，
 * 也不能跳——规范第 3.1.1 节把这个顺序定成了必须遵守的初始化协议。 */
#define VIRTIO_STATUS_ACKNOWLEDGE 1u /* 我看见你了 */
#define VIRTIO_STATUS_DRIVER      2u /* 我有能驱动你的驱动 */
#define VIRTIO_STATUS_DRIVER_OK   4u /* 驱动就绪，可以开始干活 */
#define VIRTIO_STATUS_FEATURES_OK 8u /* 特性协商完成 */

/* ---- 描述符环（virtqueue）的三块内存 ----------------------------------
 *
 * 一个 virtqueue 由三块共享内存组成。理解这三块各自"谁写谁读"是理解整个
 * virtio 的关键：
 *
 *   1. 描述符表 desc[]：一个数组，每一项描述"一段内存缓冲区"（物理地址 +
 *      长度 + 标志）。多项可以用 next 字段串成链表，表示"这个请求由这几段
 *      缓冲区组成"。驱动写，设备读。
 *      注意它**不是**队列——它是一个缓冲池，项的下标就是它的名字。
 *
 *   2. available 环 avail：驱动用它说"desc 里第 N 号链表现在归你了"。
 *      驱动写 idx，设备读 idx。
 *
 *   3. used 环 used：设备用它说"desc 里第 N 号链表我处理完了"。
 *      设备写 idx，驱动读 idx。
 *
 * 两个环都是"只增不减的 16 位计数器 idx + 一个环形数组"。谁生产谁递增
 * 自己那个 idx，消费方记住上次看到的值，两者不等就说明有新东西。这个
 * 模式（单生产者单消费者的无锁环形队列）在 virtio 之外也到处都是。
 *
 * 为什么要两个环而不是一个：因为提交和完成是**异步**的，而且完成顺序
 * 不保证等于提交顺序（真实设备会重排以优化寻道）。两个独立的环让双方
 * 各自只写自己那一侧，不需要任何锁。 */
#define VIRTQ_QUEUE_SIZE 8 /* 队列项数，必须是 2 的幂。本驱动一次只用 1 项，
                            * 但队列本身不能只有 1 项（规范要求 2 的幂，且
                            * QueueNumMax 通常远大于 1，取 8 足够小又合法）。*/

/* 描述符的 flags 位。 */
#define VIRTQ_DESC_F_NEXT  1 /* 这一项后面还有，看 next 字段 */
#define VIRTQ_DESC_F_WRITE 2 /* 这段缓冲区是**设备往里写**的
                              * （从设备的角度命名，别被绕进去：读磁盘时
                              *  数据缓冲区要设的正是 WRITE，因为是设备写
                              *  内存。这是 virtio 里最容易设反的一位）*/

struct virtq_desc {
    uint64_t addr;  /* 缓冲区的**物理**地址 */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;                      /* 驱动下一次要写的位置（只增）*/
    uint16_t ring[VIRTQ_QUEUE_SIZE];   /* 每项是一个 desc 下标 */
    uint16_t used_event;               /* 本驱动不用（事件抑制特性）*/
};

struct virtq_used_elem {
    uint32_t id;  /* 完成的那条描述符链的头部下标 */
    uint32_t len; /* 设备实际写了多少字节 */
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;                                  /* 设备已完成的总数（只增）*/
    struct virtq_used_elem ring[VIRTQ_QUEUE_SIZE];
    uint16_t avail_event;                          /* 本驱动不用 */
};

/* ---- virtio-blk 协议层 -------------------------------------------------
 *
 * 一个块请求由**三段**缓冲区组成，这是 virtio-blk 规范规定的固定形状：
 *   第 1 段：请求头（下面这个结构），驱动写，设备读
 *   第 2 段：数据，读请求时设备写、驱动读（所以要标 VIRTQ_DESC_F_WRITE）
 *   第 3 段：状态字节，设备写，驱动读
 *
 * 为什么必须拆成三段而不是一个结构体：因为三段的方向不同。描述符的
 * WRITE 标志是按段设置的，头是驱动->设备，数据和状态是设备->驱动。
 * 合成一段就无法表达这个方向差异——这正是描述符链存在的理由。 */
#define VIRTIO_BLK_T_IN  0 /* 读（从设备读进内存）*/
#define VIRTIO_BLK_T_OUT 1 /* 写（本驱动不实现）*/

struct virtio_blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector; /* 以 512 字节扇区为单位，不是以文件系统块为单位 */
};

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/* ---- 驱动状态 ---------------------------------------------------------
 *
 * 这几块结构放在 .bss 里静态分配，不用 kalloc_page()，原因有三个：
 *   1. 设备要拿它们的物理地址。静态变量的物理地址是"虚拟地址 -
 *      KERNEL_VIRT_BASE"，一行减法就能算；kalloc 回来的页也一样，但静态
 *      分配额外保证了这些结构在整个内核生命周期里地址不变、不会被释放。
 *   2. blk_init() 在 kalloc 池建好之后才跑，用 kalloc 本来也可以，但
 *      "驱动的队列内存"属于驱动自己的固定家当，不是动态资源。
 *   3. 对齐要求可以用 __attribute__((aligned)) 直接表达。
 *
 * 对齐：virtio 1.0 规范第 2.4 节要求 desc 表 16 字节对齐、avail 环 2 字节、
 * used 环 4 字节。这里统统给到 16，宽于要求总是安全的。
 *
 * 一个容易忽略的点：这三块结构**不能跨页放到不连续的物理页上**——它们
 * 每一块内部必须物理连续，因为设备只拿到一个起始物理地址 + 隐含的大小。
 * 本 Lab 里每块都远小于 4KiB，静态分配天然满足；真实驱动里分配大队列时
 * 这是要专门处理的约束。 */
static struct virtq_desc  g_desc[VIRTQ_QUEUE_SIZE] __attribute__((aligned(16)));
static struct virtq_avail g_avail __attribute__((aligned(16)));
static struct virtq_used  g_used  __attribute__((aligned(16)));

/* 请求头和状态字节也要被设备访问，同样需要是静态的、地址稳定的内存。
 * 不能放在栈上：栈是 kalloc 来的页，地址虽然也能翻译，但这两个结构在
 * 设备处理请求的整个窗口里都必须有效，而本驱动是同步等待的，栈其实也
 * 安全——真正的理由是可读性：把"要交给设备的内存"和"普通局部变量"在
 * 声明位置上就区分开。 */
static struct virtio_blk_req_header g_req_header __attribute__((aligned(16)));
static uint8_t g_req_status __attribute__((aligned(16)));

/* 探测到的设备基址。0 表示还没初始化。 */
static uintptr_t g_virtio_base;

/* 上一次看到的 used.idx。设备每完成一个请求就把它自己的 used.idx 加一，
 * 驱动拿"现在的值 != 我记住的值"判断有新的完成事件。
 *
 * 为什么记住上次的值、而不是"等 used.idx == 1"：idx 是**只增**的累积
 * 计数器，不会回到 0，第二次请求完成时它是 2 而不是 1。写死成等某个
 * 具体数值的代码只有第一次能工作。 */
static uint16_t g_used_seen;

/* MMIO 寄存器访问。必须是 volatile：编译器不知道这些地址背后是设备，
 * 会以为"刚写进去的值再读出来还是它"、"连续两次读同一个地址结果一样"，
 * 于是把读写合并或删掉。volatile 禁止这类优化，保证每一次访问都真的
 * 发生、次数和顺序都跟源码一致。
 *
 * 必须用 32 位访问：virtio-mmio 的寄存器都是 32 位的，用 8 位或 64 位
 * 访问是未定义行为（QEMU 会报 guest error 或直接返回 0）。 */
static inline uint32_t mmio_read(uint32_t offset)
{
    return *(volatile uint32_t *)(g_virtio_base + offset);
}

static inline void mmio_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(g_virtio_base + offset) = value;
}

/* 扫描 8 个槽位，找第一个 DeviceID == 2（块设备）的。
 *
 * 为什么要扫描而不是硬编码 0x10001000：因为不指定 bus= 时 QEMU **从高往低**
 * 分配槽位，只挂一块盘时它落在 0x10008000。硬编码第一个槽位的教程在这种
 * 命令行下会读到一个 DeviceID=0 的空槽，然后报"没有磁盘"，而盘其实就在
 * 隔壁。run-qemu.sh 现在显式把盘钉在 bus.0，但驱动仍然扫描——环境约束和
 * 代码健壮性是两件事，不该互相替代。
 *
 * 扫描本身也是真实驱动的常态：设备在哪个槽位是运行时才知道的事实，
 * 正常系统靠设备树（riscv/ARM）或 PCI 枚举（x86）得到这个信息。本 Lab
 * 不实现设备树解析，用"遍历已知的地址窗口 + 看魔数"这个更朴素的办法，
 * 但"地址要发现、不要假设"这个态度是一样的。 */
static void virtio_find_device(void)
{
    for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        g_virtio_base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;

        if (mmio_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC) {
            continue; /* 这个地址上不是 virtio 传输通道 */
        }
        if (mmio_read(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_BLOCK) {
            continue; /* 是 virtio 通道，但槽位空着（0）或挂的是别的设备 */
        }

        uint32_t version = mmio_read(VIRTIO_MMIO_VERSION);
        if (version != VIRTIO_VERSION_MODERN) {
            /* 找到块设备了，但它只肯说 legacy 协议。单独报这个错、并且
             * 直接把该加的 QEMU 参数写出来——这是本课程反复用的一条原则：
             * 错误信息要说清"下一步做什么"，而不只是"哪里不对"。 */
            kprintf("virtio: 槽位 %u 上的块设备报告版本 %u，本驱动只实现版本 %u\n",
                    i, version, (uint32_t)VIRTIO_VERSION_MODERN);
            kprintf("  版本 1 是 legacy 接口（QueuePFN 那一套），寄存器布局不同。\n");
            kprintf("  QEMU 的 virtio-mmio 默认 force-legacy=true，需要加上：\n");
            kprintf("    -global virtio-mmio.force-legacy=false\n");
            panic("virtio: 块设备是 legacy 版本，缺少 force-legacy=false");
        }

        kprintf("virtio: block device at slot %u (0x%lx), version %u\n",
                i, (uint64_t)g_virtio_base, version);
        return;
    }

    g_virtio_base = 0;
    kprintf("virtio: 扫遍 %u 个 MMIO 槽位（0x%lx 起，每个 0x%lx）都没有块设备\n",
            (uint32_t)VIRTIO_MMIO_SLOTS, (uint64_t)VIRTIO_MMIO_BASE,
            (uint64_t)VIRTIO_MMIO_STRIDE);
    kprintf("  检查 QEMU 命令行是否挂了 -drive ...,if=none,id=d0 加"
            " -device virtio-blk-device,drive=d0\n");
    panic("virtio: 找不到 virtio 块设备");
}

void blk_init(void)
{
    virtio_find_device();

    /* ---- 规范第 3.1.1 节的设备初始化序列 ----
     *
     * 这一段的每一步都不能省、不能换顺序。设备内部是一个状态机，它靠
     * Status 寄存器的写入来推进；顺序错了设备会拒绝工作，而且通常不会
     * 报错，只是安静地什么都不做——这类"没有错误信息的失败"是写设备
     * 驱动最常见的挫败感来源，照着规范逐条对是唯一可靠的办法。 */

    /* 第 0 步：写 0 复位设备。即使刚上电也要做——不能假设设备处于初始
     * 状态（比如 bootloader 可能已经用过它了；本课程 riscv 用 OpenSBI
     * 做 bootloader，它不碰 virtio，但这个假设不该依赖）。 */
    mmio_write(VIRTIO_MMIO_STATUS, 0);

    /* 第 1、2 步：ACKNOWLEDGE 然后 DRIVER。注意是**累积**的：第二次写的是
     * 两位的并，不是只写 DRIVER。写成 `mmio_write(STATUS, DRIVER)` 会把
     * ACKNOWLEDGE 清掉，设备认为驱动退回了上一步。 */
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    mmio_write(VIRTIO_MMIO_STATUS, status);

    status |= VIRTIO_STATUS_DRIVER;
    mmio_write(VIRTIO_MMIO_STATUS, status);

    /* 第 3 步：特性协商。设备在 DEVICE_FEATURES 里列出它支持什么，驱动在
     * DRIVER_FEATURES 里回答"这些我也支持"。
     *
     * 本 Lab 的简化：一个特性都不启用，直接回答 0。这是合法的——所有可选
     * 特性都是可选的。真实驱动会在这里启用 VIRTIO_BLK_F_RO（只读盘检测）、
     * 间接描述符、事件抑制等等。
     *
     * 读一次 DEVICE_FEATURES 再丢掉，不是白读：保留这一行是为了让"协商"
     * 这个动作在代码里留下痕迹，也方便调试时在这里打印看设备到底支持什么。 */
    (void)mmio_read(VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, 0);

    /* 第 4、5 步：FEATURES_OK，然后**读回来确认**。
     *
     * 这是整个序列里唯一一个"写完要检查"的步骤，规范专门强调：如果设备
     * 不接受驱动选的特性组合，它会把 FEATURES_OK 位清掉。不读回确认就
     * 继续往下走，会在一个设备已经拒绝了的配置上操作。 */
    status |= VIRTIO_STATUS_FEATURES_OK;
    mmio_write(VIRTIO_MMIO_STATUS, status);

    if (!(mmio_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        panic("virtio: 设备拒绝了特性协商结果（FEATURES_OK 被清掉）");
    }

    /* 第 6 步：配置队列 0。virtio-blk 只有一个队列（队列 0），网卡那类
     * 设备会有多个（收/发各一个甚至多对）。 */
    mmio_write(VIRTIO_MMIO_QUEUE_SEL, 0);

    if (mmio_read(VIRTIO_MMIO_QUEUE_READY) != 0) {
        panic("virtio: 队列 0 在配置之前就已经是 ready 状态（复位没生效？）");
    }

    uint32_t num_max = mmio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0) {
        panic("virtio: 队列 0 不存在（QueueNumMax 是 0）");
    }
    if (num_max < VIRTQ_QUEUE_SIZE) {
        kprintf("virtio: 设备队列上限 %u，小于本驱动要求的 %u\n",
                num_max, (uint32_t)VIRTQ_QUEUE_SIZE);
        panic("virtio: 设备队列容量不足");
    }
    mmio_write(VIRTIO_MMIO_QUEUE_NUM, VIRTQ_QUEUE_SIZE);

    /* 三块内存清零后，把**物理地址**分低/高 32 位告诉设备。
     *
     * 清零不是可选的卫生习惯：avail.idx / used.idx 的初始值必须是 0，
     * 驱动的 g_used_seen 也从 0 开始。.bss 本来就是零，这里显式清一遍是
     * 为了让"队列从零状态开始"这件事在代码里可见（也让将来支持重新初始化
     * 时行为正确）。 */
    memset(g_desc, 0, sizeof(g_desc));
    memset(&g_avail, 0, sizeof(g_avail));
    memset(&g_used, 0, sizeof(g_used));
    g_used_seen = 0;

    uintptr_t desc_phys  = virt_to_phys(g_desc);
    uintptr_t avail_phys = virt_to_phys(&g_avail);
    uintptr_t used_phys  = virt_to_phys(&g_used);

    mmio_write(VIRTIO_MMIO_QUEUE_DESC_LOW,    (uint32_t)desc_phys);
    mmio_write(VIRTIO_MMIO_QUEUE_DESC_HIGH,   (uint32_t)(desc_phys >> 32));
    mmio_write(VIRTIO_MMIO_QUEUE_DRIVER_LOW,  (uint32_t)avail_phys);
    mmio_write(VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail_phys >> 32));
    mmio_write(VIRTIO_MMIO_QUEUE_DEVICE_LOW,  (uint32_t)used_phys);
    mmio_write(VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used_phys >> 32));

    /* 队列配好了，告诉设备可以用。 */
    mmio_write(VIRTIO_MMIO_QUEUE_READY, 1);

    /* 第 7 步：DRIVER_OK。这一位之后设备才会真正处理请求。 */
    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write(VIRTIO_MMIO_STATUS, status);

    kprintf("virtio: queue 0 ready (%u descriptors), polled read-only\n",
            (uint32_t)VIRTQ_QUEUE_SIZE);
}

/* 轮询上限。和 ide.c 的 ATA_POLL_LIMIT 是同一条纪律：所有等硬件的循环
 * 都要有退出路径，否则"设备不响应"这件事的表现是内核静默卡死。 */
#define VIRTIO_POLL_LIMIT 1000000

void blk_read(uint32_t blkno, void *buf)
{
    /* 本 Lab 的文件系统块大小恰好等于 virtio-blk 的扇区单位（512），
     * 所以块号直接就是扇区号。真实文件系统用 4KiB 块时这里要乘 8。 */
    _Static_assert(BSIZE == 512, "本驱动假设文件系统块大小等于 512 字节扇区");

    if (g_virtio_base == 0) {
        panic("blk_read: virtio 设备还没初始化（blk_init() 没被调用？）");
    }
    if (blkno >= FS_NBLOCKS) {
        kprintf("blk_read: 块号 %u 超出镜像范围（共 %u 块）\n",
                blkno, (uint32_t)FS_NBLOCKS);
        panic("blk_read: 块号越界——上层算错了块号");
    }

    /* ---- 组装一条三段描述符链 ----
     *
     * 用 desc[0]、desc[1]、desc[2] 三项，串成 0 -> 1 -> 2。
     * 本驱动一次只有一个在途请求，所以每次都从 0 号开始重用，不需要
     * 维护"哪些描述符空闲"的分配器（真实驱动需要，那是一个空闲链表）。 */
    g_req_header.type = VIRTIO_BLK_T_IN;
    g_req_header.reserved = 0;
    g_req_header.sector = blkno;

    /* 状态字节先写一个不可能是设备结果的值。
     * 这样"设备根本没写这个字节"和"设备写了 OK(0)"就能区分开——否则
     * 初值是 0 的话，设备什么都没做也会被读成成功。这是检查硬件结果时
     * 的通用手法：哨兵值要落在合法结果集之外。 */
    g_req_status = 0xFF;

    g_desc[0].addr  = virt_to_phys(&g_req_header);
    g_desc[0].len   = sizeof(g_req_header);
    g_desc[0].flags = VIRTQ_DESC_F_NEXT; /* 驱动->设备，所以不设 WRITE */
    g_desc[0].next  = 1;

    /* 数据段。VIRTQ_DESC_F_WRITE 的含义是"**设备**往这段内存写"——读磁盘
     * 时正是如此。这一位的命名视角是设备侧，设反了的症状是缓冲区里什么
     * 都没出现（设备认为这段是它要读的输入），很容易误判成"盘上是空的"。 */
    g_desc[1].addr  = virt_to_phys(buf);
    g_desc[1].len   = BSIZE;
    g_desc[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    g_desc[1].next  = 2;

    g_desc[2].addr  = virt_to_phys(&g_req_status);
    g_desc[2].len   = 1;
    g_desc[2].flags = VIRTQ_DESC_F_WRITE; /* 链尾，不设 NEXT */
    g_desc[2].next  = 0;

    /* ---- 提交 ----
     *
     * 把链头的下标（0）放进 avail 环当前位置，然后递增 avail.idx。
     * 下标要对队列长度取模——环形数组就是这么回事。 */
    g_avail.ring[g_avail.idx % VIRTQ_QUEUE_SIZE] = 0;

    /* 内存屏障：保证上面对描述符和 avail.ring 的所有写入，在设备看到
     * avail.idx 的新值**之前**就已经可见。
     *
     * 为什么需要它：设备是另一个"处理器"，它通过内存看驱动的意图。
     * CPU 和编译器都可以重排写入顺序，只要单线程语义不变——但 avail.idx
     * 的递增是那个"现在归你了"的信号，它必须最后到达。如果 idx 先被设备
     * 看到、描述符内容后到，设备会去处理一条还没填好的请求，读到的是
     * 上一次的地址或者全零。
     *
     * 这类 bug 的可怕之处在于它是时序相关的：在 QEMU 这种顺序执行的
     * 模拟器上几乎永远不出现，在真实的乱序 CPU 上偶发。屏障不是为了让
     * 当前环境跑通，是为了让代码在正确的模型下是对的。
     *
     * riscv 的 `fence rw, rw` 表示"屏障之前的所有读写，都要排在屏障之后
     * 的所有读写之前"。x86 的强内存模型里对应的是 sfence/mfence。 */
    __asm__ volatile("fence rw, rw" ::: "memory");

    g_avail.idx++;

    __asm__ volatile("fence rw, rw" ::: "memory");

    /* 通知设备队列 0 有新请求。写这个寄存器是一次 MMIO 写，设备侧会
     * 立刻（在 QEMU 里是同步地）开始处理。 */
    mmio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* ---- 等完成 ----
     *
     * 轮询 used.idx。设备处理完一条链就把 used.idx 加一。 */
    uint32_t spins = 0;
    while (g_used.idx == g_used_seen) {
        if (++spins >= VIRTIO_POLL_LIMIT) {
            kprintf("virtio: 等块 %u 完成超时（轮询 %u 次），used.idx=%u seen=%u status=0x%x\n",
                    blkno, (uint32_t)VIRTIO_POLL_LIMIT,
                    (uint32_t)g_used.idx, (uint32_t)g_used_seen,
                    mmio_read(VIRTIO_MMIO_STATUS));
            panic("virtio: 等待设备完成请求超时");
        }
        /* 每次循环都要重新从内存读 g_used.idx——设备改的是内存，编译器
         * 看不到有谁会改它，会把这个读提到循环外面变成死循环。
         *
         * 这里用屏障而不是把 g_used 声明成 volatile：volatile 只能阻止
         * 编译器优化，不提供任何多处理器可见性保证；fence 两件事都做到。
         * 把"设备会改的内存"和"需要屏障"绑在一起理解，比记住 volatile
         * 的特例更可靠。 */
        __asm__ volatile("fence rw, rw" ::: "memory");
    }

    /* 屏障：确认 used.idx 变了之后，才去读设备写进缓冲区和状态字节的
     * 内容。没有这个屏障，CPU 可能已经把旧的缓冲区内容预读进寄存器/缓存。
     * 这是上面提交侧屏障的镜像——一进一出，两侧都要。 */
    __asm__ volatile("fence rw, rw" ::: "memory");

    /* 核对设备到底完成了哪条链。本驱动一次只有一个在途请求，所以答案
     * 必然是 0 号；检查它是为了在"设备行为和我的假设不符"时立刻发现，
     * 而不是默默用错数据。 */
    struct virtq_used_elem *done = &g_used.ring[g_used_seen % VIRTQ_QUEUE_SIZE];
    if (done->id != 0) {
        kprintf("virtio: 设备完成的描述符链头是 %u，本驱动只提交过 0\n",
                (uint32_t)done->id);
        panic("virtio: used 环里出现了预期之外的描述符下标");
    }

    g_used_seen++;

    if (g_req_status == 0xFF) {
        panic("virtio: 设备报告完成，但状态字节没被写过——请求根本没被处理");
    }
    if (g_req_status != VIRTIO_BLK_S_OK) {
        kprintf("virtio: 读块 %u 失败，设备状态字节 = %u（1=IOERR, 2=UNSUPP）\n",
                blkno, (uint32_t)g_req_status);
        panic("virtio: virtio-blk 读请求返回错误");
    }

    /* 数据已经由设备 DMA 进 buf 了——注意这里没有任何搬数据的代码，
     * 这正是和 ide.c 最大的区别：那边有一行 insw 把 512 字节逐字搬进来，
     * 这边 CPU 一个字节都没碰过。 */
}
