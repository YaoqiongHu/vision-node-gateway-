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

/* ---------- 摄像头 XCLK ----------：
 * OV7670 模块没有晶振，必须由 MCU 送时钟（6.5–27 MHz，典型 8/12/24 MHz）。
 *   PA8 = MCO1 → J3 第 33 脚
 * MCO1 可选源：HSI 16M / LSE 32k / HSE 8M / PLL；分频 1~5。
 * 这里用 HSE 不分频 → 8 MHz，不占定时器、不用中断。
 * 注意 PC9 不能当 MCO2 用 —— 它被 DCMI_D3 占用。 */
#define BOARD_XCLK_PORT         GPIOA
#define BOARD_XCLK_PIN          8U
#define BOARD_XCLK_AF           0U          /* PA8 的 MCO1 是 AF0（AFR 复位值即 0） */

/* MCO1 源选择，编码与 RCC_CFGR 的 MCO1[1:0] 完全一致：
 *   0 = HSI 16 MHz    1 = LSE 32.768 kHz    2 = HSE 8 MHz    3 = PLL 168 MHz
 * 现在用 2（8 MHz）：边沿慢、逻辑分析仪好测，S2 只验"时钟送出来了"。
 * S4 要求 QQVGA ≥15 fps，8 MHz 下帧率只有约 10 fps，那时把这里改成 0
 * （HSI 16 MHz → 约 20 fps）。HSI 精度 ±1%，对摄像头够用。 */
#define BOARD_XCLK_SOURCE       2
#define BOARD_XCLK_DIV          1U          /* MCO1PRE：1 = 不分频，2~5 = 除以 2~5 */

/* ---------- SCCB / 软件 I²C ----------
 * OV7670 的配置通道叫 SCCB，电气上就是 I²C，只是第 9 位不要 ACK、不能用重复起始。
 *   SCL = PB10 → J2 第 54 脚
 *   SDA = PB11 → J2 第 53 脚
 * 两条线都靠上拉电阻拉高，模块上一般自带 4.7 kΩ；这里再开内部上拉（约 40 kΩ）兜底。
 * MPU6050 以后共用这条总线（地址 0x68）。 */
#define BOARD_SCCB_SCL_PORT     GPIOB
#define BOARD_SCCB_SCL_PIN      10U
#define BOARD_SCCB_SDA_PORT     GPIOB
#define BOARD_SCCB_SDA_PIN      11U
/* 半周期微秒数：5 µs → 约 100 kHz。先把速度压慢，波形看得清、分析仪好抓。 */
#define BOARD_SCCB_HALF_US      5U

/* ---------- CAN1（S2 第三条通路）----------
 * 元数据总线，500 kbps，对端是树莓派上的 MCP2515 + TJA1050。
 *   CAN1_RX = PD0 → J3 第 22 脚   ← TJA1050 的 RXD
 *   CAN1_TX = PD1 → J3 第 21 脚   → TJA1050 的 TXD
 * 为什么不用 PB8/PB9：那两个脚是 AF9 的默认映射，但被 DCMI_D6/D7 占了；
 * PD0/PD1 是 CAN1 的重映射脚，正好顶上（本项目第 1 号引脚冲突）。
 * TJA1050 的 S 脚必须接 GND（高速模式），电源 5 V。 */
#define BOARD_CAN               CAN1
#define BOARD_CAN_AF            9U
#define BOARD_CAN_RX_PORT       GPIOD
#define BOARD_CAN_RX_PIN        0U
#define BOARD_CAN_TX_PORT       GPIOD
#define BOARD_CAN_TX_PIN        1U
#define BOARD_CAN_BITRATE       500000U

/* 位定时：APB1 = 42 MHz。
 *   tq        = 6 / 42 MHz = 142.857 ns   （BRP = 5，即分频 6）
 *   一位      = 1(SYNC) + 11(BS1) + 2(BS2) = 14 tq = 2 µs  → 500 kbps
 *   采样点    = (1 + 11) / 14 = 85.7%      （CiA 推荐的 87.5% 附近）
 *   SJW       = 1 tq
 * 组成：BRP=5 | TS1=10<<16 | TS2=1<<20 = 0x001A0005 */
#define BOARD_CAN_BTR           0x001A0005U

/* ---------- 调试口 ----------
 * SWD：PA13 = SWDIO（J3-28）、PA14 = SWCLK（J3-27）。这两个脚不要挪作他用。 */

#endif /* BOARD_CONFIG_H */
