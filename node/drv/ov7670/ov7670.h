#ifndef OV7670_H
#define OV7670_H

#include <stdint.h>

/* =============================================================================
 *  OV7670 —— 只管"这颗芯片的寄存器怎么读写"
 *
 *  总线怎么打波形是 bsp_i2c_soft 的事，这里一个 GPIO 都不碰。
 *  换一颗摄像头，改这个目录；换一块主板，bsp_i2c_soft 不用动。
 * ========================================================================== */

/** 7 位 SCCB 地址（写 0x42 / 读 0x43 由 bsp 层拼出来） */
#define OV7670_ADDR7        0x21U

#define OV7670_REG_PID      0x0AU   /* 产品高位识别号，固定 0x76 */
#define OV7670_REG_VER      0x0BU   /* 产品低位识别号，固定 0x73 */

#define OV7670_PID_VALUE    0x76U
#define OV7670_VER_VALUE    0x73U

/**
 * 写一个寄存器：START → 0x42 → reg → val → STOP
 * @return 1 = 从机应答；0 = 无应答（SCCB 允许不应答，不能当失败）
 */
int ov7670_write_reg(uint8_t reg, uint8_t val);

/**
 * 读一个寄存器。SCCB 不支持重复起始，所以是**两次独立的传输**：
 *   ① START → 0x42 → reg → STOP      先把寄存器地址告诉它
 *   ② START → 0x43 → 读 1 字节 → NACK → STOP
 * @return 1 = 从机应答；0 = 无应答
 */
int ov7670_read_reg(uint8_t reg, uint8_t *val);

/** 返回 (PID << 8) | VER，正常应该是 0x7673。 */
uint16_t ov7670_chip_id(void);

/** 1 = PID 和 VER 都对，说明 SCCB 通路完全正常。 */
int ov7670_present(void);

#endif /* OV7670_H */
