#include "stm32f4xx.h"
#include "board.h"
#include "bsp_dwt.h"

/* 每微秒多少个周期。168 MHz → 168。用 uint32_t 存，delay 时按 32 位乘，
 * 单次最长约 25 秒才回绕，分片处理即可。 */
static uint32_t s_cycles_per_us = 168U;

void dwt_init(void)
{
    uint32_t hclk = board_hclk_hz();

    s_cycles_per_us = (hclk != 0U) ? (hclk / 1000000U) : 1U;
    if (s_cycles_per_us == 0U) {
        s_cycles_per_us = 1U;
    }

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* 给 DWT 供电 */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;              /* 开始计数 */
}

uint32_t dwt_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t dwt_elapsed_cycles(uint32_t since)
{
    return DWT->CYCCNT - since;          /* 无符号减法，回绕自动正确 */
}

uint32_t dwt_elapsed_us(uint32_t since)
{
    return dwt_elapsed_cycles(since) / s_cycles_per_us;
}

void dwt_delay_us(uint32_t us)
{
    uint32_t start;
    uint32_t ticks;

    if (us == 0U) {
        return;
    }

    start = DWT->CYCCNT;
    ticks = us * s_cycles_per_us;

    while ((DWT->CYCCNT - start) < ticks) {
        /* 空转 */
    }
}

void dwt_delay_ms(uint32_t ms)
{
    /* 分片，避免 ms 很大时 ticks 溢出 32 位（4294 秒以上才需要，但便宜） */
    while (ms-- != 0U) {
        dwt_delay_us(1000U);
    }
}
