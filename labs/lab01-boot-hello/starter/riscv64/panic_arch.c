/* Lab1 starter (riscv64)：实现 panic.h 的架构专属停机函数。
 *
 * 契约和 x86_64 版本一样：panic_halt() 被调用时说明内核已经决定
 * "不能再继续跑了"，唯一职责是让 CPU 安全地永久停下来，不能返回。
 *
 * riscv64 这边不需要像 x86_64 那样显式关中断——riscv 的中断默认
 * 就是关闭的（要通过 sstatus.SIE 显式打开，本 Lab 阶段从没打开过），
 * 所以单纯的 wfi 循环已经足够安全，CPU 会一直等待中断（但中断实际
 * 上关着，永远不会被唤醒），效果上等同于永久停机。
 */

void panic_halt(void)
{
    /* TODO：无限循环执行 wfi。
     *
     * 提示：
     *   for (;;) {
     *       __asm__ volatile ("wfi");
     *   }
     */
    for (;;) {
    }
}
