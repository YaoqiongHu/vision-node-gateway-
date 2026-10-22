#include "stm32f4xx.h"
#include "board.h"
#include "bsp_uart.h"
#include "board_config.h"

void bsp_uart_init(uint32_t baud)
{
    uint32_t pclk2;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;     /* 读回，等时钟稳定 */

    /* PA9 = USART1_TX，AF7，推挽、高速（给 2 Mbps 留余量）、无上下拉 */
    board_gpio_af(BOARD_UART_TX_PORT, BOARD_UART_TX_PIN, BOARD_UART_AF);
    board_gpio_speed(BOARD_UART_TX_PORT, BOARD_UART_TX_PIN, 2U);
    board_gpio_pull(BOARD_UART_TX_PORT, BOARD_UART_TX_PIN, 0U);

    /* PA10 = USART1_RX，AF7，上拉（空闲时线上是高的；悬空不会乱收） */
    board_gpio_af(BOARD_UART_RX_PORT, BOARD_UART_RX_PIN, BOARD_UART_AF);
    board_gpio_pull(BOARD_UART_RX_PORT, BOARD_UART_RX_PIN, 1U);

    pclk2 = board_apb2_hz();
    if (pclk2 == 0U) {
        pclk2 = 84000000U;
    }

    BOARD_UART->CR1 = 0U;       /* 先关掉，才能在 UE=0 时写 BRR */
    BOARD_UART->CR2 = 0U;       /* 1 个停止位 */
    BOARD_UART->CR3 = 0U;       /* 无流控 */
    /* OVER8=0 时 BRR = USARTDIV * 16 = fCK / baud */
    BOARD_UART->BRR = (pclk2 + (baud / 2U)) / baud;
    BOARD_UART->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void bsp_uart_putc(char c)
{
    while ((BOARD_UART->SR & USART_SR_TXE) == 0U) {
        /* 等发送数据寄存器空 */
    }
    BOARD_UART->DR = (uint32_t)(uint8_t)c;
}

void bsp_uart_write(const char *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; i++) {
        bsp_uart_putc(buf[i]);
    }
}

void bsp_uart_puts(const char *s)
{
    while (*s != '\0') {
        bsp_uart_putc(*s++);
    }
}

void bsp_uart_flush(void)
{
    while ((BOARD_UART->SR & USART_SR_TC) == 0U) {
        /* 等整个帧移出去 */
    }
}

int bsp_uart_getc_nonblocking(void)
{
    if ((BOARD_UART->SR & USART_SR_RXNE) == 0U) {
        return -1;
    }
    /* 先读 SR 再读 DR 是 F4 规定的清序列：DR 读操作会清掉 RXNE，
     * 同时把 ORE/FE/NE/PE 一起清掉，不会让错误标志卡住后续接收。 */
    return (int)(BOARD_UART->DR & 0xFFU);
}

void bsp_uart_rx_flush(void)
{
    while (bsp_uart_getc_nonblocking() >= 0) {
        /* 丢弃 */
    }
}

/* ---------------------------------------------------------------------------
 * newlib 的重定向：printf 最终落到这里。
 * 定义了这个强符号之后，链接时就不会再从 libnosys 里拉那个空实现。
 * ------------------------------------------------------------------------- */
int _write(int file, char *ptr, int len)
{
    (void)file;

    if (len <= 0) {
        return 0;
    }

    bsp_uart_write(ptr, (uint32_t)len);
    bsp_uart_flush();
    return len;
}
