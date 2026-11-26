#ifndef BSP_DCMI_H
#define BSP_DCMI_H

#include <stdint.h>

/* =============================================================================
 *  DCMI + DMA 抓一帧
 *
 *  OV7670 的像素时钟、行同步、帧同步都是它自己在推，MCU 这边只做"被动接收"：
 *    PIXCLK 上升沿 → 数据线上是一个有效字节
 *    HREF 高      → 本行有效
 *    VSYNC 高     → 新一轮帧开始
 *
 *  为什么必须用 DMA：QQVGA RGB565 一帧 38,400 字节，PCLK 8 MHz 下
 *  每 125 ns 就有一个字节，用中断或轮询根本来不及。
 *
 *  为什么用"快照模式"（DCMI_CR.CM=1）而不是连续采集：
 *  快照模式下，置位 CAPTURE 后硬件会**等到下一个 VSYNC** 才开始收，
 *  收满一帧自动把 CAPTURE 清零 —— 天然对齐帧边界，不会出现"半帧错位"。
 *
 *  缓冲放普通 SRAM（.bss），DMA 可达；**不能放 CCM**。
 * ========================================================================== */

/** 配置 11 根 DCMI 引脚 + DMA2 Stream1，并准备好接收。缓冲区由调用方给。 */
int bsp_dcmi_init(uint8_t *buf, uint32_t bytes);

/**
 * 抓一帧，阻塞到收满或超时。
 * @return 0 = 收到一整帧；-1 = 超时（没接摄像头 / 没有 PCLK）；
 *         -2 = DCMI 报了溢出错误（帧率太高或内存太慢）
 */
int bsp_dcmi_capture(uint32_t timeout_ms);

/** 最近一次抓帧用掉的毫秒数（0 表示还没抓过） */
uint32_t bsp_dcmi_last_ms(void);
uint32_t bsp_dcmi_frame_offset(void);
uint32_t bsp_dcmi_vsync_count(void);

/** DCMI 状态快照，出错时看它：
 *  低 3 位来自 SR（bit0 HSYNC / bit1 VSYNC / bit2 FIFO 非空）；
 *  高 5 位来自 RIS（bit8 溢出 / bit9 同步错误 / bit10 VSYNC / bit11 行 / bit12 收完）。 */
uint32_t bsp_dcmi_status(void);

/* 调试用：不走 DMA，CPU 直接读 DCMI->DR（返回写入的字数）。 */
uint32_t bsp_cam_bitbang_frame(uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms);

uint32_t bsp_cam_bitbang(uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms);

uint32_t bsp_dcmi_poll_words(uint32_t *out, uint32_t words, uint32_t timeout_ms);

/* 数一行里 PCLK 的上升沿个数 = 每行字节数的实测值 */
uint32_t bsp_cam_pclk_per_line(uint32_t timeout_ms);

#endif /* BSP_DCMI_H */
