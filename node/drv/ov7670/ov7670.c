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
    /* 第一步：把寄存器地址送进去，然后 STOP。
     * SCCB 没有"当前地址指针"这个概念，所以每次读都得重新指一次。 */
    (void)bsp_i2c_write(OV7670_ADDR7, &reg, 1U);

    /* 第二步：重新起始，以读地址访问，读一个字节，主机回 NACK。 */
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
