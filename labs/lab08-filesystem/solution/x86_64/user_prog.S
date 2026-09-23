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
    /* 在栈上开出缓冲区。rbp 拿来存缓冲区地址，全程不变——rbp 在
     * System V 调用约定里是 callee-saved，syscall_entry 也不碰它
     * （见 Lab7 user_prog.S 关于 rbx 的同类论证），所以它的值能跨越
     * 系统调用存活。 */
    sub $BUFSZ, %rsp
    mov %rsp, %rbp

    /* ---- 1. hello.txt：一次读完（文件不足一块） ---- */
    lea msg_open_hello(%rip), %rdi
    mov $msg_open_hello_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    lea name_hello(%rip), %rdi
    mov $6, %rax /* SYS_OPEN */
    syscall
    cmp $0, %rax
    jl open_failed
    mov %rax, %r12   /* r12 = fd，callee-saved，能跨 syscall 存活 */

    /* read(fd, buf, BUFSZ)：一次就够，98 < 128。 */
    mov %r12, %rdi
    mov %rbp, %rsi
    mov $BUFSZ, %rdx
    mov $7, %rax /* SYS_READ */
    syscall
    cmp $0, %rax
    jle read_failed

    /* 把读到的内容原样打到控制台：write(1, buf, n)。rax 此刻是读到的
     * 字节数，正好是 write 的 len 参数——但 rax 同时也要装调用号，
     * 所以先搬到 rsi 再设 rax。顺序反了会把字节数覆盖掉，是这种
     * "返回值紧接着当下一个调用的参数"的写法里最容易犯的错。 */
    mov %rax, %rsi
    mov %rbp, %rdi
    mov $1, %rax /* SYS_WRITE */
    syscall

    mov %r12, %rdi
    mov $8, %rax /* SYS_CLOSE */
    syscall
    cmp $0, %rax
    jl close_failed

    /* ---- 2. motd.txt：循环读，跨多个块 ---- */
    lea msg_open_motd(%rip), %rdi
    mov $msg_open_motd_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    lea name_motd(%rip), %rdi
    mov $6, %rax /* SYS_OPEN */
    syscall
    cmp $0, %rax
    jl open_failed
    mov %rax, %r12

motd_read_loop:
    mov %r12, %rdi
    mov %rbp, %rsi
    mov $BUFSZ, %rdx
    mov $7, %rax /* SYS_READ */
    syscall
    cmp $0, %rax
    jl read_failed
    je motd_read_done   /* 返回 0 = EOF，正常结束条件 */

    mov %rax, %rsi
    mov %rbp, %rdi
    mov $1, %rax /* SYS_WRITE */
    syscall
    jmp motd_read_loop

motd_read_done:
    mov %r12, %rdi
    mov $8, %rax /* SYS_CLOSE */
    syscall
    cmp $0, %rax
    jl close_failed

    lea msg_motd_eof(%rip), %rdi
    mov $msg_motd_eof_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    /* ---- 3. exact.txt：文件大小正好是一整块 ---- */
    /* 这一步验证 fs_read() 里那条"读多少由 inode.size 决定、不由块数
     * 决定"的边界（见 fs.c fs_read() 的注释）。文件本身的内容就是在讲
     * 这个边界，所以照样打印出来——读对了，你会看到一段完整的说明文字；
     * 读错了，现象分两种，取决于 fs_read 怎么写：
     *   - 完全不检查块号的写法：多读一块，而 addrs[1] 是 0，于是把块 0
     *     （超级块）当文件内容打出来，屏幕上是一串二进制乱码；
     *   - 本 Lab 的写法：fs_read 在 addr == 0 处 panic，直接停机报错。
     * 两种都很吵，不会静默通过——这正是挑这个文件大小的目的。 */
    lea msg_open_exact(%rip), %rdi
    mov $msg_open_exact_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    lea name_exact(%rip), %rdi
    mov $6, %rax /* SYS_OPEN */
    syscall
    cmp $0, %rax
    jl open_failed
    mov %rax, %r12

exact_read_loop:
    mov %r12, %rdi
    mov %rbp, %rsi
    mov $BUFSZ, %rdx
    mov $7, %rax /* SYS_READ */
    syscall
    cmp $0, %rax
    jl read_failed
    je exact_read_done

    mov %rax, %rsi
    mov %rbp, %rdi
    mov $1, %rax /* SYS_WRITE */
    syscall
    jmp exact_read_loop

exact_read_done:
    mov %r12, %rdi
    mov $8, %rax /* SYS_CLOSE */
    syscall
    cmp $0, %rax
    jl close_failed

    lea msg_exact_eof(%rip), %rdi
    mov $msg_exact_eof_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    /* ---- 4. 打开不存在的文件：期望 -1 ---- */
    /* 这一步验证的是"用户程序的错误不应该搞停内核"。如果内核在
     * fs_lookup() 找不到文件时 panic 而不是返回 0，这里整个系统就
     * 挂了——而正确的行为是用户程序拿到 -1、自己决定怎么办。 */
    lea name_nope(%rip), %rdi
    mov $6, %rax /* SYS_OPEN */
    syscall
    cmp $0, %rax
    jge missing_file_unexpectedly_opened

    lea msg_missing_ok(%rip), %rdi
    mov $msg_missing_ok_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    /* ---- 5. 重复 close：期望 -1 ---- */
    /* r12 还是上一个已经关掉的 fd。再关一次应该失败——如果成功了，
     * 说明 sys_close() 没有真的清掉 used 标志，fd 表的状态是假的。 */
    mov %r12, %rdi
    mov $8, %rax /* SYS_CLOSE */
    syscall
    cmp $0, %rax
    jge double_close_unexpectedly_ok

    lea msg_double_close_ok(%rip), %rdi
    mov $msg_double_close_ok_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    /* ---- 6. 全部通过 ---- */
    lea msg_all_ok(%rip), %rdi
    mov $msg_all_ok_len, %rsi
    mov $1, %rax /* SYS_WRITE */
    syscall

    mov $0, %rdi
    mov $2, %rax /* SYS_EXIT */
    syscall
    jmp halt_loop

/* ---- 失败路径 ---------------------------------------------------------
 * 每种失败打印一条各不相同的消息再 exit(1)。不合并成一条通用的
 * "something failed"：测试和人工排查都需要知道是哪一步失败的，而用户
 * 态没有 printf 能把错误码打出来，唯一能携带信息的就是消息本身。
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
