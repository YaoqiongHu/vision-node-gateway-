#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"
#include "board.h"
#include "bsp_can.h"
#include "bsp_dwt.h"
#include "bsp_i2c_soft.h"
#include "bsp_uart.h"
#include "bsp_xclk.h"
#include "board_config.h"
#include "ov7670/ov7670.h"
#include "app_tasks.h"

/* 给调试器看的量：openocd 里 `mdw &g_uptime_ms` 就能确认主循环真的在跑。
 * volatile 保证不会被 -O2 优化掉。 */
volatile uint32_t g_led0_toggles;
volatile uint32_t g_selftest_tx;
volatile uint32_t g_selftest_rx;
volatile int32_t  g_selftest_result;   /* 1 = 通过 / 0 = 不匹配 / -1 = 没收到 */
volatile uint16_t g_sccb_id;           /* (PID << 8) | VER，正常 0x7673 */
volatile int32_t  g_can_loopback = -2; /* 0 = 回环自测通过；-1 = 失败；-2 = 还没跑 */
volatile uint32_t g_can_tx_ok;         /* 正常模式下成功发出的帧数 */
volatile uint32_t g_can_tx_fail;       /* 发不出去（对端没上线 / 没 ACK）的次数 */

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
        printf("[loopback] disconnected\r\n");
    } else if ((n == expect) && (memcmp(rx, kProbe, expect) == 0)) {
        g_selftest_result = 1;
        printf("[loopback] PASS：%lu  OK\r\n",
               (unsigned long)n);
    } else {
        g_selftest_result = 0;
        printf("[loopback] FAIL： %lu  %lu \r\n",
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
    /* PLLP 字段编码 00=÷2 01=÷4 10=÷6 11=÷8，分频值 = 2 x (字段 + 1)。
     * 写成"字段 + 1"会打出 P=1（实际是 ÷2）。 */
    uint32_t p = 2U * (((pllcfgr & RCC_PLLCFGR_PLLP) >> RCC_PLLCFGR_PLLP_Pos) + 1U);
    uint32_t q = (pllcfgr & RCC_PLLCFGR_PLLQ) >> RCC_PLLCFGR_PLLQ_Pos;

    printf("\r\n");
    printf("=== vision-node / S2-1  XCLK ===\r\n");
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

/* S2 第一步的自检输出：把"应该是什么"和"寄存器里实际是什么"放在同一行，
 * 现场没带 USB-TTL 时，用调试器读这几个寄存器也能判断。 */
static void print_xclk(int ok)
{
    if (ok) {
        printf("xclk      : PA8 <- MCO1 %s = %lu Hz\r\n",
               bsp_xclk_source_name(), (unsigned long)bsp_xclk_hz());
        printf("            GPIOA MODER=0x%08lx AFR[1]=0x%08lx CFGR=0x%08lx\r\n",
               (unsigned long)BOARD_XCLK_PORT->MODER,
               (unsigned long)BOARD_XCLK_PORT->AFR[1],
               (unsigned long)RCC->CFGR);
    } else {
        printf("xclk      : FAIL - HSE not ready, PA8 stays silent\r\n");
    }
}

/* S2 第二步：SCCB 自检。
 * 分两步看：先扫总线（有没有器件应答），再读 OV7670 的 PID/VER（读写时序对不对）。
 * 这两步分开，能把"线没接"和"时序写错"区分开。 */
static void sccb_selftest(void)
{
    uint8_t pid = 0U;
    uint8_t ver = 0U;
    int     n;

    bsp_i2c_init();

    n = bsp_i2c_scan();

    (void)ov7670_read_reg(OV7670_REG_PID, &pid);
    (void)ov7670_read_reg(OV7670_REG_VER, &ver);
    g_sccb_id = (uint16_t)(((uint16_t)pid << 8) | (uint16_t)ver);

    printf("[sccb] OV7670 PID=0x%02X VER=0x%02X  -> %s\r\n",
           (unsigned)pid, (unsigned)ver, ov7670_present() ? "OK" : "FAIL");

    if ((n > 0) && (pid == 0U) && (ver == 0U)) {
        printf("[sccb] bus has a device but OV7670 does not answer: SCL/SDA swapped?\r\n");
    }
    if (n == 0) {
        printf("[sccb] no ACK on the whole bus: check pull-ups (~4.7k to 3.3V) and wires\r\n");
    }
}

/* S2 第三步：CAN1 自检。
 * 先跑内部回环（控制器自己发自己收，不需要收发器、不需要对端），
 * 通了才能说明位定时和寄存器配置是对的；然后再切回正常模式等对端上线。 */
static void can_selftest(void)
{
    uint8_t tec = 0U;
    uint8_t rec = 0U;
    int     ok;

    if (bsp_can_init() != 0) {
        printf("[can] init FAILED: controller never left init mode\r\n");
        return;
    }

    ok = bsp_can_loopback_selftest();
    g_can_loopback = (int32_t)ok;
    bsp_can_error_counters(&tec, &rec);

    printf("[can] loopback %s  BTR=0x%08lX (%lu kbps)  TEC=%u REC=%u\r\n",
           (ok == 0) ? "OK" : "FAIL",
           (unsigned long)bsp_can_btr(),
           (unsigned long)(BOARD_CAN_BITRATE / 1000U),
           (unsigned)tec, (unsigned)rec);
    printf("            PD0/PD1 AF%lu  GPIOD MODER=0x%08lX AFR=0x%08lX\r\n",
           (unsigned long)BOARD_CAN_AF,
           (unsigned long)GPIOD->MODER,
           (unsigned long)GPIOD->AFR[0]);
}

int main(void)
{
    /* SystemInit() 已由启动文件调用：FPU 打开 + HSE/PLL → 168 MHz。 */
    board_init();
    dwt_init();
    bsp_uart_init(BOARD_UART_BAUD);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* S2 第一步：先把摄像头的时钟送出去。XCLK 没有，OV7670 连 SCCB 都不应答。 */
    int xclk_ok = bsp_xclk_init();

    print_banner();
    print_xclk(xclk_ok);
    sccb_selftest();
    can_selftest();
    uart_loopback_selftest();

    /* S1b：从这里交给 FreeRTOS。main() 之后不再返回。 */
    app_tasks_start();

    for (;;) {
        /* 不可达 */
    }
}
