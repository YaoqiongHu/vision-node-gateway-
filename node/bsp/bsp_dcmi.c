#include <stddef.h>

#include "stm32f4xx.h"
#include "board.h"
#include "bsp_dcmi.h"
#include "bsp_dwt.h"
#include "board_config.h"

/* =============================================================================
 *  DCMI + DMA2 Stream1 Channel1
 *
 *  数据通路：OV7670 → DCMI 引脚 → DCMI 内部 4 字 FIFO → DMA 请求
 *            → DMA2_Stream1 搬到内存（每个 32 位字打包 4 个像素字节）
 *
 *  8 位模式下 DCMI_DR 每个 32 位读数装 4 个像素，所以 DMA 用字（32 位）传输
 *  最省事：NDTR = 字节数 / 4，内存侧自增，外设侧固定 DCMI->DR。
 * ========================================================================== */

#define DCMI_TIMEOUT_MS     500U

static uint8_t *s_buf;
volatile uint32_t g_dcmi_lisr;   /* 最后一次抓帧结束时的 DMA2->LISR，供调试 */
static uint32_t s_bytes;
static uint32_t s_last_ms;

/* ---- 帧同步：VSYNC 中断记录"这一刻 DMA 已经写到缓冲区的第几个字节" ----
 * 轮询 VSYNC 的脉冲太脆弱（脉冲窄、还可能被超时逻辑跳过去），
 * 改由 DCMI 的中断在硬件事件发生的瞬间取 NDTR，误差只有一个中断延迟。 */
#define DCMI_SYNC_SLOTS  8U
static volatile uint32_t s_vs_off[DCMI_SYNC_SLOTS];
static volatile uint32_t s_vs_cnt;
static uint32_t          s_total_words;

/** 极性试验用：CR 的 PCKPOL/HSPOL/VSPOL 三位 */
/* PCKPOL=1（PCLK 上升沿采样）；HSPOL/VSPOL=0（HREF、VSYNC 都是高有效） */
uint32_t g_dcmi_pol = DCMI_CR_PCKPOL;

/** 1 = 快照模式（按 CWSIZE 抓一帧就停），0 = 连续模式 */
uint32_t g_dcmi_cm = 0U;
static uint32_t s_status;

/* ---- DMA2 Stream1 的标志在 LISR 里，位 6..11 ---- */
#define DCMI_DMA_TCIF       DMA_LISR_TCIF1
#define DCMI_DMA_ERR        (DMA_LISR_TEIF1 | DMA_LISR_DMEIF1 | DMA_LISR_FEIF1)
#define DMA_LIFCR_ALL       (DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | \
                             DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1)

void DCMI_IRQHandler(void)
{
    uint32_t risr = BOARD_DCMI->RISR;

    if ((risr & DCMI_RIS_VSYNC_RIS) != 0U) {
        uint32_t used = s_total_words - DMA2_Stream1->NDTR;   /* 已搬走的字数 */
        s_vs_off[s_vs_cnt % DCMI_SYNC_SLOTS] = used * 4U;     /* 换算成字节 */
        s_vs_cnt++;
    }

    BOARD_DCMI->ICR = DCMI_ICR_VSYNC_ISC | DCMI_ICR_FRAME_ISC |
                      DCMI_ICR_LINE_ISC   | DCMI_ICR_OVR_ISC   | DCMI_ICR_ERR_ISC;
}

static void dcmi_pin(GPIO_TypeDef *port, uint32_t pin)
{
    board_gpio_enable_clock(port);
    board_gpio_af(port, pin, BOARD_DCMI_AF);
    board_gpio_speed(port, pin, 3U);   /* 50 MHz，PCLK 最高 8 MHz 绰绰有余 */
    board_gpio_pull(port, pin, 0U);
}

static void dcmi_gpio_init(void)
{
    dcmi_pin(BOARD_DCMI_PIXCLK_PORT, BOARD_DCMI_PIXCLK_PIN);
    dcmi_pin(BOARD_DCMI_HSYNC_PORT,  BOARD_DCMI_HSYNC_PIN);
    dcmi_pin(BOARD_DCMI_VSYNC_PORT,  BOARD_DCMI_VSYNC_PIN);

    dcmi_pin(BOARD_DCMI_D0_PORT, BOARD_DCMI_D0_PIN);
    dcmi_pin(BOARD_DCMI_D1_PORT, BOARD_DCMI_D1_PIN);
    dcmi_pin(BOARD_DCMI_D2_PORT, BOARD_DCMI_D2_PIN);
    dcmi_pin(BOARD_DCMI_D3_PORT, BOARD_DCMI_D3_PIN);
    dcmi_pin(BOARD_DCMI_D4_PORT, BOARD_DCMI_D4_PIN);
    dcmi_pin(BOARD_DCMI_D5_PORT, BOARD_DCMI_D5_PIN);
    dcmi_pin(BOARD_DCMI_D6_PORT, BOARD_DCMI_D6_PIN);
    dcmi_pin(BOARD_DCMI_D7_PORT, BOARD_DCMI_D7_PIN);
}

static void dcmi_dma_init(void)
{
    DMA_Stream_TypeDef *s = DMA2_Stream1;

    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    (void)RCC->AHB1ENR;

    s->CR = 0U;
    while ((s->CR & DMA_SxCR_EN) != 0U) {
        /* 等上一次传输真正停下来才能改寄存器 */
    }

    DMA2->LIFCR = DMA_LIFCR_ALL;

    s->PAR  = (uint32_t)(uintptr_t)&BOARD_DCMI->DR;
    s->M0AR = (uint32_t)(uintptr_t)s_buf;
    s->NDTR = s_bytes / 4U;

    /* 通道 1（DCMI）、外设→内存、字宽、内存自增、普通模式、高优先级 */
    s->CR = (1UL << DMA_SxCR_CHSEL_Pos)                  /* CHSEL = 1 */
          | DMA_SxCR_MSIZE_1                             /* 32 位 */
          | DMA_SxCR_PSIZE_1                             /* 32 位 */
          | DMA_SxCR_MINC                                /* 内存地址自增 */
          | DMA_SxCR_PL_1;                               /* 高优先级 */
    /* 方向：DIR = 00 才是"外设 → 内存"。写 DIR_0（=01）会变成
     * "内存 → 外设"，DMA 会把缓冲区里的内容往 DCMI->DR 里灌、
     * 缓冲区永远不被写入 —— 现象就是"抓帧成功、NDTR 归零、数据全 0"。 */

    /* 直接模式：不进 DMA 自己的 FIFO，外设来一个字就搬一个字。
     * 用 FIFO 模式时，每次收工把流关掉都会置 FEIF（FIFO 里还有残留数据），
     * 下一次开流立刻被判成错误。32 位对 32 位也不需要攒突发。 */
    s->FCR = DMA_SxFCR_DMDIS;
}

int bsp_dcmi_init(uint8_t *buf, uint32_t bytes)
{
    if ((buf == NULL) || (bytes == 0U) || ((bytes % 4U) != 0U)) {
        return -1;
    }

    s_buf    = buf;
    s_bytes  = bytes;
    s_last_ms = 0U;
    s_status = 0U;

    RCC->AHB2ENR |= RCC_AHB2ENR_DCMIEN;
    (void)RCC->AHB2ENR;

    dcmi_gpio_init();
    dcmi_dma_init();

    /* 8 位并行、硬件同步（用 VSYNC/HREF 而不是内嵌码）、
     * 连续模式、PCLK 上升沿采样、VSYNC 与 HREF 都是高电平有效。
     * OV7670：数据在 PCLK 下降沿变化、上升沿稳定，所以必须选上升沿。
     *
     * 为什么用连续模式而不是快照模式：实测快照模式下 DCMI 会在第一个
     * VSYNC 就清掉 CAPTURE（FRAME_RIS 置位），一个像素都到不了 DMA。
     * 改成连续模式、由 DMA 的 NDTR 计满一帧，实测可稳定收满 38,400 字节。
     * 帧对齐由 bsp_dcmi_capture() 里的 VSYNC 边沿同步负责。 */
    BOARD_DCMI->CR = DCMI_CR_ENABLE | g_dcmi_pol | (g_dcmi_cm ? DCMI_CR_CM : 0U);
    BOARD_DCMI->CWSTRTR = 0U;
    BOARD_DCMI->CWSIZER = ((uint32_t)BOARD_IMG_HEIGHT << 16) | (uint32_t)BOARD_IMG_WIDTH;

    BOARD_DCMI->IER = DCMI_IER_VSYNC_IE;   /* 只要 VSYNC：用它给帧首打标记 */

    /* 项目约定：要调 FreeRTOS 的中断一律 5..7。这里不在中断里调任何
     * RTOS API，只读一个寄存器、记一个数，给 6 足够安全。 */
    NVIC_SetPriority(DCMI_IRQn, 6U);
    NVIC_EnableIRQ(DCMI_IRQn);

    return 0;
}

int bsp_dcmi_capture(uint32_t timeout_ms)
{
    DMA_Stream_TypeDef *s = DMA2_Stream1;
    uint32_t t0;
    uint32_t elapsed;
    int      ret = 0;

    if (s_buf == NULL) {
        return -1;
    }

    /* 1. 复位 DMA 流，重新装帧长 */
    s->CR &= ~DMA_SxCR_EN;
    while ((s->CR & DMA_SxCR_EN) != 0U) {
    }
    DMA2->LIFCR = DMA_LIFCR_ALL;
    s->NDTR = s_bytes / 4U;

    /* 2. 让 DCMI 先跑起来 */
    BOARD_DCMI->ICR = DCMI_ICR_OVR_ISC | DCMI_ICR_ERR_ISC
                    | DCMI_ICR_FRAME_ISC | DCMI_ICR_VSYNC_ISC | DCMI_ICR_LINE_ISC;
    BOARD_DCMI->CR |= DCMI_CR_CAPTURE;

    /* 3. 直接把 DMA 开起来，让缓冲区从任意位置开始填。
     *    帧首对齐不在这里做 —— 由 DCMI 的 VSYNC 中断记录偏移量，
     *    搬完之后再用 bsp_dcmi_frame_offset() + memmove 对齐（见 main.c）。
     *    缓冲区要按"两帧"申请（BOARD_IMG_BYTES * 2）。 */
    s_vs_cnt   = 0U;
    s_total_words = s_bytes / 4U;

    /* 4. 开 DMA，等它把缓冲区的字搬完 */
    s->CR |= DMA_SxCR_EN;
    t0 = dwt_cycles();
    while ((DMA2->LISR & DCMI_DMA_TCIF) == 0U) {
        elapsed = dwt_elapsed_us(t0) / 1000U;
        if (elapsed > timeout_ms) {
            ret = -1;
            break;
        }
        if ((DMA2->LISR & DCMI_DMA_ERR) != 0U) {
            ret = -2;
            break;
        }
    }

    s_last_ms = dwt_elapsed_us(t0) / 1000U;
    /* SR 只有 bit0/1/2（HSYNC、VSYNC、FNE），溢出和同步错误在 RISR 里；
     * 打包成 0xRRRR_SSSS 一起留给调用方看。 */
    s_status  = (BOARD_DCMI->SR & 0x7UL) | ((BOARD_DCMI->RISR & 0x1FUL) << 8);

    g_dcmi_lisr = DMA2->LISR;

    s->CR &= ~DMA_SxCR_EN;
    while ((s->CR & DMA_SxCR_EN) != 0U) {
    }

out:
    BOARD_DCMI->CR &= ~DCMI_CR_CAPTURE;
    DMA2->LIFCR = DMA_LIFCR_ALL;
    BOARD_DCMI->ICR = DCMI_ICR_OVR_ISC | DCMI_ICR_ERR_ISC
                    | DCMI_ICR_FRAME_ISC | DCMI_ICR_VSYNC_ISC | DCMI_ICR_LINE_ISC;

    return ret;
}

uint32_t bsp_dcmi_last_ms(void)
{
    return s_last_ms;
}

uint32_t bsp_dcmi_status(void)
{
    return s_status;
}

/* ---------------------------------------------------------------------------
 * 调试用：完全不经过 DMA，CPU 死等 DCMI 的 FNE 位，一个一个读 DR。
 * 用来判断"数据到底有没有进到 DCMI"，与 DMA 通路解耦。
 * 返回实际读到的 32 位字个数。慢，只用于小样本。
 * ------------------------------------------------------------------------- */
/* 直接数"一行里有多少个 PCLK 上升沿"。
 * 这个数字就是一行的**字节数**（RGB565 每个 PCLK 出一个字节），
 * 用它可以直接判定 320 还是 160，从而定下每行该收多少字节。 */
uint32_t bsp_cam_pclk_per_line(uint32_t timeout_ms)
{
    uint32_t guard = 0U, limit = timeout_ms * 20000U;
    uint32_t n = 0U, prev;

    while ((GPIOA->IDR & (1U << 4)) != 0U) {            /* 先等 HREF 变低 */
        if (++guard > limit) { return 0U; }
    }
    while ((GPIOA->IDR & (1U << 4)) == 0U) {            /* 再等 HREF 变高 */
        if (++guard > limit) { return 0U; }
    }
    prev = GPIOA->IDR & (1U << 6);
    while ((GPIOA->IDR & (1U << 4)) != 0U) {            /* HREF 高电平期间数 PCLK */
        uint32_t cur = GPIOA->IDR & (1U << 6);
        if ((prev == 0U) && (cur != 0U)) {
            n++;
        }
        prev = cur;
        if (++guard > limit) { return n; }
    }
    return n;
}

uint32_t bsp_dcmi_poll_words(uint32_t *out, uint32_t words, uint32_t timeout_ms)
{
    uint32_t n = 0U;
    uint32_t t0 = dwt_cycles();

    BOARD_DCMI->ICR = DCMI_ICR_OVR_ISC | DCMI_ICR_ERR_ISC | DCMI_ICR_FRAME_ISC;
    BOARD_DCMI->CR |= DCMI_CR_CAPTURE;

    while ((n < words) && (dwt_elapsed_us(t0) < (timeout_ms * 1000U))) {
        if ((BOARD_DCMI->SR & DCMI_SR_FNE) != 0U) {
            out[n++] = BOARD_DCMI->DR;
        }
    }

    BOARD_DCMI->CR &= ~DCMI_CR_CAPTURE;
    s_status = (uint32_t)(BOARD_DCMI->SR & 0x7U) |
               ((uint32_t)(BOARD_DCMI->RISR & 0x1FU) << 8);

    return n;
}

/* ---------------------------------------------------------------------------
 * 调试用：完全不用 DCMI 外设，用 CPU + IDR 位敲着采一行数据。
 * 目的：绕开 DCMI 的同步/极性配置，直接证明"这 8 个焊盘上到底有没有像素"。
 * 采样时机：等 PCLK 上升沿后立刻读 IDR（和 DCMI 的做法一致）。
 * 返回采到的字节数。
 * ------------------------------------------------------------------------- */
uint32_t bsp_cam_bitbang(uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms)
{
    uint32_t n = 0U;
    uint32_t t0 = dwt_cycles();

    /* 等一帧结束（VSYNC 从高到低），从帧首开始采 */
    while (((GPIOB->IDR >> 7) & 1U) == 0U) {
        if (dwt_elapsed_us(t0) > timeout_ms * 1000U) { return 0U; }
    }
    while (((GPIOB->IDR >> 7) & 1U) != 0U) {
        if (dwt_elapsed_us(t0) > timeout_ms * 1000U) { return 0U; }
    }

    while (n < max_bytes) {
        uint32_t c, b;

        if (dwt_elapsed_us(t0) > timeout_ms * 1000U) { break; }
        if (((GPIOA->IDR >> 4) & 1U) == 0U) { continue; }      /* 等 HREF 有效 */

        while (((GPIOA->IDR >> 6) & 1U) == 0U) { }             /* 等 PCLK 上升沿 */
        c = GPIOC->IDR;
        b = GPIOB->IDR;

        buf[n++] = (uint8_t)((((c >> 6) & 1U))       |
                             (((c >> 7) & 1U) << 1)  |
                             (((c >> 8) & 1U) << 2)  |
                             (((c >> 9) & 1U) << 3)  |
                             (((c >> 11) & 1U) << 4) |
                             (((b >> 6) & 1U) << 5)  |
                             (((b >> 8) & 1U) << 6)  |
                             (((b >> 9) & 1U) << 7));

        while (((GPIOA->IDR >> 6) & 1U) != 0U) { }             /* 等 PCLK 下降沿 */
    }

    return n;
}

/* 整个帧都用位敲采样（调试用）：先等 VSYNC 下降沿，然后一直采到
 * max_bytes 满或 VSYNC 再次变高为止。用来验证"传感器 + 接线"本身好不好。 */
uint32_t bsp_cam_bitbang_frame(uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms)
{
    uint32_t n = 0U;
    uint32_t t0 = dwt_cycles();

    while (((GPIOB->IDR >> 7) & 1U) == 0U) {
        if (dwt_elapsed_us(t0) > timeout_ms * 1000U) { return 0U; }
    }
    while (((GPIOB->IDR >> 7) & 1U) != 0U) {
        if (dwt_elapsed_us(t0) > timeout_ms * 1000U) { return 0U; }
    }

    while ((n < max_bytes) && (dwt_elapsed_us(t0) < timeout_ms * 1000U)) {
        uint32_t c, b;

        if (((GPIOA->IDR >> 4) & 1U) == 0U) { continue; }
        while (((GPIOA->IDR >> 6) & 1U) == 0U) { }
        c = GPIOC->IDR;
        b = GPIOB->IDR;
        buf[n++] = (uint8_t)((((c >> 6) & 1U))       |
                             (((c >> 7) & 1U) << 1)  |
                             (((c >> 8) & 1U) << 2)  |
                             (((c >> 9) & 1U) << 3)  |
                             (((c >> 11) & 1U) << 4) |
                             (((b >> 6) & 1U) << 5)  |
                             (((b >> 8) & 1U) << 6)  |
                             (((b >> 9) & 1U) << 7));
        while (((GPIOA->IDR >> 6) & 1U) != 0U) { }
    }

    return n;
}

/* ------------------------------------------------------------------ *
 *  调试用：以最快速度连续采样 8 根数据线，不做任何门控
 *
 *  用途：判断数据焊盘上到底有没有"被驱动"的信号。
 *  采样循环约 8 个周期一次（21 M 采样/秒），足以看清 8 MHz 的 PCLK 数据。
 *  分析时看两点：每位的占空比是否接近 50%、相邻样本的变化率是否高。
 * ------------------------------------------------------------------ */
void bsp_cam_raw_snapshot(uint8_t *buf, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t c = GPIOC->IDR;
        uint32_t b = GPIOB->IDR;
        buf[i] = (uint8_t)(((c >> 6) & 0x0FU) |
                           (((c >> 11) & 0x01U) << 4) |
                           (((b >> 8) & 0x01U) << 5) |
                           (((b >> 9) & 0x01U) << 6) |
                           (((b >> 6) & 0x01U) << 7));
    }
}

/* ------------------------------------------------------------------ *
 *  从 VSYNC 中断记下的偏移里，挑一个"后面还够一整帧"的作为帧首
 *
 *  缓冲里通常能记到两个 VSYNC：第一帧开头、第二帧开头。
 *  选"不超过 (总长 - 一帧)"的最大偏移，就得到一帧完整且对齐的数据。
 * ------------------------------------------------------------------ */
uint32_t bsp_dcmi_frame_offset(void)
{
    uint32_t i;
    uint32_t best  = 0U;
    uint32_t limit = (s_bytes > (uint32_t)BOARD_IMG_BYTES)
                   ? (s_bytes - (uint32_t)BOARD_IMG_BYTES) : 0U;
    uint32_t n     = (s_vs_cnt < DCMI_SYNC_SLOTS) ? s_vs_cnt : DCMI_SYNC_SLOTS;

    for (i = 0U; i < n; i++) {
        uint32_t off = s_vs_off[i] & ~3UL;   /* 4 字节对齐 */
        if ((off <= limit) && (off > best)) {
            best = off;
        }
    }
    return best;
}

uint32_t bsp_dcmi_vsync_count(void)
{
    return s_vs_cnt;
}
