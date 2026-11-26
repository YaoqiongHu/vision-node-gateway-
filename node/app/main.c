#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"
#include "board.h"
#include "bsp_dcmi.h"
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
volatile int32_t  g_img_result = -2;   /* 0 = 抓帧成功；-1 = 超时；-2 = 溢出错误 */
volatile uint32_t g_img_ms;            /* 抓到一帧用掉多少毫秒 */
volatile uint32_t g_img_sum;           /* 整帧字节累加和，给调试器核对 */
/* 总线探针：[0]=PCLK [1]=HREF [2]=VSYNC [3..10]=D0..D7，
 * 存的是 50 ms 窗口内该位被采到高电平的次数（不是"是/否"）。 */
volatile uint32_t g_probe[11];
volatile uint32_t g_sweep[8][4];
volatile uint32_t g_frame_off;
volatile uint32_t g_vsync_cnt;
volatile uint8_t  g_raw[4096];
extern void bsp_cam_raw_snapshot(uint8_t *buf, uint32_t n);
extern uint32_t g_dcmi_cm;
extern uint32_t g_dcmi_pol;
volatile uint32_t g_pclk_line;
/* 纯 CPU 轮询 DCMI->DR 抽样：g_poll_words 个 32 位字，直接看一眼原始数据。 */
volatile uint32_t g_poll_buf[16];
volatile uint32_t g_poll_words;
volatile uint32_t g_bb_words;
volatile uint8_t  g_bb_buf[64];

/* 一帧 QQVGA RGB565：160 x 120 x 2 = 38,400 B。
 * 必须是普通 SRAM（.bss），DMA 不能访问 CCM。 */
/* 两帧缓冲：DMA 连续填 2 帧，再用 VSYNC 记录的偏移把其中完整的一帧挪到开头 */
static uint8_t g_frame[BOARD_IMG_BYTES * 2U] __attribute__((aligned(4)));

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

/* S3/S4：把摄像头的数据接收通路打通。
 *
 * 流程：SCCB 配置 OV7670 → 配 DCMI/DMA → 抓一帧 → 统计 → 通过串口发给电脑。
 * 这一步只看"能不能收到一帧像样的数据"，二值化和质心是 S5 的事。
 *
 * 传给电脑的格式（临时，S8 会换成项目正式帧协议）：
 *   'I' 'M' 'G' '!'  4 字节魔数
 *   width  (uint16 LE)
 *   height (uint16 LE)
 *   bytes  (uint32 LE)
 *   payload（width*height*2 字节，RGB565，高字节在前）
 */
static void send_image_header(uint32_t width, uint32_t height, uint32_t bytes)
{
    uint8_t hdr[12];

    hdr[0] = 'I';
    hdr[1] = 'M';
    hdr[2] = 'G';
    hdr[3] = '!';
    hdr[4] = (uint8_t)(width & 0xFFU);
    hdr[5] = (uint8_t)((width >> 8) & 0xFFU);
    hdr[6] = (uint8_t)(height & 0xFFU);
    hdr[7] = (uint8_t)((height >> 8) & 0xFFU);
    hdr[8]  = (uint8_t)(bytes & 0xFFU);
    hdr[9]  = (uint8_t)((bytes >> 8) & 0xFFU);
    hdr[10] = (uint8_t)((bytes >> 16) & 0xFFU);
    hdr[11] = (uint8_t)((bytes >> 24) & 0xFFU);

    bsp_uart_write((const char *)hdr, sizeof(hdr));
}

/* 单字节统计：全 0x00 说明没有 PCLK / 数据线没接，
 * 全 0xFF 说明数据线被拉高，乱跳说明时序或接线有问题。 */
static void print_frame_stats(void)
{
    uint32_t i;
    uint32_t sum = 0U;
    uint32_t zero = 0U;
    uint32_t full = 0U;
    uint8_t  lo = 0xFFU;
    uint8_t  hi = 0x00U;
    uint16_t px00;
    uint16_t pxc;

    for (i = 0U; i < BOARD_IMG_BYTES; i++) {
        uint8_t b = g_frame[i];

        sum += b;
        if (b == 0x00U) {
            zero++;
        } else if (b == 0xFFU) {
            full++;
        }
        if (b < lo) {
            lo = b;
        }
        if (b > hi) {
            hi = b;
        }
    }

    g_img_sum = sum;

    px00 = (uint16_t)((g_frame[0] << 8) | g_frame[1]);
    pxc  = (uint16_t)((g_frame[(60U * BOARD_IMG_WIDTH + 80U) * 2U] << 8)
                    | g_frame[(60U * BOARD_IMG_WIDTH + 80U) * 2U + 1U]);

    printf("[cam] frame %lu B  min=0x%02X max=0x%02X mean=%lu  zero=%lu ff=%lu\r\n",
           (unsigned long)BOARD_IMG_BYTES,
           (unsigned)lo, (unsigned)hi,
           (unsigned long)(sum / BOARD_IMG_BYTES),
           (unsigned long)zero, (unsigned long)full);
    printf("[cam] pixel(0,0)=0x%04X  pixel(80,60)=0x%04X  dcmi_sr=0x%08lX\r\n",
           (unsigned)px00, (unsigned)pxc,
           (unsigned long)bsp_dcmi_status());
}


/* ---------------------------------------------------------------------------
 * 摄像头总线探针：在 50 ms 窗口里拼命采样 DCMI 那几根脚，
 * 统计每根线被采到"高"的次数。用来区分"摄像头没输出"和"线没接进来"。
 *   PCLK/HREF/VSYNC 都在动、D0..D7 全 0  -> 数据线没进来
 *   全都 0                               -> 摄像头没在出图（PWDN/RESET/XCLK）
 * 注意：这只看 MCU 这一侧的引脚，跟逻辑分析仪量模块那一侧是两回事。
 * ------------------------------------------------------------------------- */
static void probe_camera_bus(uint32_t ms)
{
    uint32_t hi[11];
    uint32_t samples = 0U;
    uint32_t t0 = dwt_cycles();
    uint32_t i;

    for (i = 0U; i < 11U; i++) {
        hi[i] = 0U;
    }

    while (dwt_elapsed_us(t0) < (ms * 1000U)) {
        uint32_t a = GPIOA->IDR;
        uint32_t b = GPIOB->IDR;
        uint32_t c = GPIOC->IDR;
        uint32_t d = GPIOD->IDR;

        hi[0]  += (a >> BOARD_DCMI_PIXCLK_PIN) & 1U;
        hi[1]  += (a >> BOARD_DCMI_HSYNC_PIN)  & 1U;
        hi[2]  += (b >> BOARD_DCMI_VSYNC_PIN)  & 1U;
        hi[3]  += (c >> BOARD_DCMI_D0_PIN)     & 1U;
        hi[4]  += (c >> BOARD_DCMI_D1_PIN)     & 1U;
        hi[5]  += (c >> BOARD_DCMI_D2_PIN)     & 1U;
        hi[6]  += (c >> BOARD_DCMI_D3_PIN)     & 1U;
        hi[7]  += (c >> BOARD_DCMI_D4_PIN)     & 1U;
        hi[8]  += (d >> BOARD_DCMI_D5_PIN)     & 1U;
        hi[9]  += (b >> BOARD_DCMI_D6_PIN)     & 1U;
        hi[10] += (b >> BOARD_DCMI_D7_PIN)     & 1U;
        samples++;
    }

    for (i = 0U; i < 11U; i++) {
        g_probe[i] = hi[i];
    }

    printf("[cam] bus probe %lu samples in %lu ms: PCLK=%lu HREF=%lu VSYNC=%lu\r\n",
           (unsigned long)samples, (unsigned long)ms,
           (unsigned long)hi[0], (unsigned long)hi[1], (unsigned long)hi[2]);
    printf("[cam] data high counts: D0=%lu D1=%lu D2=%lu D3=%lu D4=%lu D5=%lu D6=%lu D7=%lu\r\n",
           (unsigned long)hi[3], (unsigned long)hi[4], (unsigned long)hi[5],
           (unsigned long)hi[6], (unsigned long)hi[7], (unsigned long)hi[8],
           (unsigned long)hi[9], (unsigned long)hi[10]);
}

static void camera_selftest(void)
{
    int      rc;

    if (!ov7670_present()) {
        printf("[cam] OV7670 not answering on SCCB, skip DCMI\r\n");
        return;
    }

    rc = ov7670_init_rgb565_qqvga();
    printf("[cam] OV7670 -> QQVGA RGB565 %s\r\n", (rc == 0) ? "OK" : "PARTIAL (some regs NACK)");

    if (bsp_dcmi_init(g_frame, BOARD_IMG_BYTES) != 0) {
        printf("[cam] DCMI init FAILED\r\n");
        return;
    }

    probe_camera_bus(50U);

    g_poll_words = bsp_dcmi_poll_words((uint32_t *)g_poll_buf, 16U, 300U);

    /* 帧缓冲先填 0xAA：抓完若还是 0xAA，说明 DMA 根本没往这里写；
     * 若变成 0x00，说明 DMA 写了，只是 DCMI 送来的是 0。 */
    memset(g_frame, 0xAA, BOARD_IMG_BYTES);

    /* 位敲采样：绕过 DCMI，直接看焊盘上的字节 */
    {
        static uint8_t bb[256];
        g_bb_words = bsp_cam_bitbang(bb, 64U, 300U);
        for (uint32_t i = 0U; i < 16U; i++) {
            g_bb_buf[i] = bb[i];
        }
    }

    (void)ov7670_test_pattern(1);     /* 打开八色条：与光照无关，便于判断接线 */
    {
        /* 回读关键寄存器，确认写入真的生效了 */
        uint8_t r12 = 0xFFU, r40 = 0xFFU, r0c = 0xFFU, r3e = 0xFFU, r70 = 0xFFU, r71 = 0xFFU;
        (void)ov7670_read_reg(0x12U, &r12);
        (void)ov7670_read_reg(0x40U, &r40);
        (void)ov7670_read_reg(0x0CU, &r0c);
        (void)ov7670_read_reg(0x3EU, &r3e);
        (void)ov7670_read_reg(0x70U, &r70);
        (void)ov7670_read_reg(0x71U, &r71);
        printf("[cam] readback 0x12=%02x 0x40=%02x 0x0C=%02x 0x3E=%02x 0x70=%02x 0x71=%02x\r\n",
               r12, r40, r0c, r3e, r70, r71);
    }
    bsp_cam_raw_snapshot((uint8_t *)g_raw, 4096U);

    /* 对照实验：传感器正常 vs COM2 软件掉电。
     * 两次直方图若几乎一样 → 数据线根本没被传感器驱动（接线/模块问题）；
     * 差异明显 → 数据线确实在传数据，问题在 DCMI 采样。 */
    {
        uint32_t i, z, f, o;

        z = f = o = 0;
        for (i = 0; i < 4096U; i++) {
            uint8_t v = g_raw[i];
            if (v == 0x00U) {
                z++;
            } else if (v == 0xFFU) {
                f++;
            } else {
                o++;
            }
        }
        printf("[cam] awake : zero=%lu ff=%lu other=%lu\r\n",
               (unsigned long)z, (unsigned long)f, (unsigned long)o);

        (void)ov7670_sleep(1);
        dwt_delay_ms(20U);
        bsp_cam_raw_snapshot((uint8_t *)g_raw, 4096U);
        z = f = o = 0;
        for (i = 0; i < 4096U; i++) {
            uint8_t v = g_raw[i];
            if (v == 0x00U) {
                z++;
            } else if (v == 0xFFU) {
                f++;
            } else {
                o++;
            }
        }
        printf("[cam] asleep: zero=%lu ff=%lu other=%lu\r\n",
               (unsigned long)z, (unsigned long)f, (unsigned long)o);
        (void)ov7670_sleep(0);
        dwt_delay_ms(20U);
    }

    /* 连续模式抓两帧（76,800 B），VSYNC 中断给帧首打了标记，
     * 抓完把完整的那一帧挪到缓冲区开头。 */
    g_dcmi_cm  = 0U;   /* 快照模式(CWSIZE)在本板上始终抓不到数据，只能用连续模式 */
    /* 极性：OV7670 的 HREF 与 VSYNC 都是**高有效**，所以 HSPOL/VSPOL 必须为 0。
     * 之前三位全置 1（HSPOL=1 表示"HREF 低有效"），DCMI 于是在
     * 行消隐期采样 —— 那一小段时间数据线上是常数 0x01，
     * 于是"帧长正好 38,400、内容恒为 0x01"。这是本次的关键 bug。
     * PCKPOL=1：在 PCLK 上升沿采样（传感器在下降沿换数据）。 */
    g_dcmi_pol = DCMI_CR_PCKPOL;

    /* 实测"一行到底有多少个 PCLK"：每个 PCLK 出一个字节，
     * 所以这个数字就是每行的字节数（320 = RGB565 正常，160 = 只出一半）。 */
    g_pclk_line = 0U;   /* 轮询数 PCLK 在 8 MHz 下太慢（实测只数到 57），别再用了 */
    printf("[cam] PCLK per line = %lu (expect 320 for RGB565)\r\n",
           (unsigned long)g_pclk_line);
    bsp_dcmi_init(g_frame, BOARD_IMG_BYTES * 2U);
    memset(g_frame, 0xAA, BOARD_IMG_BYTES * 2U);
    rc = bsp_dcmi_capture(500U);
    g_img_ms     = bsp_dcmi_last_ms();
    g_vsync_cnt  = bsp_dcmi_vsync_count();

    if (rc == 0) {
        uint32_t off = bsp_dcmi_frame_offset();
        g_frame_off = off;
        /* 就地平移：memmove 处理重叠，源和目的都在同一块缓冲里 */
        memmove(g_frame, g_frame + off, BOARD_IMG_BYTES);
        rc = 0;
    }
    g_img_result = (int32_t)rc;
    g_img_ms     = bsp_dcmi_last_ms();

    if (rc != 0) {
        printf("[cam] capture %s after %lu ms  sr=0x%08lX\r\n",
               (rc == -1) ? "TIMEOUT" : "ERROR",
               (unsigned long)g_img_ms,
               (unsigned long)bsp_dcmi_status());
        printf("[cam] timeout usually means: no PCLK (XCLK?), D0-D7 not connected, "
               "or PWDN/RESET floating\r\n");
        return;
    }

    printf("[cam] capture OK in %lu ms\r\n", (unsigned long)g_img_ms);
    print_frame_stats();

    /* 把这一帧发给电脑，约 3.3 s（115200）。 */
    printf("[cam] sending %lu bytes over UART, please wait...\r\n",
           (unsigned long)BOARD_IMG_BYTES);
    send_image_header(BOARD_IMG_WIDTH, BOARD_IMG_HEIGHT, BOARD_IMG_BYTES);
    bsp_uart_write((const char *)g_frame, BOARD_IMG_BYTES);
    bsp_uart_flush();
    printf("\r\n[cam] image sent\r\n");
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
    camera_selftest();
    uart_loopback_selftest();

    /* S1b：从这里交给 FreeRTOS。main() 之后不再返回。 */
    app_tasks_start();

    for (;;) {
        /* 不可达 */
    }
}
