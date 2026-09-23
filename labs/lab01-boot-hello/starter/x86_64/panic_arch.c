/* Lab1 starter (x86_64)：实现 panic.h 的架构专属停机函数。
 *
 * 契约（见 src/common/panic.c 的调用方式）：panic_halt() 被调用时，
 * 说明内核已经决定"不能再继续跑了"，这个函数唯一的职责是让 CPU
 * 安全地永久停下来，不能返回。
 *
 * 为什么要先 cli 再 hlt：hlt 单独使用时，CPU 会在下一次任何中断
 * 到来时被唤醒继续执行——这不是我们想要的"永久停机"。本 Lab 阶段
 * 还没有配置 IDT（中断描述符表，Lab5 才会做），如果这时候真的来了
 * 一次中断，CPU 会尝试按一个不存在的中断处理表去处理，导致更混乱的
 * 故障。cli（Clear Interrupt Flag）先关掉可屏蔽中断，让 hlt 循环
 * 真正表现为"停在这里不动"。
 */

void panic_halt(void)
{
    /* TODO：关中断，然后无限循环执行 hlt。
     *
     * 提示：
     *   __asm__ volatile ("cli");
     *   for (;;) {
     *       __asm__ volatile ("hlt");
     *   }
     */
    for (;;) {
    }
}
