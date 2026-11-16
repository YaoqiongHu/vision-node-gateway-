#include "ov7670.h"

#include "bsp_i2c_soft.h"

int ov7670_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2];

    buf[0] = reg;
    buf[1] = val;

    return bsp_i2c_write(OV7670_ADDR7, buf, 2U);
}

int ov7670_read_reg(uint8_t reg, uint8_t *val)
{
    /* 第一步：这是一次"写"，但写的不是数据寄存器，而是芯片内部的
     * "寄存器地址指针"。0x42 = 设备地址 0x21 + 写位，0x0A 被存进那个指针；
     * 帧尾的 STOP 不会清掉它，所以第二步还能接着用。 */

    (void)bsp_i2c_write(OV7670_ADDR7, &reg, 1U);

    /* 第二步：以读地址 0x43 重新起始，芯片把指针指向的寄存器内容吐出来。
     *
     * 为什么不能在一次传输里做完（写完地址立刻掉头读）？
     * 因为那种做法叫"重复起始"，标准 I²C 支持、**SCCB 不支持**，
     * 所以必须 STOP 之后重新 START。 */
    return bsp_i2c_read(OV7670_ADDR7, val, 1U);
}

uint16_t ov7670_chip_id(void)
{
    uint8_t pid = 0U;
    uint8_t ver = 0U;

    (void)ov7670_read_reg(OV7670_REG_PID, &pid);
    (void)ov7670_read_reg(OV7670_REG_VER, &ver);

    return (uint16_t)(((uint16_t)pid << 8) | (uint16_t)ver);
}

int ov7670_present(void)
{
    return (ov7670_chip_id() == ((uint16_t)((OV7670_PID_VALUE << 8) | OV7670_VER_VALUE))) ? 1 : 0;
}
