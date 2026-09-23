/* Lab8 x86_64：用户程序——用 open/read/close 把磁盘上的文件读出来打印。
 *
 * Lab7 的这份文件演示的是 fork/exec/wait；本 Lab 换成文件操作，*不再*
 * fork。理由是要验证的东西不同了：本 Lab 要看的是"用户程序能不能通过
 * fd 读到磁盘上真实存在的文件内容"，而 fork 会让输出交错、还会引入
 * Lab7 那套 rbx 命运线标记的复杂度，对验证文件系统毫无帮助。fork 之后
 * fd 表怎么继承是本 Lab 的挑战任务，挑战任务会让你把 fork 加回来。
 *
 * 调用约定（沿用 Lab6/Lab7，内核侧见 trap_entry.S 和 trap.c）：
 *   rax = 系统调用号（syscall.h）
 *   rdi/rsi/rdx = 前三个参数（Lab8 起是三个：sys_read(fd, buf, len)）
 *   返回值在 rax
 *
 * 缓冲区放在*用户栈*上，不放在 .data：
 *   1. user_prog.ld 的 SECTIONS 里只列了 .text 和 .rodata，没有 .data
 *      ——Lab7 那个 exit_code_slot 能工作是靠链接器把未列出的段放在
 *      末尾、objcopy 恰好把它一起拷进了二进制，属于"碰巧成立"。本 Lab
 *      不依赖这个。
 *   2. 更重要的是这本来就是真实程序的做法：read() 的目标缓冲区绝大多数
 *      时候是栈上的局部数组。栈页由内核在 map_user_prog() 里映射好
 *      （USER_STACK_VADDR 往下一整页，可写），进来时 rsp 就在页顶，
 *      sub 一下就有空间。
 *
 * 流程：
 *   1. open("hello.txt") -> 一次 read 读完（98 字节，不足一块）-> 打印
 *   2. open("motd.txt")  -> 循环 read 直到返回 0（1824 字节，跨 4 块）
 *                           -> 每读一段就打印，验证偏移在内核侧被正确
 *                              推进、跨块边界拼接正确
 *   3. open("exact.txt") -> 循环 read（正好 512 字节，整块边界）
 *                           -> 验证第二次 read 返回 0 而不是多读一块
 *   4. open("nope.txt")  -> 期望返回 -1（文件不存在不该 panic 内核）
 *   5. close 一个已经关掉的 fd -> 期望返回 -1（验证 used 标志真的清了）
 *   6. exit(0)
 *
 * 每一步都把结果打成一行可以被 tests/expect-x86_64.txt 抓的字符串。
 * 注意本程序不做数字格式化（用户态没有 printf），所以"读到了多少字节"
 * 这类信息不打印具体数值，只打印"成功/失败"这种定性结论——需要看具体
 * 字节数的话，内核侧 fs_init/kernel_boot 的 kprintf 已经打了。
 */
.code64

.section .text
.global _user_start

/* 栈上缓冲区的大小。刻意比文件小很多（motd.txt 有 1824 字节），这样
 * 第 2 步必须真的循环多次才能读完——如果缓冲区比文件还大，一次 read
 * 就读完了，"偏移被正确推进"这件事根本不会被验证到。 */
.set BUFSZ, 128

_user_start:
    /* TODO 1：在栈上开出 BUFSZ 字节的缓冲区，并把它的地址存进 rbp。
     *
     * 为什么：后面每一次 read() 都要传一个"往哪儿写"的地址，而用户态没有
     * malloc、user_prog.ld 里也没有 .data 段（见模块注释），所以唯一的
     * 选择是栈。进来时 rsp 指向内核在 map_user_prog() 里映射好的那一页
     * 栈顶，往下 sub 出来的就是合法可写内存。
     *
     * 为什么存 rbp 而不是随便挑一个寄存器：这个地址要跨越后面十几次
     * syscall 一直有效，所以必须放在 callee-saved 寄存器里
     * （rbx/rbp/r12-r15）。rdi/rsi/rdx/rax 每次 syscall 都被当参数或
     * 返回值用掉，存进去下一次调用就没了。rbp 能跨 syscall 存活的具体
     * 论证跟 Lab7 user_prog.S 里关于 rbx 的那段完全同构：syscall_entry
     * 自己不碰它，而被调用的 C 函数受 System V 约定约束必须还原它。
     *
     * 提示：
     * sub $BUFSZ, %rsp
     * mov %rsp, %rbp
     */

    /* ---- 1. hello.txt：一次读完（文件不足一块） ---- */

    /* TODO 2：打印 msg_open_hello，open("hello.txt")，读一次，把读到的
     * 内容原样打到控制台，然后 close。open/read/close 任何一个返回负数就
     * 分别跳去 open_failed / read_failed / close_failed。
     *
     * 为什么：这是整个文件里最完整的一次 open-read-write-close 往返，
     * 后面四步都是它的变形。三件事值得在这里想清楚：
     *
     * 1. 为什么判负数：fd 是非负的小整数（trap.c 的 fd_lookup() 决定了
     *    这一点），所以"负数"是本课程约定的唯一错误信号——没有 errno，
     *    错在哪只能靠内核侧的 kprintf 看。
     *
     * 2. fd 要存进 r12：跟 TODO 1 的 rbp 同一个理由，fd 要活过后面的
     *    read 和 close。选 r12 是因为 rbp 已经被缓冲区占了。
     *
     * 3. **read 返回值紧接着要当 write 的参数，顺序不能反。** read 回来
     *    时 rax 是读到的字节数，而它正好是 write 的 len；但 rax 同时也要
     *    装 write 的调用号。所以必须先 mov %rax, %rsi 把字节数搬走，再
     *    mov $1, %rax。写反了就是用调用号当长度打印，症状是屏幕上出现
     *    一个字符或者一堆越界垃圾——这是"返回值即下一个调用的参数"这种
     *    写法里最容易犯的错，写汇编时每次复用 rax 都值得停一下。
     *
     * 这一步用 jle（<=0 就算失败）而不是 jl：hello.txt 确定有内容，
     * 读到 0 字节说明哪里错了。下一步的循环里就不能这么写了，那边 0 是
     * 正常的 EOF——同一个返回值在不同上下文里含义不同，是 read() 这个
     * 接口的特点。
     *
     * 提示：
     * lea msg_open_hello(%rip), %rdi
     * mov $msg_open_hello_len, %rsi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     *
     * lea name_hello(%rip), %rdi
     * mov $6, %rax          // SYS_OPEN
     * syscall
     * cmp $0, %rax
     * jl open_failed
     * mov %rax, %r12        // r12 = fd
     *
     * mov %r12, %rdi        // read(fd, buf, BUFSZ)：一次就够，98 < 128
     * mov %rbp, %rsi
     * mov $BUFSZ, %rdx
     * mov $7, %rax          // SYS_READ
     * syscall
     * cmp $0, %rax
     * jle read_failed
     *
     * mov %rax, %rsi        // 先搬字节数，再设调用号
     * mov %rbp, %rdi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     *
     * mov %r12, %rdi
     * mov $8, %rax          // SYS_CLOSE
     * syscall
     * cmp $0, %rax
     * jl close_failed
     */

    /* ---- 2. motd.txt：循环读，跨多个块 ---- */

    /* TODO 3：打印 msg_open_motd，open("motd.txt")，然后**循环** read +
     * write 直到 read 返回 0，close，最后打印 msg_motd_eof。
     *
     * 为什么：这是本 Lab 最重要的一步——它验证的不是"能读到字节"，而是
     * **文件描述符携带的偏移在内核侧被正确推进**。motd.txt 有 1824 字节、
     * 缓冲区只有 128 字节（BUFSZ 刻意选小，见上面 .set 的注释），所以必须
     * 循环 15 次才能读完。如果 trap.c 的 sys_read() 忘了 `f->off += got;`
     * 那一行，症状是这个循环**永远读到同一段内容、永远不结束**——串口
     * 会无限重复 motd.txt 的第一段。这个现象很难误判，这也是为什么要挑
     * 一个远大于缓冲区的文件。
     *
     * 循环的结束条件是 read 返回 0，而不是"读满了"：
     *   - 返回 0 = EOF，正常结束，用 je 跳出去；
     *   - 返回负数 = 真的出错，跳 read_failed；
     *   - 返回 1..BUFSZ = 拿到一段，打印出来再回去读下一段。
     * 注意"返回值比 BUFSZ 小"**不能**当成 EOF：最后一次读到的就是不满
     * 一个缓冲区的残余（1824 % 128 = 32），要是把它当结束条件，正好读完
     * 整数个缓冲区的文件就会漏掉最后那次返回 0 的确认——下一步的
     * exact.txt 专门就是这个情况。这条规则在真实的 read(2) 上同样成立：
     * "短读不等于 EOF"是 UNIX I/O 最经典的踩坑点之一。
     *
     * 提示：
     * lea msg_open_motd(%rip), %rdi
     * mov $msg_open_motd_len, %rsi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     *
     * lea name_motd(%rip), %rdi
     * mov $6, %rax          // SYS_OPEN
     * syscall
     * cmp $0, %rax
     * jl open_failed
     * mov %rax, %r12
     *
     * motd_read_loop:
     * mov %r12, %rdi
     * mov %rbp, %rsi
     * mov $BUFSZ, %rdx
     * mov $7, %rax          // SYS_READ
     * syscall
     * cmp $0, %rax
     * jl read_failed
     * je motd_read_done     // 返回 0 = EOF，正常结束条件
     *
     * mov %rax, %rsi
     * mov %rbp, %rdi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     * jmp motd_read_loop
     *
     * motd_read_done:
     * mov %r12, %rdi
     * mov $8, %rax          // SYS_CLOSE
     * syscall
     * cmp $0, %rax
     * jl close_failed
     *
     * lea msg_motd_eof(%rip), %rdi
     * mov $msg_motd_eof_len, %rsi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     */

    /* ---- 3. exact.txt：文件大小正好是一整块 ---- */
    /* 这一步验证 fs_read() 里那条"读多少由 inode.size 决定、不由块数
     * 决定"的边界（见 fs.c fs_read() 的注释）。文件本身的内容就是在讲
     * 这个边界，所以照样打印出来——读对了，你会看到一段完整的说明文字；
     * 读错了，现象分两种，取决于 fs_read 怎么写：
     *   - 完全不检查块号的写法：多读一块，而 addrs[1] 是 0，于是把块 0
     *     （超级块）当文件内容打出来，屏幕上是一串二进制乱码；
     *   - 本 Lab 的写法：fs_read 在 addr == 0 处 panic，直接停机报错。
     * 两种都很吵，不会静默通过——这正是挑这个文件大小的目的。 */

    /* TODO 4：跟 TODO 3 一模一样的循环，只是文件换成 "exact.txt"、
     * 消息换成 msg_open_exact / msg_exact_eof、标签换成 exact_read_loop /
     * exact_read_done。
     *
     * 为什么单独来一遍：文件大小正好是 512 字节 = 一整块，这是
     * fs_read() 里"读多少由 inode.size 决定、不由块数决定"那条边界的
     * 唯一触发条件（上面那段注释讲了读错时的两种现象）。代码跟上一步
     * 同构，所以这一步练的不是写法，而是**为什么测试用例要专门挑边界
     * 大小的文件**：512 不是随手写的数字，它是"最后一次 read 正好把
     * 最后一块读完、下一次必须返回 0"这个场景的最小构造。写测试时主动
     * 去构造这种"刚好卡在边界上"的输入，比多跑十个普通用例更有用。
     *
     * 提示：把 TODO 3 的代码整段拷过来，改四个名字即可。
     */

    /* ---- 4. 打开不存在的文件：期望 -1 ---- */
    /* 这一步验证的是"用户程序的错误不应该搞停内核"。如果内核在
     * fs_lookup() 找不到文件时 panic 而不是返回 0，这里整个系统就
     * 挂了——而正确的行为是用户程序拿到 -1、自己决定怎么办。 */

    /* TODO 5：open("nope.txt")，**期望失败**：返回值 >= 0 就跳去
     * missing_file_unexpectedly_opened，返回负数才打印 msg_missing_ok。
     *
     * 为什么判断方向是反的：前面四步都是"失败就跳走"，这一步是"成功才
     * 跳走"。这不是绕，是本 Lab 的一个核心断言——**用户程序打开一个不
     * 存在的文件，是完全正常的事件，内核必须返回错误码而不是 panic。**
     * 如果 sys_open() 在 fs_lookup() 返回 0 时 panic，跑到这一行整个系统
     * 就停机了，测试连后面的输出都看不到。区分"内核自己坏了"和"用户请求
     * 不合法"是写系统调用时最该先想清楚的一件事，trap.c sys_open() 的
     * TODO 里还有一段专门讲它。
     *
     * 提示：
     * lea name_nope(%rip), %rdi
     * mov $6, %rax          // SYS_OPEN
     * syscall
     * cmp $0, %rax
     * jge missing_file_unexpectedly_opened
     *
     * lea msg_missing_ok(%rip), %rdi
     * mov $msg_missing_ok_len, %rsi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     */

    /* ---- 5. 重复 close：期望 -1 ---- */
    /* r12 还是上一个已经关掉的 fd。再关一次应该失败——如果成功了，
     * 说明 sys_close() 没有真的清掉 used 标志，fd 表的状态是假的。 */

    /* TODO 6：拿 r12 里那个**已经关掉**的 fd 再 close 一次，同样期望
     * 失败：>= 0 跳去 double_close_unexpectedly_ok，负数才打印
     * msg_double_close_ok。
     *
     * 为什么值得专门测一次：sys_close() 里"把整个槽位清掉"和"只把
     * used 置 0"在第一次 close 时表现完全一样，区别只在第二次 close
     * ——如果 used 标志没真的清掉，第二次 close 会返回成功，fd 表就在
     * 说谎。"一个操作做完之后，状态真的变了吗"只能靠再做一次同样的
     * 操作来验证，这是测试状态机时的通用手法。
     *
     * 提示：
     * mov %r12, %rdi
     * mov $8, %rax          // SYS_CLOSE
     * syscall
     * cmp $0, %rax
     * jge double_close_unexpectedly_ok
     *
     * lea msg_double_close_ok(%rip), %rdi
     * mov $msg_double_close_ok_len, %rsi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     */

    /* ---- 6. 全部通过 ---- */

    /* TODO 7：打印 msg_all_ok，然后 exit(0)，最后 jmp halt_loop 兜底。
     *
     * 为什么要有这一行总结消息：tests/expect-x86_64.txt 抓的就是它。
     * 前面每一步都有自己的输出，但"全部走完了"需要一个单独的标志——
     * 否则程序在第 4 步静默退出、前 3 步的输出照样在，测试会误判成通过。
     * 用一条只有跑到最后才会打印的消息当总开关，是让"测试通过"这件事
     * 真的等价于"代码是对的"的最便宜手段。
     *
     * exit 之后的 jmp halt_loop 正常永远不会执行到，理由跟 Lab6/Lab7
     * 对应位置一样：把"理论上不会发生"变成可控挂起，而不是让 CPU 冲进
     * 后面的 .rodata 当指令跑。
     *
     * 提示：
     * lea msg_all_ok(%rip), %rdi
     * mov $msg_all_ok_len, %rsi
     * mov $1, %rax          // SYS_WRITE
     * syscall
     *
     * mov $0, %rdi
     * mov $2, %rax          // SYS_EXIT
     * syscall
     * jmp halt_loop
     */

/* ---- 失败路径 ---------------------------------------------------------
 * 已经写好，不是 TODO：下面五条失败路径和文件末尾的两个数据段直接给出
 * ——上面每个 TODO 都按名字引用它们（跳转标签、消息标签、msg_X_len），
 * 写法跟 Lab6/Lab7 同一个套路，没有新知识，改动字符串只会让
 * tests/expect-x86_64.txt 对不上。
 *
 * 但有一点值得看一眼：每种失败打印一条各不相同的消息再 exit(1)，不合并
 * 成一条通用的 "something failed"。测试和人工排查都需要知道是哪一步失败
 * 的，而用户态没有 printf 能把错误码打出来，唯一能携带信息的就是消息
 * 本身——错误信息的"分辨率"完全取决于你预先准备了几条不同的消息。
 * exit(1) 而不是死循环：让父进程/内核能看到非零退出码。 */
open_failed:
    lea msg_open_failed(%rip), %rdi
    mov $msg_open_failed_len, %rsi
    mov $1, %rax
    syscall
    jmp fail_exit

read_failed:
    lea msg_read_failed(%rip), %rdi
    mov $msg_read_failed_len, %rsi
    mov $1, %rax
    syscall
    jmp fail_exit

close_failed:
    lea msg_close_failed(%rip), %rdi
    mov $msg_close_failed_len, %rsi
    mov $1, %rax
    syscall
    jmp fail_exit

missing_file_unexpectedly_opened:
    lea msg_missing_bad(%rip), %rdi
    mov $msg_missing_bad_len, %rsi
    mov $1, %rax
    syscall
    jmp fail_exit

double_close_unexpectedly_ok:
    lea msg_double_close_bad(%rip), %rdi
    mov $msg_double_close_bad_len, %rsi
    mov $1, %rax
    syscall
    jmp fail_exit

fail_exit:
    mov $1, %rdi
    mov $2, %rax /* SYS_EXIT */
    syscall

halt_loop:
    jmp halt_loop

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

msg_open_hello:
    .ascii "user: reading hello.txt\n"
msg_open_hello_len = . - msg_open_hello

msg_open_motd:
    .ascii "user: reading motd.txt in 128-byte chunks\n"
msg_open_motd_len = . - msg_open_motd

msg_motd_eof:
    .ascii "user: motd.txt reached EOF\n"
msg_motd_eof_len = . - msg_motd_eof

msg_open_exact:
    .ascii "user: reading exact.txt (exactly one block)\n"
msg_open_exact_len = . - msg_open_exact

msg_exact_eof:
    .ascii "user: exact.txt reached EOF without overrun\n"
msg_exact_eof_len = . - msg_exact_eof

msg_missing_ok:
    .ascii "user: open(nope.txt) correctly failed\n"
msg_missing_ok_len = . - msg_missing_ok

msg_double_close_ok:
    .ascii "user: double close correctly failed\n"
msg_double_close_ok_len = . - msg_double_close_ok

msg_all_ok:
    .ascii "user: all file tests passed\n"
msg_all_ok_len = . - msg_all_ok

msg_open_failed:
    .ascii "user: FAIL open returned an error\n"
msg_open_failed_len = . - msg_open_failed

msg_read_failed:
    .ascii "user: FAIL read returned an error\n"
msg_read_failed_len = . - msg_read_failed

msg_close_failed:
    .ascii "user: FAIL close returned an error\n"
msg_close_failed_len = . - msg_close_failed

msg_missing_bad:
    .ascii "user: FAIL open(nope.txt) unexpectedly succeeded\n"
msg_missing_bad_len = . - msg_missing_bad

msg_double_close_bad:
    .ascii "user: FAIL second close unexpectedly succeeded\n"
msg_double_close_bad_len = . - msg_double_close_bad
