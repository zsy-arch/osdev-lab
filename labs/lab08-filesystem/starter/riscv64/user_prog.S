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
    /* TODO 1：在用户栈上开出 BUFSZ 字节的缓冲区，把它的地址存进 s0。
     *
     * 为什么要放栈上、为什么必须存在 callee-saved 寄存器里，见
     * ../x86_64/user_prog.S 的 TODO 1，推理完全一样。riscv64 这边的具体
     * 差别只有两点：
     *   - 进来时 sp 指向 USER_STACK_VADDR（栈页顶，proc.c 的
     *     map_user_prog() 映射好的可写页），往下挪 128 字节就是缓冲区；
     *   - callee-saved 寄存器在 riscv64 是 s0-s11，本文件用 s0 存缓冲区、
     *     s1 存 fd（x86_64 那边对应 rbp 和 r12）。
     *
     * 提示：
     * addi sp, sp, -BUFSZ
     * mv s0, sp
     */

    /* ---- 1. hello.txt：一次读完（文件不足一块） ---- */

    /* TODO 2：打印 msg_open_hello，open("hello.txt")，读一次，把读到的
     * 内容原样打到控制台，然后 close。三个调用任何一个返回负数就分别跳去
     * open_failed / read_failed / close_failed。
     *
     * 这是整个文件里最完整的一次 open-read-write-close 往返，后面四步都是
     * 它的变形。"为什么用负数当错误信号"、"为什么 fd 要存进 callee-saved
     * 寄存器"见 ../x86_64/user_prog.S 的 TODO 2。
     *
     * riscv64 这边有一个**同一个坑、换了个位置**的地方值得单独想清楚：
     * read 返回后 a0 是读到的字节数，而它正好是 write 的 len；但 a0 同时
     * 也是 write 的**第一个参数**（缓冲区地址）。所以必须先 `mv a1, a0`
     * 把字节数搬走，再 `mv a0, s0` 装缓冲区地址。顺序反了就把字节数覆盖
     * 掉了。x86_64 那边的冲突是 rax 同时要装返回值和调用号——不同的寄存器
     * 分工，同一个"返回值紧接着当下一个调用的参数"的陷阱。写汇编时每次
     * 复用刚拿到返回值的那个寄存器，都值得停一下先问"它下一句要当什么"。
     *
     * 另外注意本课程的 sys_write 只有两个参数（buf, len），没有 fd——它
     * 永远写控制台。把控制台也做成 fd 表里的一项是本 Lab 的挑战任务。
     *
     * 这一步用 blez（<=0 就算失败）而不是 bltz：hello.txt 确定有内容，
     * 读到 0 字节说明哪里错了。下一步的循环里就不能这么写，那边 0 是正常
     * 的 EOF——同一个返回值在不同上下文里含义不同，是 read() 的接口特点。
     *
     * 提示：
     * la a0, msg_open_hello
     * la a1, msg_open_hello_end
     * sub a1, a1, a0
     * li a7, 1              // SYS_WRITE
     * ecall
     *
     * la a0, name_hello
     * li a7, 6              // SYS_OPEN
     * ecall
     * bltz a0, open_failed
     * mv s1, a0             // s1 = fd
     *
     * mv a0, s1             // read(fd, buf, BUFSZ)：一次就够，98 < 128
     * mv a1, s0
     * li a2, BUFSZ
     * li a7, 7              // SYS_READ
     * ecall
     * blez a0, read_failed
     *
     * mv a1, a0             // 先搬字节数，再装缓冲区地址
     * mv a0, s0
     * li a7, 1              // SYS_WRITE
     * ecall
     *
     * mv a0, s1
     * li a7, 8              // SYS_CLOSE
     * ecall
     * bltz a0, close_failed
     */

    /* ---- 2. motd.txt：循环读，跨多个块 ---- */

    /* TODO 3：打印 msg_open_motd，open("motd.txt")，然后**循环** read +
     * write 直到 read 返回 0，close，最后打印 msg_motd_eof。
     *
     * 这是本 Lab 最重要的一步——它验证的不是"能读到字节"，而是**文件描述符
     * 携带的偏移在内核侧被正确推进**：motd.txt 有 1824 字节、缓冲区只有
     * 128 字节，必须循环 15 次才能读完。如果 trap.c 的 sys_read() 忘了
     * `f->off += got;`，症状是这个循环**永远读到同一段内容、永远不结束**。
     * "为什么短读不等于 EOF"这条规则的完整展开见 ../x86_64/user_prog.S
     * 的 TODO 3。
     *
     * riscv64 这边的分支写法：
     *   - bltz a0, read_failed —— 负数 = 真的出错；
     *   - beqz a0, motd_read_done —— 0 = EOF，正常跳出；
     *   - 落下来的 1..BUFSZ = 拿到一段，打印再 j 回去。
     * 三条分支对应 x86_64 的 jl / je / 落空，只是 riscv64 的比较-分支合并
     * 成了一条指令，不需要先 cmp。
     *
     * 提示：
     * la a0, msg_open_motd
     * la a1, msg_open_motd_end
     * sub a1, a1, a0
     * li a7, 1              // SYS_WRITE
     * ecall
     *
     * la a0, name_motd
     * li a7, 6              // SYS_OPEN
     * ecall
     * bltz a0, open_failed
     * mv s1, a0
     *
     * motd_read_loop:
     * mv a0, s1
     * mv a1, s0
     * li a2, BUFSZ
     * li a7, 7              // SYS_READ
     * ecall
     * bltz a0, read_failed
     * beqz a0, motd_read_done   // 返回 0 = EOF，正常结束条件
     *
     * mv a1, a0
     * mv a0, s0
     * li a7, 1              // SYS_WRITE
     * ecall
     * j motd_read_loop
     *
     * motd_read_done:
     * mv a0, s1
     * li a7, 8              // SYS_CLOSE
     * ecall
     * bltz a0, close_failed
     *
     * la a0, msg_motd_eof
     * la a1, msg_motd_eof_end
     * sub a1, a1, a0
     * li a7, 1              // SYS_WRITE
     * ecall
     */

    /* ---- 3. exact.txt：文件大小正好是一整块 ---- */
    /* 这一步验证 fs_read() 里那条"读多少由 inode.size 决定、不由块数
     * 决定"的边界（见 fs.c fs_read() 的注释）。文件本身的内容就是在讲
     * 这个边界，所以照样打印出来——读对了会看到一段完整的说明文字；读错
     * 了现象分两种：完全不检查块号的写法会多读一块、而 addrs[1] 是 0，
     * 于是把块 0（超级块）当文件内容打出来，屏幕上是一串乱码；本 Lab 的
     * 写法会在 addr == 0 处 panic 停机。两种都很吵，不会静默通过——这
     * 正是挑这个文件大小的目的。 */

    /* TODO 4：跟 TODO 3 一模一样的循环，只是文件换成 "exact.txt"、消息
     * 换成 msg_open_exact / msg_exact_eof、标签换成 exact_read_loop /
     * exact_read_done。
     *
     * 为什么要单独再来一遍（512 字节 = 正好一整块，是 fs_read() 那条
     * "读多少由 inode.size 决定、不由块数决定"边界的最小构造），见
     * ../x86_64/user_prog.S 的 TODO 4。
     *
     * 提示：把 TODO 3 的代码整段拷过来，改四个名字即可。
     */

    /* ---- 4. 打开不存在的文件：期望 -1 ---- */
    /* 这一步验证的是"用户程序的错误不应该搞停内核"。如果内核在
     * fs_lookup() 找不到文件时 panic 而不是返回 0，这里整个系统就挂了
     * ——正确的行为是用户程序拿到 -1、自己决定怎么办。 */

    /* TODO 5：open("nope.txt")，**期望失败**：a0 >= 0 就跳去
     * missing_file_unexpectedly_opened（用 bgez，注意判断方向跟前面四步
     * 是反的），负数才打印 msg_missing_ok。
     *
     * 为什么这一步的断言是反的——"用户程序打开一个不存在的文件是完全正常
     * 的事件，内核必须返回错误码而不是 panic"——见 ../x86_64/user_prog.S
     * 的 TODO 5，以及 trap.c sys_open() 那个 TODO 里关于"这件事是谁的错"
     * 的讨论。
     *
     * 提示：
     * la a0, name_nope
     * li a7, 6              // SYS_OPEN
     * ecall
     * bgez a0, missing_file_unexpectedly_opened
     *
     * la a0, msg_missing_ok
     * la a1, msg_missing_ok_end
     * sub a1, a1, a0
     * li a7, 1              // SYS_WRITE
     * ecall
     */

    /* ---- 5. 重复 close：期望 -1 ---- */
    /* s1 还是上一个已经关掉的 fd。再关一次应该失败——如果成功了，说明
     * sys_close() 没有真的清掉 used 标志，fd 表的状态是假的。 */

    /* TODO 6：拿 s1 里那个**已经关掉**的 fd 再 close 一次，同样用 bgez
     * 判成功即失败，负数才打印 msg_double_close_ok。
     *
     * 为什么值得专门测（第一次 close 之后"状态真的变了吗"只能靠再做一次
     * 同样的操作来验证）见 ../x86_64/user_prog.S 的 TODO 6。
     *
     * 提示：
     * mv a0, s1
     * li a7, 8              // SYS_CLOSE
     * ecall
     * bgez a0, double_close_unexpectedly_ok
     *
     * la a0, msg_double_close_ok
     * la a1, msg_double_close_ok_end
     * sub a1, a1, a0
     * li a7, 1              // SYS_WRITE
     * ecall
     */

    /* ---- 6. 全部通过 ---- */

    /* TODO 7：打印 msg_all_ok，然后 exit(0)，最后 j halt_loop 兜底。
     *
     * msg_all_ok 是 tests/expect-riscv64.txt 抓的那一行——它存在的意义是
     * 让"测试通过"真的等价于"全部步骤都跑完了"，理由见
     * ../x86_64/user_prog.S 的 TODO 7。兜底用 j，不要用 wfi：wfi 在用户态
     * 会触发非法指令异常（Lab6 的 TODO 2 讲过）。
     *
     * 提示：
     * la a0, msg_all_ok
     * la a1, msg_all_ok_end
     * sub a1, a1, a0
     * li a7, 1              // SYS_WRITE
     * ecall
     *
     * li a0, 0
     * li a7, 2              // SYS_EXIT
     * ecall
     * j halt_loop
     */

/* ---- 失败路径 ---------------------------------------------------------
 * 已经写好，不是 TODO：下面五条失败路径和文件末尾的两个数据段直接给出
 * ——上面每个 TODO 都按名字引用它们（跳转标签、msg_X/msg_X_end 成对的
 * 标签），写法跟 Lab6/Lab7 同一个套路，改动字符串只会让
 * tests/expect-riscv64.txt 对不上。
 *
 * 但有一点值得看一眼：每种失败打印一条各不相同的消息再 exit(1)，不合并
 * 成一条通用的 "something failed"。测试和人工排查都需要知道是哪一步失败
 * 的，而用户态没有 printf 能把错误码打出来，唯一能携带信息的就是消息
 * 本身——错误信息的"分辨率"完全取决于你预先准备了几条不同的消息。
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
