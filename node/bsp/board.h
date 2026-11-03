#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "stm32f4xx.h"

/* =============================================================================
 *  板级封装：只管"这颗料接在哪根脚上"，不掺业务。
 *  换板子 → 改 board_config.h 与本文件的时钟/引脚初始化。
 * ========================================================================== */

typedef enum {
    BOARD_LED0 = 0,
    BOARD_LED1 = 1
} board_led_t;

/** 开时钟、配 LED 引脚、LED 全灭。时钟树已由 SystemInit() 建好。 */
void board_init(void);

void board_led_set(board_led_t led, int on);
void board_led_toggle(board_led_t led);

/** GPIO 通用配置，供同层的 bsp_uart / bsp_dcmi 等复用 */
void board_gpio_enable_clock(GPIO_TypeDef *port);
void board_gpio_output(GPIO_TypeDef *port, uint32_t pin);
void board_gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af);
void board_gpio_speed(GPIO_TypeDef *port, uint32_t pin, uint32_t speed);
void board_gpio_pull(GPIO_TypeDef *port, uint32_t pin, uint32_t pull);

/** 从寄存器反推出来的实际时钟（与 SystemCoreClock 互为校验） */
uint32_t board_sysclk_hz(void);
uint32_t board_hclk_hz(void);
uint32_t board_apb1_hz(void);
uint32_t board_apb2_hz(void);

const char *board_name(void);

#endif /* BOARD_H */
