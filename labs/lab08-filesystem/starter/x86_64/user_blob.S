/* Lab6 x86_64：把 user_prog.S 独立编译/链接出的扁平二进制（见
 * Makefile 里 objcopy -O binary 那条规则）整段吸进内核镜像——这是
 * 本 Lab 刻意选择的"不实现 ELF 加载器"的省力做法（ELF 解析是 Lab9
 * 的教学内容）：.incbin 是汇编器指令，效果是"把指定文件的原始字节
 * 原样嵌入当前 section，就像它们是手写的 .byte 序列"，链接期完成，
 * 不需要内核在运行时解析任何格式。
 *
 * 放进 .rodata（只读数据段）而不是 .text——虽然这段字节最终会被当
 * 代码执行，但那是在*用户页表*里通过独立的映射（kernel_main.c 会把
 * 这段内容拷贝到一个新分配的物理页，再用 PTE_FLAG_EXECUTABLE 映射
 * 到用户虚拟地址），它在*内核自己的地址空间*里始终只是一段需要被
 * 读取（拷贝）的原始数据，从不会被内核的 %rip 直接指向执行——这个
 * 区分（内核如何看待这段数据 vs 用户程序如何看待同一段数据）本身
 * 也是"地址空间"这个抽象的一个体现，值得在 README 里点出来。
 *
 * __user_prog_start/__user_prog_end 是 kernel_main.c 拷贝时需要的
 * 边界——.incbin 本身不会自动生成任何符号，必须手写两个标签夹住它。
 */
.section .rodata

.global __user_prog_start
.global __user_prog_end

__user_prog_start:
.incbin "user_prog.bin"
__user_prog_end:
