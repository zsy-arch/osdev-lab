/* Lab8 riscv64：用户程序——用 open/read/close 把磁盘上的文件读出来打印。
 *
 * 跟 x86_64 那份（../x86_64/user_prog.S）同一个流程、同一个教学取向，
 * 顶部注释里"为什么本 Lab 不再 fork"、"为什么缓冲区放在用户栈上而不是
 * .data"这两段推理见那边，这里不重复，只说 riscv64 这一侧的差异。
 *
 * 调用约定：
 *   a7 = 系统调用号（syscall.h）
 *   a0/a1/a2 = 前三个参数（Lab8 起是三个：sys_read(fd, buf, len)）
 *   返回值在 a0
 * 注意 sys_write 在本课程里只有两个参数（buf, len），没有 fd——它永远
 * 写控制台。把控制台也做成 fd 表里的一项是本 Lab 的挑战任务。
 *
 * 跨系统调用存活的寄存器用 s0/s1：
 *   s0 = 缓冲区地址（用户栈上）
 *   s1 = 当前的 fd
 * 为什么这两个能跨 ecall 存活，Lab7 这份文件顶部已经完整论证过（两层
 * 保证：trap_entry.S 的 128 字节轻量栈帧从头到尾不 sd/ld 任何 s0-s11；
 * riscv64 ABI 规定 s0-s11 是 callee-saved，内核侧那些普通 C 函数如果
 * 用到必须自己恢复）。本 Lab 直接沿用这个结论。
 *
 * 一处跟 x86_64 版本刻意一致的写法：消息长度用 `la` 两个标签再 sub
 * 算出来，而不是 `li a1, msg_len` 这种"汇编期常量"。这是 Lab6/Lab7
 * riscv64 侧就定下的做法——差值虽然在汇编期就能定，但写成两次 la 一次
 * sub 不依赖"标签差是否被延迟到链接期解析"这个细节，任何汇编器版本下
 * 都成立。x86_64 那边用 `msg_x_len = . - msg_x` 是因为它本来就要用
 * 立即数形式的 mov，两边各自用了自己最自然的写法。
 *
 * 流程（跟 x86_64 版本逐步对应，expect 文件里两边抓的是同样的行）：
 *   1. open("hello.txt") -> 一次 read 读完（98 字节，不足一块）-> 打印
 *   2. open("motd.txt")  -> 循环 read 直到返回 0（1824 字节，跨 4 块）
 *   3. open("exact.txt") -> 循环 read（正好 512 字节，整块边界）
 *                           -> 验证第二次 read 返回 0 而不是多读一块
 *   4. open("nope.txt")  -> 期望返回 -1（文件不存在不该 panic 内核）
 *   5. close 一个已经关掉的 fd -> 期望返回 -1（验证 used 标志真的清了）
 *   6. exit(0)
 */
.section .text
.global _user_start

/* 栈上缓冲区的大小。刻意比 motd.txt（1824 字节）小很多，这样第 2 步
 * 必须真的循环多次才能读完——缓冲区比文件还大的话，一次 read 就读完了，
 * "偏移被内核正确推进"这件事根本不会被验证到。
 *
 * 128 也在 addi 的立即数范围内（-2048..2047），所以下面可以直接
 * `addi sp, sp, -BUFSZ`，不需要先 li 到寄存器再 sub。 */
.set BUFSZ, 128

_user_start:
    /* 在用户栈上开出缓冲区。进来时 sp 指向 USER_STACK_VADDR（栈页顶，
     * proc.c 的 map_user_prog() 映射好的可写页），往下挪 128 字节就是
     * 缓冲区。 */
    addi sp, sp, -BUFSZ
    mv s0, sp

    /* ---- 1. hello.txt：一次读完（文件不足一块） ---- */
    lla a0, msg_open_hello
    lla a1, msg_open_hello_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    lla a0, name_hello
    li a7, 6 /* SYS_OPEN */
    ecall
    bltz a0, open_failed
    mv s1, a0   /* s1 = fd */

    /* read(fd, buf, BUFSZ)：一次就够，98 < 128。 */
    mv a0, s1
    mv a1, s0
    li a2, BUFSZ
    li a7, 7 /* SYS_READ */
    ecall
    blez a0, read_failed

    /* 把读到的内容原样打到控制台：write(buf, n)。a0 此刻是读到的字节数,
     * 正好是 write 的 len 参数——但 a0 同时也是 write 的第一个参数
     * （缓冲区地址），所以必须先把字节数搬到 a1 再设 a0。顺序反了会把
     * 字节数覆盖掉，是这种"返回值紧接着当下一个调用的参数"的写法里最
     * 容易犯的错（x86_64 那边是 rax 同时要装返回值和调用号，同一个坑
     * 换了个位置）。 */
    mv a1, a0
    mv a0, s0
    li a7, 1 /* SYS_WRITE */
    ecall

    mv a0, s1
    li a7, 8 /* SYS_CLOSE */
    ecall
    bltz a0, close_failed

    /* ---- 2. motd.txt：循环读，跨多个块 ---- */
    lla a0, msg_open_motd
    lla a1, msg_open_motd_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    lla a0, name_motd
    li a7, 6 /* SYS_OPEN */
    ecall
    bltz a0, open_failed
    mv s1, a0

motd_read_loop:
    mv a0, s1
    mv a1, s0
    li a2, BUFSZ
    li a7, 7 /* SYS_READ */
    ecall
    bltz a0, read_failed
    beqz a0, motd_read_done   /* 返回 0 = EOF，正常结束条件 */

    mv a1, a0
    mv a0, s0
    li a7, 1 /* SYS_WRITE */
    ecall
    j motd_read_loop

motd_read_done:
    mv a0, s1
    li a7, 8 /* SYS_CLOSE */
    ecall
    bltz a0, close_failed

    lla a0, msg_motd_eof
    lla a1, msg_motd_eof_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    /* ---- 3. exact.txt：文件大小正好是一整块 ---- */
    /* 这一步验证 fs_read() 里那条"读多少由 inode.size 决定、不由块数
     * 决定"的边界（见 fs.c fs_read() 的注释）。文件本身的内容就是在讲
     * 这个边界，所以照样打印出来——读对了会看到一段完整的说明文字；读错
     * 了现象分两种：完全不检查块号的写法会多读一块、而 addrs[1] 是 0，
     * 于是把块 0（超级块）当文件内容打出来，屏幕上是一串乱码；本 Lab 的
     * 写法会在 addr == 0 处 panic 停机。两种都很吵，不会静默通过——这
     * 正是挑这个文件大小的目的。 */
    lla a0, msg_open_exact
    lla a1, msg_open_exact_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    lla a0, name_exact
    li a7, 6 /* SYS_OPEN */
    ecall
    bltz a0, open_failed
    mv s1, a0

exact_read_loop:
    mv a0, s1
    mv a1, s0
    li a2, BUFSZ
    li a7, 7 /* SYS_READ */
    ecall
    bltz a0, read_failed
    beqz a0, exact_read_done

    mv a1, a0
    mv a0, s0
    li a7, 1 /* SYS_WRITE */
    ecall
    j exact_read_loop

exact_read_done:
    mv a0, s1
    li a7, 8 /* SYS_CLOSE */
    ecall
    bltz a0, close_failed

    lla a0, msg_exact_eof
    lla a1, msg_exact_eof_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    /* ---- 4. 打开不存在的文件：期望 -1 ---- */
    /* 这一步验证的是"用户程序的错误不应该搞停内核"。如果内核在
     * fs_lookup() 找不到文件时 panic 而不是返回 0，这里整个系统就挂了
     * ——正确的行为是用户程序拿到 -1、自己决定怎么办。 */
    lla a0, name_nope
    li a7, 6 /* SYS_OPEN */
    ecall
    bgez a0, missing_file_unexpectedly_opened

    lla a0, msg_missing_ok
    lla a1, msg_missing_ok_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    /* ---- 5. 重复 close：期望 -1 ---- */
    /* s1 还是上一个已经关掉的 fd。再关一次应该失败——如果成功了，说明
     * sys_close() 没有真的清掉 used 标志，fd 表的状态是假的。 */
    mv a0, s1
    li a7, 8 /* SYS_CLOSE */
    ecall
    bgez a0, double_close_unexpectedly_ok

    lla a0, msg_double_close_ok
    lla a1, msg_double_close_ok_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    /* ---- 6. 全部通过 ---- */
    lla a0, msg_all_ok
    lla a1, msg_all_ok_end
    sub a1, a1, a0
    li a7, 1 /* SYS_WRITE */
    ecall

    li a0, 0
    li a7, 2 /* SYS_EXIT */
    ecall
    j halt_loop

/* ---- 失败路径 ---------------------------------------------------------
 * 每种失败打印一条各不相同的消息再 exit(1)。不合并成一条通用的
 * "something failed"：测试和人工排查都需要知道是哪一步失败的，而用户态
 * 没有 printf 能把错误码打出来，唯一能携带信息的就是消息本身。
 * exit(1) 而不是死循环：让内核能看到非零退出码。 */
open_failed:
    lla a0, msg_open_failed
    lla a1, msg_open_failed_end
    sub a1, a1, a0
    li a7, 1
    ecall
    j fail_exit

read_failed:
    lla a0, msg_read_failed
    lla a1, msg_read_failed_end
    sub a1, a1, a0
    li a7, 1
    ecall
    j fail_exit

close_failed:
    lla a0, msg_close_failed
    lla a1, msg_close_failed_end
    sub a1, a1, a0
    li a7, 1
    ecall
    j fail_exit

missing_file_unexpectedly_opened:
    lla a0, msg_missing_bad
    lla a1, msg_missing_bad_end
    sub a1, a1, a0
    li a7, 1
    ecall
    j fail_exit

double_close_unexpectedly_ok:
    lla a0, msg_double_close_bad
    lla a1, msg_double_close_bad_end
    sub a1, a1, a0
    li a7, 1
    ecall
    j fail_exit

fail_exit:
    li a0, 1
    li a7, 2 /* SYS_EXIT */
    ecall

/* exit() 正常情况下不返回（sys_exit_proc() 把进程标成 ZOMBIE 然后
 * 换到调度器，永远不回到这里）。留一个死循环兜底：万一将来 exit 的
 * 实现变了、真的返回了，也不会掉进程序末尾之后的未定义内存去执行。 */
halt_loop:
    j halt_loop

.section .rodata
/* 文件名字符串。必须以 '\0' 结尾——内核侧 name_eq()（fs.c）按 C 字符串
 * 语义比较，靠这个 NUL 知道名字到哪里结束。.asciz 自动补 NUL，写成
 * .ascii 就会漏掉，症状是"文件明明在镜像里却查不到"（名字后面跟着的
 * 是下一个字符串的内容）。 */
name_hello:
    .asciz "hello.txt"
name_motd:
    .asciz "motd.txt"
name_exact:
    .asciz "exact.txt"
name_nope:
    .asciz "nope.txt"

/* 消息用 .ascii（不补 NUL）+ 一个结束标签：长度由两个标签相减得到，
 * sys_write 要的是显式长度，不是 NUL 结尾的字符串。 */
msg_open_hello:
    .ascii "user: reading hello.txt\n"
msg_open_hello_end:

msg_open_motd:
    .ascii "user: reading motd.txt in 128-byte chunks\n"
msg_open_motd_end:

msg_motd_eof:
    .ascii "user: motd.txt reached EOF\n"
msg_motd_eof_end:

msg_open_exact:
    .ascii "user: reading exact.txt (exactly one block)\n"
msg_open_exact_end:

msg_exact_eof:
    .ascii "user: exact.txt reached EOF without overrun\n"
msg_exact_eof_end:

msg_missing_ok:
    .ascii "user: open(nope.txt) correctly failed\n"
msg_missing_ok_end:

msg_double_close_ok:
    .ascii "user: double close correctly failed\n"
msg_double_close_ok_end:

msg_all_ok:
    .ascii "user: all file tests passed\n"
msg_all_ok_end:

msg_open_failed:
    .ascii "user: FAIL open returned an error\n"
msg_open_failed_end:

msg_read_failed:
    .ascii "user: FAIL read returned an error\n"
msg_read_failed_end:

msg_close_failed:
    .ascii "user: FAIL close returned an error\n"
msg_close_failed_end:

msg_missing_bad:
    .ascii "user: FAIL open(nope.txt) unexpectedly succeeded\n"
msg_missing_bad_end:

msg_double_close_bad:
    .ascii "user: FAIL second close unexpectedly succeeded\n"
msg_double_close_bad_end:
