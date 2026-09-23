/* Lab6 riscv64：把 user_prog.S 独立编译/链接出的扁平二进制整段吸进
 * 内核镜像，跟 x86_64 侧（../x86_64/user_blob.S）同一个理由/同一个
 * 机制，见那份文件的完整注释，这里不重复。
 *
 * __user_prog_start/__user_prog_end 是 kernel_main.c 拷贝时需要的
 * 边界，跟 x86_64 侧同名符号是两份独立的符号（各自链接进各自架构的
 * 内核镜像），不是共享的。 */
.section .rodata

.global __user_prog_start
.global __user_prog_end

__user_prog_start:
.incbin "user_prog.bin"
__user_prog_end:
