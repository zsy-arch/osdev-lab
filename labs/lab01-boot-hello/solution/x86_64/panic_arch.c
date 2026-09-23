/* x86_64 的 panic_halt()：关中断后死循环执行 hlt。
 *
 * 先 cli 再 hlt 而不是只 hlt：hlt 单独使用时，一旦来了个中断（哪怕是我们
 * 没打算处理的），CPU 会被唤醒继续往下执行——如果不关中断，"停机"这个
 * 意图就不可靠。Lab1 阶段还没有设置中断向量表，任何意外中断走到默认
 * 处理路径都是未定义行为，cli 把这条路直接堵死。
 */

void panic_halt(void)
{
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
