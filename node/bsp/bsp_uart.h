#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>

/* =============================================================================
 *  USART1 调试串口（PA9 = TX / PA10 = RX，AF7）
 *
 *  当前是**轮询发送**，接收留到 S1b 之后按需开中断：
 *    - S1a 只要能 printf，别把中断优先级那套提前引进来；
 *    - 真收数据时再配 NVIC（优先级必须落在 5..7，见计划书）。
 *
 *  2 Mbps 是后面传图像用的速率，这里按 pclk2/baud 算 BRR，
 *  84 MHz / 2 Mbps = 42，正好整除，不用小数分频。
 * ========================================================================== */

void bsp_uart_init(uint32_t baud);

void bsp_uart_putc(char c);
void bsp_uart_puts(const char *s);
void bsp_uart_write(const char *buf, uint32_t len);

/** 非阻塞收一个字节；没有数据返回 -1。读 DR 会顺带清掉 ORE/FE/NE/PE。 */
int bsp_uart_getc_nonblocking(void);

/** 丢弃接收缓冲里已到的数据（自环测试前清场用） */
void bsp_uart_rx_flush(void);

/** 等最后一个字节真正移出移位寄存器（复位前/切波特率前必须调用） */
void bsp_uart_flush(void);

#endif /* BSP_UART_H */
