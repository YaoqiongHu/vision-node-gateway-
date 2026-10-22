#ifndef BSP_DWT_H
#define BSP_DWT_H

#include <stdint.h>

/* =============================================================================
 *  DWT 周期计数器 —— 裸机下唯一的时间基准。
 *
 *  为什么不用 SysTick：S6 之后 SysTick 要交给 FreeRTOS 当心跳（优先级最低），
 *  再拿它做 delay 会和调度器打架。DWT 是内核自带、与中断无关的 32 位计数器，
 *  而且顺手能把"这段代码跑了多少个周期"量出来（S12 实时性数据要用）。
 *
 *  在 168 MHz 下 CYCCNT 约 25.6 秒回绕一次，所有差值都用无符号减法，天然抗回绕。
 * ========================================================================== */

void dwt_init(void);

/** 当前周期计数（自 dwt_init 后从 0 开始累加） */
uint32_t dwt_cycles(void);

/** 自 since 起经过的周期数，自动处理回绕 */
uint32_t dwt_elapsed_cycles(uint32_t since);

/** 自 since 起经过的微秒数 */
uint32_t dwt_elapsed_us(uint32_t since);

void dwt_delay_us(uint32_t us);
void dwt_delay_ms(uint32_t ms);

#endif /* BSP_DWT_H */
