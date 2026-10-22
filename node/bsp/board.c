#include "board.h"
#include "board_config.h"

/* HSE_VALUE / HSI_VALUE 在 CMSIS 头里没有定义（ST 把这两个放在 HAL 的
 * stm32f4xx_hal_conf.h 里）。本项目不用 HAL，所以自己兜底，与
 * arch/system_stm32f4xx.c 里的默认值保持一致。 */
#ifndef HSE_VALUE
#define HSE_VALUE 8000000U
#endif
#ifndef HSI_VALUE
#define HSI_VALUE 16000000U
#endif

/* system_stm32f4xx.c 里的分频查表，索引 = RCC->CFGR 对应 3/4 位字段，值 = 右移位数 */
extern const uint8_t AHBPrescTable[16];
extern const uint8_t APBPrescTable[8];

/* -------------------------------------------------------------------------- */

static uint32_t led_pin(board_led_t led)
{
    return (led == BOARD_LED0) ? BOARD_LED0_PIN : BOARD_LED1_PIN;
}

void board_gpio_output(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = pin * 2U;

    port->MODER   = (port->MODER & ~(3UL << shift)) | (1UL << shift);  /* 01 = 通用输出 */
    port->OTYPER &= ~(1UL << pin);                                     /* 0 = 推挽 */
    port->OSPEEDR &= ~(3UL << shift);                                  /* 00 = 低速，够点亮 LED */
    port->PUPDR   &= ~(3UL << shift);                                  /* 00 = 无上下拉 */
}

void board_gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    uint32_t shift = pin * 2U;
    uint32_t idx   = pin >> 3U;            /* AFR[0] 管 0..7，AFR[1] 管 8..15 */
    uint32_t nib   = (pin & 7U) * 4U;

    port->MODER = (port->MODER & ~(3UL << shift)) | (2UL << shift);    /* 10 = 复用功能 */
    port->AFR[idx] = (port->AFR[idx] & ~(0xFUL << nib)) | ((af & 0xFUL) << nib);
}

void board_gpio_speed(GPIO_TypeDef *port, uint32_t pin, uint32_t speed)
{
    uint32_t shift = pin * 2U;

    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | ((speed & 3UL) << shift);
}

void board_gpio_pull(GPIO_TypeDef *port, uint32_t pin, uint32_t pull)
{
    uint32_t shift = pin * 2U;

    port->PUPDR = (port->PUPDR & ~(3UL << shift)) | ((pull & 3UL) << shift);
}

void board_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOFEN;
    (void)RCC->AHB1ENR;   /* 读回一次，确保时钟使能在后续写寄存器前生效 */

    board_gpio_output(BOARD_LED_PORT, BOARD_LED0_PIN);
    board_gpio_output(BOARD_LED_PORT, BOARD_LED1_PIN);

    board_led_set(BOARD_LED0, 0);
    board_led_set(BOARD_LED1, 0);
}

void board_led_set(board_led_t led, int on)
{
    uint32_t mask = 1UL << led_pin(led);

#if BOARD_LED_ACTIVE_LOW
    if (on) {
        BOARD_LED_PORT->ODR &= ~mask;
    } else {
        BOARD_LED_PORT->ODR |= mask;
    }
#else
    if (on) {
        BOARD_LED_PORT->ODR |= mask;
    } else {
        BOARD_LED_PORT->ODR &= ~mask;
    }
#endif
}

void board_led_toggle(board_led_t led)
{
    BOARD_LED_PORT->ODR ^= (1UL << led_pin(led));
}

/* ---------------------------------------------------------------- 时钟反推 */

uint32_t board_sysclk_hz(void)
{
    uint32_t sws = RCC->CFGR & RCC_CFGR_SWS;

    if (sws == RCC_CFGR_SWS_PLL) {
        uint32_t pllcfgr = RCC->PLLCFGR;
        uint32_t src = ((pllcfgr & RCC_PLLCFGR_PLLSRC) != 0U) ? HSE_VALUE : HSI_VALUE;
        uint32_t m = pllcfgr & RCC_PLLCFGR_PLLM;
        uint32_t n = (pllcfgr & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
        uint32_t p = (((pllcfgr & RCC_PLLCFGR_PLLP) >> RCC_PLLCFGR_PLLP_Pos) + 1U) * 2U;

        if (m == 0U) {
            return 0U;
        }
        return (src / m) * n / p;
    }
    if (sws == RCC_CFGR_SWS_HSE) {
        return HSE_VALUE;
    }
    if (sws == RCC_CFGR_SWS_HSI) {
        return HSI_VALUE;
    }
    return 0U;
}

uint32_t board_hclk_hz(void)
{
    uint32_t hpre = (RCC->CFGR & RCC_CFGR_HPRE) >> RCC_CFGR_HPRE_Pos;

    return board_sysclk_hz() >> AHBPrescTable[hpre];
}

uint32_t board_apb1_hz(void)
{
    uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;

    return board_hclk_hz() >> APBPrescTable[ppre1];
}

uint32_t board_apb2_hz(void)
{
    uint32_t ppre2 = (RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos;

    return board_hclk_hz() >> APBPrescTable[ppre2];
}

const char *board_name(void)
{
    return "DevEBox STM32F407Z / STM32F407ZGT6";
}
