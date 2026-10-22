#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* =============================================================================
 *  板级配置 —— 换板子只改这个文件
 *
 *  板子：DevEBox 大越电子 STM32F407Z（STM32F407ZGT6，LQFP144）
 *    Flash 1 MB / SRAM 112K+16K（DMA 可达）/ CCM 64K / HSE = 8 MHz（Y2）
 *  引脚位置来自原理图 J2/J3 排针，详见 docs/board.md
 * ========================================================================== */

/* ---------- LED ----------
 * 阳极接 3V3，经 510R 电阻到引脚 → 低电平点亮（灌电流）。
 *   LED0 = PF9  → J2 第 17 脚
 *   LED1 = PF10 → J2 第 20 脚
 * 核心板若没焊 LED，这两个脚仍引到排针，可当信号脚给逻辑分析仪。 */
#define BOARD_LED0_PIN          9U
#define BOARD_LED1_PIN          10U
#define BOARD_LED_PORT          GPIOF
#define BOARD_LED_ACTIVE_LOW    1

/* ---------- USART1 = 调试串口 ----------
 *   PA9  = TX → J3 第 32 脚（同时引到 J5 ISP 排针第 4 脚）
 *   PA10 = RX → J3 第 31 脚（同时引到 J5 ISP 排针第 3 脚）
 * 注意：J5 上还有 5V/GND，接 USB-TTL 时只接 GND/TX/RX 三根。 */
#define BOARD_UART              USART1
#define BOARD_UART_IRQn         USART1_IRQn
#define BOARD_UART_AF           7U
#define BOARD_UART_TX_PORT      GPIOA
#define BOARD_UART_TX_PIN       9U
#define BOARD_UART_RX_PORT      GPIOA
#define BOARD_UART_RX_PIN       10U
#define BOARD_UART_BAUD         115200U

/* ---------- 调试口 ----------
 * SWD：PA13 = SWDIO（J3-28）、PA14 = SWCLK（J3-27）。这两个脚不要挪作他用。 */

#endif /* BOARD_CONFIG_H */
