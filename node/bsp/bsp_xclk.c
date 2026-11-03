#include "bsp_xclk.h"

#include "stm32f4xx.h"
#include "board.h"
#include "board_config.h"

/* HSE_VALUE / HSI_VALUE 在 CMSIS 头里没有定义，与 board.c 一样自己兜底 */
#ifndef HSE_VALUE
#define HSE_VALUE 8000000U
#endif
#ifndef HSI_VALUE
#define HSI_VALUE 16000000U
#endif

/* =============================================================================
 *  MCO1 的两个字段（RM0090 8.3.3 RCC_CFGR）
 *
 *    MCO1[1:0]    bit 22:21   00=HSI  01=LSE  10=HSE  11=PLL
 *    MCO1PRE[2:0] bit 26:24   0xx=/1   100=/2  101=/3  110=/4  111=/5
 *
 *  为什么选 MCO1 而不选定时器输出：不占定时器、不用中断、不受调度影响，
 *  上电后输出就是一个跟 CPU 无关的稳定方波 —— 摄像头要的正是这种时钟。
 * ========================================================================== */

static int s_ready;   /* bsp_xclk_init() 成功过没有 */

/** MCO1PRE 字段 → 实际分频值 */
static uint32_t mco1_div(uint32_t cfgr)
{
    uint32_t pre = (cfgr & RCC_CFGR_MCO1PRE) >> RCC_CFGR_MCO1PRE_Pos;

    /* 0..3 都表示"不分频"，4..7 依次是 /2 /3 /4 /5 */
    return (pre < 4U) ? 1U : (pre - 2U);
}

/** 当前 MCO1 选中的源频率 */
static uint32_t mco1_src_hz(uint32_t cfgr)
{
    switch ((cfgr & RCC_CFGR_MCO1) >> RCC_CFGR_MCO1_Pos) {
    case 0U:
        return HSI_VALUE;
    case 1U:
        return 32768U;              /* LSE */
    case 2U:
        return HSE_VALUE;
    default:
        return board_sysclk_hz();   /* PLL 主输出 */
    }
}

int bsp_xclk_init(void)
{
    uint32_t cfgr;

    s_ready = 0;

    /* 源没就绪时把它选到 MCO1 上，PA8 就是一条死电平。
     * SystemInit() 在 HSE 起振失败时会退回 HSI，所以这里必须先问一句 ——
     * 否则接线后会以为是"模块坏了"。 */
    if (BOARD_XCLK_SOURCE == 2 && (RCC->CR & RCC_CR_HSERDY) == 0U) {
        return 0;
    }
    if (BOARD_XCLK_SOURCE == 3 && (RCC->CR & RCC_CR_PLLRDY) == 0U) {
        return 0;
    }

    board_gpio_enable_clock(BOARD_XCLK_PORT);

    /* 1) 先定源和分频。此刻 PA8 还是普通 IO，写 CFGR 不会往线上打毛刺。
     *    手册建议"复位后、使能 PLL 之前"改这两个字段，那是为了让正在输出的
     *    MCO1 不被打断；我们这里是第一次启用，PA8 还没接上，不存在这个问题。 */
    cfgr  = RCC->CFGR;
    cfgr &= ~(RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    cfgr |= ((uint32_t)(BOARD_XCLK_SOURCE) & 3U) << RCC_CFGR_MCO1_Pos;
#if (BOARD_XCLK_DIV >= 2U) && (BOARD_XCLK_DIV <= 5U)
    /* /2 → 100, /3 → 101, /4 → 110, /5 → 111，即编码 = 分频值 + 2 */
    cfgr |= (uint32_t)(BOARD_XCLK_DIV + 2U) << RCC_CFGR_MCO1PRE_Pos;
#endif
    RCC->CFGR = cfgr;

    /* 2) 再把引脚切到复用功能。MCO1 是 PA8 的 AF0，而 AFR 的复位值就是 0，
     *    所以 board_gpio_af() 写进去的 0 正好是我们要的 —— AFR 不用手改。 */
    board_gpio_af(BOARD_XCLK_PORT, BOARD_XCLK_PIN, BOARD_XCLK_AF);
    board_gpio_pull(BOARD_XCLK_PORT, BOARD_XCLK_PIN, 0U);   /* 外部没上下拉 */
    board_gpio_speed(BOARD_XCLK_PORT, BOARD_XCLK_PIN, 3U);  /* 最高速：8 MHz 边沿要干净 */

    s_ready = 1;
    return 1;
}

uint32_t bsp_xclk_hz(void)
{
    uint32_t cfgr;

    if (!s_ready) {
        return 0U;
    }

    cfgr = RCC->CFGR;
    return mco1_src_hz(cfgr) / mco1_div(cfgr);
}

const char *bsp_xclk_source_name(void)
{
    switch ((RCC->CFGR & RCC_CFGR_MCO1) >> RCC_CFGR_MCO1_Pos) {
    case 0U:
        return "HSI";
    case 1U:
        return "LSE";
    case 2U:
        return "HSE";
    default:
        return "PLL";
    }
}
