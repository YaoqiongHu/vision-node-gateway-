/**
 * @file    system_stm32f4xx.c
 * @brief   STM32F407 时钟树与 FPU 初始化（CMSIS + 裸寄存器，无 HAL）
 *
 * 由 startup 的 Reset_Handler 在 .data/.bss 就绪后、main 之前调用一次。
 *
 * 目标时钟（HSE 8 MHz 晶振 → PLL）：
 *
 *   HSE 8 MHz ──/M=8──> 1 MHz ──×N=336──> 336 MHz ──/P=2──> SYSCLK 168 MHz
 *                                              └──/Q=7──> 48 MHz（USB/SDIO）
 *   SYSCLK 168 MHz ──/AHB=1──> HCLK 168 MHz
 *   HCLK 168 MHz ──/APB1=4──> PCLK1  42 MHz  ← APB1 上限 42 MHz，别改成 /2
 *   HCLK 168 MHz ──/APB2=2──> PCLK2  84 MHz  ← APB2 上限 84 MHz
 *
 * 三条容易踩的约束：
 *   1. 提频前必须先把 Flash 等待周期提到 5 WS（168 MHz / 3.3 V，RM0090 表 10）。
 *      HSI 16 MHz 下用 5 WS 只是变慢，不会错；反过来会取到乱码指令。
 *   2. 切 SYSCLK 源之前必须等 PLLRDY，切换后必须等 SWS 变成 PLL，否则
 *      后面配外设时总线还跑在 HSI 上，算出来的波特率全错。
 *   3. APB1 超过 42 MHz / APB2 超过 84 MHz 属于超频，HAL 不拦、硬件也不报，
 *      现象是"能跑，但偶发死机"。
 *
 * 本文件只碰两样东西：FPU 使能位 和 RCC/FLASH 的时钟寄存器。
 * NVIC 优先级分组、SysTick、外设时钟一律留给 bsp/board.c——那是板级决策，
 * 尤其 SysTick 在 S6 之后归 FreeRTOS 管，这里绝不初始化。
 */

#include "stm32f4xx.h"

/* ========================================================================== */
/*                              时钟参数                                       */
/* ========================================================================== */
/* 注意：CMSIS 的 stm32f4xx.h / stm32f407xx.h 里**没有** HSE_VALUE / HSI_VALUE
 * 的定义——ST 把这俩放进了 HAL 的 stm32f4xx_hal_conf.h。本项目不用 HAL，
 * 所以在这里自己定义，Makefile 用 -DHSE_VALUE=... 覆盖。 */
#ifndef HSE_VALUE
#define HSE_VALUE 8000000U          /* 板上晶振 */
#endif
#ifndef HSI_VALUE
#define HSI_VALUE 16000000U         /* 内部 RC，HSE 起振失败时的退路 */
#endif

#define PLL_M       (HSE_VALUE / 1000000U)  /* VCO 输入强制 1 MHz，抖动最小 */
#define PLL_N       336U                    /* VCO = 1 MHz × 336 = 336 MHz  */
#define PLL_P       2U                       /* SYSCLK = 336 / 2 = 168 MHz   */
#define PLL_Q       7U                       /* 48 MHz 支路 = 336 / 7         */

#define SYSCLK_HZ   168000000U

/* HSE 起振等待上限：一个空转计数，够 8 MHz 晶振起振（典型 1~5 ms） */
#define HSE_STARTUP_TIMEOUT  0x00050000U
/* PLL 锁定等待上限 */
#define PLL_LOCK_TIMEOUT     0x00050000U

#if (HSE_VALUE % 1000000U) != 0U
#error "HSE_VALUE 必须是 1 MHz 的整数倍，否则 PLL_M 取整会让 VCO 输入偏离 1 MHz"
#endif

/* ========================================================================== */
/*                           CMSIS 规定的全局量                                 */
/* ========================================================================== */
/**
 * 当前内核时钟（Hz）。默认值是"SystemInit 成功后"的值；
 * SystemInit 失败会把它改成 HSI 频率，靠它算 SysTick/波特率的代码就不会用错数。
 */
uint32_t SystemCoreClock = SYSCLK_HZ;

/* SystemCoreClockUpdate() 用的分频查表：索引就是 RCC_CFGR 里的 4 位/3 位编码。
 * 不能加 static——system_stm32f4xx.h 已经把它们声明成 extern，外部库会引用。 */
const uint8_t AHBPrescTable[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
const uint8_t APBPrescTable[8]  = {0, 0, 0, 0, 1, 2, 3, 4};

/* ========================================================================== */
/*                                内部函数                                      */
/* ========================================================================== */
/**
 * @brief 等待某位置 1，超时返回 0
 * @note  用 volatile 计数而不是 DWT：此刻 SysTick 和 DWT 都还没初始化。
 */
static uint32_t wait_flag_set(volatile uint32_t *reg, uint32_t mask, uint32_t timeout)
{
    while ((*reg & mask) == 0U) {
        if (timeout-- == 0U) {
            return 0U;
        }
    }
    return 1U;
}

/**
 * @brief HSE 起振 → PLL 168 MHz → 切 SYSCLK
 * @return 1 成功；0 HSE 未起振（调用方退回 HSI）
 */
static uint32_t clock_config_168m(void)
{
    /* 1) HSE 开振 */
    RCC->CR |= RCC_CR_HSEON;
    if (wait_flag_set(&RCC->CR, RCC_CR_HSERDY, HSE_STARTUP_TIMEOUT) == 0U) {
        RCC->CR &= ~RCC_CR_HSEON;          /* 起振失败就关掉，别让晶振一直抖 */
        return 0U;
    }

    /* 2) Flash 等待周期：必须在提频之前（见文件头约束 1） */
    FLASH->ACR = FLASH_ACR_PRFTEN        /* 预取使能       */
               | FLASH_ACR_ICEN          /* 指令 cache     */
               | FLASH_ACR_DCEN          /* 数据 cache     */
               | FLASH_ACR_LATENCY_5WS;  /* 5 个等待周期   */

    /* 3) 总线分频：AHB/1、APB1/4、APB2/2（见文件头约束 3） */
    RCC->CFGR = RCC_CFGR_HPRE_DIV1
              | RCC_CFGR_PPRE1_DIV4
              | RCC_CFGR_PPRE2_DIV2;

    /* 4) PLL 参数 */
    RCC->PLLCFGR = PLL_M
                 | (PLL_N << RCC_PLLCFGR_PLLN_Pos)
                 | (((PLL_P >> 1U) - 1U) << RCC_PLLCFGR_PLLP_Pos)
                 | (PLL_Q << RCC_PLLCFGR_PLLQ_Pos)
                 | RCC_PLLCFGR_PLLSRC_HSE;

    /* 5) PLL 上电并等锁定 */
    RCC->CR |= RCC_CR_PLLON;
    if (wait_flag_set(&RCC->CR, RCC_CR_PLLRDY, PLL_LOCK_TIMEOUT) == 0U) {
        RCC->CR &= ~RCC_CR_PLLON;
        return 0U;
    }

    /* 6) 切 SYSCLK 到 PLL，并等切换真正完成（见文件头约束 2） */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    if (wait_flag_set(&RCC->CFGR, RCC_CFGR_SWS_PLL, PLL_LOCK_TIMEOUT) == 0U) {
        return 0U;
    }

    return 1U;
}

/* ========================================================================== */
/*                                导出函数                                      */
/* ========================================================================== */
/**
 * @brief 系统初始化：FPU + 时钟。由 startup 调用，早于 main。
 */
void SystemInit(void)
{
    /* ---- FPU：CP10/CP11 全权限，否则浮点指令进 UsageFault ---- */
#if (__FPU_PRESENT == 1U)
    SCB->CPACR |= (3UL << (10U * 2U)) | (3UL << (11U * 2U));
    __DSB();
    __ISB();
#endif

    /* ---- 时钟 ---- */
    if (clock_config_168m() != 0U) {
        SystemCoreClock = SYSCLK_HZ;
    } else {
        /* HSE 挂了就老老实实跑 HSI 16 MHz：能跑、能打印、能进调试器，
         * 比在 168 MHz 的假设下算出错误的波特率更难查要强得多。 */
        SystemCoreClock = HSI_VALUE;
    }
}

/**
 * @brief 按当前寄存器值重算 SystemCoreClock
 * @note  正常流程用不到（SystemInit 已设好）；运行时改过 PLL 或从 STOP 唤醒后
 *        调一次，避免一堆外设拿旧频率算分频。
 */
void SystemCoreClockUpdate(void)
{
    uint32_t pll_m, pll_n, pll_p, pll_q;
    uint32_t pll_input_hz, vco_hz, sysclk_hz;
    uint32_t sws, ahb_div, apb1_div, apb2_div;

    sws = (RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos;

    if (sws == 0U) {
        /* HSI */
        sysclk_hz = HSI_VALUE;
    } else if (sws == 1U) {
        /* HSE 直连 */
        sysclk_hz = HSE_VALUE;
    } else {
        /* PLL：先算 VCO，再按 P 分频 */
        pll_m = RCC->PLLCFGR & RCC_PLLCFGR_PLLM;
        pll_n = (RCC->PLLCFGR & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
        pll_p = (((RCC->PLLCFGR & RCC_PLLCFGR_PLLP) >> RCC_PLLCFGR_PLLP_Pos) + 1U) * 2U;
        pll_q = (RCC->PLLCFGR & RCC_PLLCFGR_PLLQ) >> RCC_PLLCFGR_PLLQ_Pos;
        (void)pll_q;    /* 48 MHz 支路不影响内核频率，仅在此说明已解析 */

        if (pll_m == 0U) {
            pll_m = 1U; /* 防御：寄存器被写坏时不至于除零 */
        }
        pll_input_hz = ((RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) != 0U) ? HSE_VALUE : HSI_VALUE;
        vco_hz       = (pll_input_hz / pll_m) * pll_n;
        sysclk_hz    = vco_hz / pll_p;
    }

    ahb_div  = AHBPrescTable[(RCC->CFGR & RCC_CFGR_HPRE) >> RCC_CFGR_HPRE_Pos];
    apb1_div = APBPrescTable[(RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos];
    apb2_div = APBPrescTable[(RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos];

    SystemCoreClock = sysclk_hz >> ahb_div;

    /* 供外设驱动换算 PCLK1/PCLK2 用；本项目没开定时器，先留空占位。 */
    (void)apb1_div;
    (void)apb2_div;
}
