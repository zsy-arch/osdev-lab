/* Lab9：ELF64 文件格式——内核的 exec() 要靠它把磁盘上的字节变成可执行的
 * 地址空间。
 *
 * 这个文件跟 fs_format.h 放在一起、在 Lab 根目录下只有一份，由两个架构的
 * 内核共同 include。理由跟 fs_format.h 完全一样，而且更强硬：ELF 不是本
 * 课程定义的格式，它是 System V ABI 规定的、由 GNU ld 在你的开发机上写出
 * 来的字节布局。我们没有任何余地去"按架构调整"它。
 *
 * 唯一容易让人误判的地方是 e_machine：x86_64 的 ELF 里是 62，riscv64 的
 * 是 243，看起来"跟架构有关"，于是很容易想给两个架构各放一份头文件。但
 * 这是个字段的*取值*不同，不是格式不同——struct Elf64_Ehdr 的每个字段在
 * 哪个偏移、占几个字节，两边一模一样。格式定义和格式数据是两件事，把数据
 * 差异当成格式差异会导致同一个结构体被复制两遍、然后慢慢长出真的分歧。
 * 所以：结构体和常量在这里共享一份，"本内核期望哪个 e_machine"写在
 * exec.c 里唯一那个 #if defined(__x86_64__) 块中。
 *
 * ELF 的全称是 Executable and Linkable Format，一种格式同时承担三个角色：
 *   - 可重定位文件（.o）：给链接器吃，靠 section header 描述
 *   - 可执行文件：给加载器（就是我们要写的 exec）吃，靠 program header 描述
 *   - 共享库（.so）：两者兼有，还要动态链接器参与
 * 本 Lab 只做第二种，而且只用 program header——section header 对加载器
 * 完全没用（内核加载一个可执行文件时不需要知道 .text 和 .rodata 的边界在
 * 哪，只需要知道"哪一段字节要映射到哪个虚拟地址、权限是什么"）。这个
 * "加载器只看 program header、链接器只看 section header"的分工是 ELF 设计
 * 里很关键的一点，见下面 struct Elf64_Phdr 的注释。
 */
#ifndef OSDEV_ELF_H
#define OSDEV_ELF_H

#include "types.h"

/* e_ident 的长度，以及其中几个我们要检查的下标。
 *
 * 这 16 个字节是 ELF 里唯一"位置和含义都跟字长/字节序无关"的部分——必须
 * 如此，因为解析器在读到 EI_CLASS 之前根本不知道该按 32 位还是 64 位去
 * 解释后面的字段。"文件头的开头几个字节自描述该怎么读后面"是所有跨平台
 * 二进制格式的共同手法（PNG、ZIP 的魔数也是这个作用）。 */
#define EI_NIDENT  16
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4  /* 32 位还是 64 位 */
#define EI_DATA    5  /* 小端还是大端 */
#define EI_VERSION 6  /* ELF 头本身的版本 */

/* 魔数 0x7F 'E' 'L' 'F'。第一个字节刻意取 0x7F（一个不可能出现在文本
 * 文件里的控制字符），后三个是可打印字符——这样 file(1) 之类的工具既能
 * 精确识别，人 hexdump 时也一眼能认出来。 */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64  2 /* EI_CLASS：64 位 */
#define ELFDATA2LSB 1 /* EI_DATA：小端 */
#define EV_CURRENT  1 /* 版本号，至今仍然是 1 */

/* e_type：这个 ELF 文件是哪一种。
 *
 * 本 Lab 只接受 ET_EXEC（位置固定的可执行文件）。ET_DYN 也是"可执行"的
 * 一种——现代发行版默认编译出的 PIE 可执行文件就是 ET_DYN，它没有固定的
 * 加载地址，要求加载器自己选一个基址、再把所有绝对地址按这个基址重定位。
 * 那需要处理 .rela.dyn 重定位表，是动态链接器的工作，本课程不做。
 *
 * 这个区分是一个真实的踩坑点：本 Lab 的用户程序用 -fno-pic 编译、用自己
 * 写的 user.ld 链接到固定地址 0x400000，所以是 ET_EXEC。如果哪天有人去掉
 * -fno-pic、或者用了发行版 gcc 的默认 PIE 设置，产物就变成 ET_DYN，本
 * Lab 的加载器会拒绝它（而不是加载出一个跑几条指令就崩的进程）。 */
#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3

/* e_machine：目标指令集。值由 ABI 注册表统一分配，不是随便取的。
 *
 * 只列出本课程用到的两个。完整列表有一百多项（EM_ARM=40、EM_AARCH64=183
 * ……），这里没必要全抄——但要知道这个字段的存在意味着什么：ELF 是一种
 * 跨架构的*容器*格式，同一套解析代码能读所有架构的 ELF，只有在真正要
 * 执行里面的指令时才需要架构匹配。本 Lab 的 exec.c 正是这个结构：解析
 * 全部共享，只在一处检查 e_machine。 */
#define EM_X86_64 62
#define EM_RISCV  243

/* p_type：program header 描述的是哪一种段。
 *
 * 加载器只关心 PT_LOAD——"把文件里 [p_offset, p_offset+p_filesz) 的字节
 * 放到虚拟地址 p_vaddr 处，一共占 p_memsz 字节"。其余类型都是给别人看的：
 * PT_DYNAMIC 给动态链接器，PT_INTERP 指明用哪个动态链接器，PT_NOTE 放
 * 构建 ID 之类的元信息，PT_PHDR 描述 program header 表自身（动态链接器
 * 靠它找到自己的 phdr）。
 *
 * 本 Lab 的加载器遇到非 PT_LOAD 的段直接跳过，不报错。这是正确的做法：
 * ELF 的设计意图就是"不认识的段类型应当忽略"，这样格式才能在不破坏老
 * 加载器的前提下增加新段类型。反过来，如果加载器对任何没见过的 p_type
 * 都报错，那链接器某天多写一个 PT_GNU_PROPERTY 段就会让所有程序跑不
 * 起来——而这件事真实发生过。 */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7

/* p_flags：段的访问权限。
 *
 * 注意这三个位的编号：X=1、W=2、R=4。习惯了 Unix 权限位 rwx=421 的人很
 * 容易把它当成 R=4/W=2/X=1 同序，实际上 ELF 这里是反的（读是最高位）。
 * 本 Lab 的 exec.c 把它们翻译成本课程页表的 PTE_FLAG_*，翻译错的后果是
 * 代码段不可执行（一进用户态就取指故障）或者数据段可执行（悄悄失去 W^X
 * 保护，不报错）——后者尤其值得警惕，因为它不会有任何症状。 */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* ELF64 文件头，固定 64 字节，位于文件最开头。
 *
 * 加载器真正需要的只有四项：e_ident（认格式）、e_type/e_machine（认能不
 * 能跑）、e_entry（从哪条指令开始执行）、以及 e_phoff/e_phentsize/e_phnum
 * （program header 表在哪、每项多大、共几项）。剩下的 e_sh* 全是 section
 * header 表的信息，加载器一律不看。
 *
 * 为什么 program header 表的"每项多大"（e_phentsize）要显式存在文件里，
 * 而不是让加载器直接用 sizeof(Elf64_Phdr)：这是格式向前兼容的标准手法
 * ——将来 phdr 如果加了字段变长了，老加载器读到 e_phentsize 比自己认识的
 * 大，仍然能用它当步长正确地跳到下一项（只是不理解新增的字段）。本 Lab
 * 的 exec.c 会检查 e_phentsize == sizeof(Elf64_Phdr) 再用，属于"我不打算
 * 兼容未来"的简化，但检查这一下比不检查好：不检查的话，步长不对会让后续
 * 每一项 phdr 都错位解析成垃圾。 */
struct Elf64_Ehdr {
    uint8_t  e_ident[EI_NIDENT]; /* 魔数 + class/data/version */
    uint16_t e_type;             /* ET_EXEC 等 */
    uint16_t e_machine;          /* EM_X86_64 / EM_RISCV */
    uint32_t e_version;          /* EV_CURRENT */
    uint64_t e_entry;            /* 程序入口的虚拟地址 */
    uint64_t e_phoff;            /* program header 表在文件里的偏移 */
    uint64_t e_shoff;            /* section header 表的偏移（加载器不用） */
    uint32_t e_flags;            /* 架构相关的标志位 */
    uint16_t e_ehsize;           /* 本结构体的大小，应为 64 */
    uint16_t e_phentsize;        /* 每个 program header 多大，应为 56 */
    uint16_t e_phnum;            /* program header 有几项 */
    uint16_t e_shentsize;        /* 以下三项都是 section header 的，不用 */
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/* ELF64 program header，固定 56 字节。一项描述一个"要加载的段"。
 *
 * 段（segment）和节（section）的区别是这里最值得说清楚的一件事：
 *
 *   - 节是*链接期*的概念：.text/.rodata/.data/.bss……粒度细，按用途划分,
 *     由 section header 描述，链接器靠它把多个 .o 里同名的节合并。
 *   - 段是*加载期*的概念：粒度粗，按*权限*划分，由 program header 描述。
 *     .text 和 .rodata 都是"可读可执行"或"只读"，通常被塞进同一个段;
 *     .data 和 .bss 都是可读可写，塞进另一个段。
 *
 * 为什么按权限合并：页表的权限粒度是页。把两个权限相同的节放进同一个段,
 * 加载器就能用一次映射覆盖它们；硬要按节分开映射，不但多做几次工作，还会
 * 因为节的大小几乎不是页的整数倍而导致同一个页被两个节共享、权限冲突。
 * 本 Lab 的 user.ld 里那几处 ALIGN(0x1000) 就是为了让 RX 段和 RW 段落在
 * 不同的页上——不加的话，链接器会让 .rodata 的尾部和 .data 的头部挤在同
 * 一个页里，那个页只能取两者权限的并集（可写*且*可执行），W^X 就破了。
 *
 * p_flags 的位置是本结构体唯一一个真正的陷阱：在 ELF32 里 p_flags 是
 * 倒数第二个字段，在 ELF64 里它被提前到了第二个。原因是 ELF64 把几个
 * 地址/长度字段从 32 位加宽到 64 位，把 32 位的 p_flags 提到前面可以省掉
 * 一处填充。照着 ELF32 的字段顺序写 ELF64 结构体是一个经典的移植 bug,
 * 症状是"p_flags 读出来是个巨大的数、p_align 读出来是权限位"——所以下面
 * 有一条静态断言钉住 sizeof，还有 exec.c 里对 e_phentsize 的检查兜底。
 *
 * p_filesz 和 p_memsz 可以不相等，而且这个差值是 .bss 得以存在的全部
 * 机制：.bss 里全是零，把成千上万个零存进可执行文件纯属浪费，于是 ELF
 * 的约定是"文件里只放 p_filesz 字节，加载器负责把 [p_filesz, p_memsz)
 * 这一段在内存里清零"。加载器漏掉这个清零，症状是未初始化的全局变量里
 * 是上一个使用者留下的垃圾——而本课程的 kalloc 恰好不保证返回零页，所以
 * 这个 bug 一定会暴露，见 exec.c 里的处理。 */
struct Elf64_Phdr {
    uint32_t p_type;   /* PT_LOAD 等 */
    uint32_t p_flags;  /* PF_R/PF_W/PF_X —— 注意在 ELF64 里它排第二 */
    uint64_t p_offset; /* 段内容在文件里的偏移 */
    uint64_t p_vaddr;  /* 段要被加载到的虚拟地址 */
    uint64_t p_paddr;  /* 物理地址，只对没有 MMU 的场景有意义，本 Lab 不用 */
    uint64_t p_filesz; /* 文件里有多少字节 */
    uint64_t p_memsz;  /* 内存里要占多少字节（>= p_filesz，差额清零） */
    uint64_t p_align;  /* 对齐要求，本 Lab 只用它做一致性检查 */
};

/* 这两个尺寸是 ABI 规定的，不是"我们这么排恰好得到的"。写成静态断言，
 * 结构体定义里任何一处笔误（字段顺序、类型宽度、漏字段）都会在编译期
 * 炸掉，而不是等到加载一个真实 ELF 时表现成"读出来的 e_entry 是垃圾"。
 *
 * 这里刻意不用 __attribute__((packed))：ELF64 的字段本来就是自然对齐的
 * （8 字节字段都在 8 的倍数偏移上），加 packed 不会改变布局，只会让每次
 * 字段访问都退化成逐字节读取，还会让"取字段地址"变成未定义行为。断言
 * 通过就证明了自然布局已经等于 ABI 布局——用断言确认，而不是用 packed
 * 强行掰弯，是处理这类"外部规定的结构体"的正确顺序。 */
_Static_assert(sizeof(struct Elf64_Ehdr) == 64,
               "Elf64_Ehdr 必须正好 64 字节（ABI 规定），检查字段顺序和宽度");
_Static_assert(sizeof(struct Elf64_Phdr) == 56,
               "Elf64_Phdr 必须正好 56 字节（ABI 规定），"
               "最常见的错因是照 ELF32 的顺序把 p_flags 写在了后面");

#endif /* OSDEV_ELF_H */
