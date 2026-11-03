#ifndef BSP_XCLK_H
#define BSP_XCLK_H

#include <stdint.h>

/* =============================================================================
 *  bsp_xclk —— 给摄像头送时钟（MCO1）
 *
 *  为什么需要：OV7670 没有内部晶振，XCLK 断掉时它连 SCCB 都不应答。
 *  为什么用 MCO1：不占定时器、不用中断，配两个寄存器就有一个稳定的方波。
 *  为什么不放 drv/：这是"板子上 PA8 接了根线"的事实，不是摄像头芯片的知识。
 * ========================================================================== */

/**
 * 把 MCO1 配置成 XCLK 输出，引脚与频率来自 board_config.h。
 *
 * @return true  = 已配置，且 HSE 确实在振（PA8 上有波形）
 *         false = HSE 没起振（SystemInit 退回 HSI 了），此时不能选它做源，
 *                 否则 PA8 上是一条死电平 —— 接线前必须先排除这种可能。
 */
int bsp_xclk_init(void);

/** 实际输出频率（Hz）。HSE 没振或未初始化时返回 0。 */
uint32_t bsp_xclk_hz(void);

/** 当前配置下"MCO1 选了哪个源"，用于串口打印自检：返回 "HSE"/"PLL"/"HSI"/"LSE"/"?"。 */
const char *bsp_xclk_source_name(void);

#endif /* BSP_XCLK_H */
