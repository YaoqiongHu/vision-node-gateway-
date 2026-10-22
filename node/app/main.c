#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"
#include "board.h"
#include "bsp_dwt.h"
#include "bsp_uart.h"
#include "board_config.h"
#include "app_tasks.h"

/* 给调试器看的量：openocd 里 `mdw &g_uptime_ms` 就能确认主循环真的在跑。
 * volatile 保证不会被 -O2 优化掉。 */
volatile uint32_t g_led0_toggles;
volatile uint32_t g_selftest_tx;
volatile uint32_t g_selftest_rx;
volatile int32_t  g_selftest_result;   /* 1 = 通过 / 0 = 不匹配 / -1 = 没收到 */

/* ---------------------------------------------------------------------------
 * 串口自环自检：把 PA9(TX) 和 PA10(RX) 用一根杜邦线短接，
 * 发出去的字节能原样收回来，说明发送、接收、波特率、引脚复用全对。
 * 没接跳线时收到 0 字节，判 -1（跳过），不影响正常运行。
 * ------------------------------------------------------------------------- */
static const char kProbe[] = "vision-node loopback\r\n";

static void uart_loopback_selftest(void)
{
    char rx[sizeof(kProbe) - 1U];
    const uint32_t expect = sizeof(kProbe) - 1U;
    uint32_t n = 0U;
    uint32_t deadline;

    bsp_uart_rx_flush();

    bsp_uart_write(kProbe, expect);
    bsp_uart_flush();

    /* 50 ms 足够把 22 个字节按 115200 弹回来（约 1.9 ms） */
    deadline = dwt_cycles() + (board_hclk_hz() / 1000U) * 50U;
    while ((n < expect) && ((int32_t)(dwt_cycles() - deadline) < 0)) {
        int c = bsp_uart_getc_nonblocking();
        if (c >= 0) {
            rx[n++] = (char)c;
        }
    }

    g_selftest_tx = expect;
    g_selftest_rx = n;

    if (n == 0U) {
        g_selftest_result = -1;
        printf("[loopback] 没收到数据 —— PA9 和 PA10 之间没接跳线，跳过\r\n");
    } else if ((n == expect) && (memcmp(rx, kProbe, expect) == 0)) {
        g_selftest_result = 1;
        printf("[loopback] PASS：%lu 字节原样收回，收发链路 OK\r\n",
               (unsigned long)n);
    } else {
        g_selftest_result = 0;
        printf("[loopback] FAIL：期望 %lu 字节，收到 %lu 字节\r\n",
               (unsigned long)expect, (unsigned long)n);
    }
}

static const char *sws_name(uint32_t sws)
{
    switch (sws) {
    case RCC_CFGR_SWS_HSI:
        return "HSI";
    case RCC_CFGR_SWS_HSE:
        return "HSE";
    case RCC_CFGR_SWS_PLL:
        return "PLL";
    default:
        return "?";
    }
}

static void print_banner(void)
{
    uint32_t pllcfgr = RCC->PLLCFGR;
    uint32_t m = pllcfgr & RCC_PLLCFGR_PLLM;
    uint32_t n = (pllcfgr & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
    uint32_t p = ((pllcfgr & RCC_PLLCFGR_PLLP) >> RCC_PLLCFGR_PLLP_Pos) + 1U;
    uint32_t q = (pllcfgr & RCC_PLLCFGR_PLLQ) >> RCC_PLLCFGR_PLLQ_Pos;

    printf("\r\n");
    printf("=== vision-node / S1a ===\r\n");
    printf("build     : %s %s\r\n", __DATE__, __TIME__);
    printf("board     : %s\r\n", board_name());
    printf("SYSCLK    : %lu Hz  (SystemCoreClock = %lu)\r\n",
           (unsigned long)board_sysclk_hz(), (unsigned long)SystemCoreClock);
    printf("HCLK      : %lu Hz\r\n", (unsigned long)board_hclk_hz());
    printf("APB1/APB2 : %lu / %lu Hz\r\n",
           (unsigned long)board_apb1_hz(), (unsigned long)board_apb2_hz());
    printf("clock src : %s   HSERDY=%u PLLRDY=%u\r\n",
           sws_name(RCC->CFGR & RCC_CFGR_SWS),
           (unsigned)((RCC->CR & RCC_CR_HSERDY) != 0U),
           (unsigned)((RCC->CR & RCC_CR_PLLRDY) != 0U));
    printf("PLL       : src=%s M=%lu N=%lu P=%lu Q=%lu\r\n",
           ((pllcfgr & RCC_PLLCFGR_PLLSRC) != 0U) ? "HSE" : "HSI",
           (unsigned long)m, (unsigned long)n, (unsigned long)p, (unsigned long)q);
    printf("flash     : LATENCY=%lu wait state\r\n",
           (unsigned long)(FLASH->ACR & FLASH_ACR_LATENCY));
    printf("uart      : USART1 115200 8N1  BRR=0x%03lx  pclk2=%lu\r\n",
           (unsigned long)BOARD_UART->BRR, (unsigned long)board_apb2_hz());
    printf("led       : PF9 500ms / PF10 100ms\r\n");
    printf("rtos      : FreeRTOS V11.3.2  tick=1000 Hz  heap=24 KB (CCM)\r\n");
    printf("----------------------------------------\r\n");
}

int main(void)
{
    /* SystemInit() 已由启动文件调用：FPU 打开 + HSE/PLL → 168 MHz。 */
    board_init();
    dwt_init();
    bsp_uart_init(BOARD_UART_BAUD);
    setvbuf(stdout, NULL, _IONBF, 0);

    print_banner();
    uart_loopback_selftest();

    /* S1b：从这里交给 FreeRTOS。main() 之后不再返回。 */
    app_tasks_start();

    for (;;) {
        /* 不可达 */
    }
}
