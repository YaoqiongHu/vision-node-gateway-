#include "ov7670.h"

#include "bsp_dwt.h"
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

/* =========================================================================--
 *  QQVGA RGB565 初始化序列
 *
 *  目标：出 160×120、每像素 2 字节（RGB565）的图，DCW 做硬件 2:1 降采样。
 *  寄存器依据：
 *    COM7(0x12)  0x14 = QVGA(0x10) | RGB(0x04)
 *    COM3(0x0C)  0x0C = 缩放使能(bit3) | DCW 使能(bit2)
 *    COM14(0x3E) 0x19 = DCW&PCLK 使能(bit4) | 手动缩放(bit3) | PCLK/2(bit2:0=001)
 *    0x72        0x11 = 行、列都 2 取 1
 *    0x73        0xF1 = DSP 的 PCLK 分频与 COM14 保持一致
 *  手册的坑：COM7 的格式表在 1.4 版中文手册里把 RGB 和 Bayer 两行印反了，
 *  以 COM15 位[5:4] 的原文和 Linux 驱动为准 → RGB = 0x04。
 * ========================================================================= */

typedef struct {
    uint8_t reg;
    uint8_t val;
} ov7670_reg_t;

/* 传感器默认值里对成像影响最大的几项，先给一套能看的设置；
 * 白平衡/曝光先交给自动（COM8/COM9），S5 调二值化时再回来细调。 */
static const ov7670_reg_t qqvga_rgb565_seq[] = {
    { 0x12U, 0x80U },   /* COM7：软复位，所有寄存器回默认值 */
    { 0x11U, 0x80U },   /* CLKRC：不预分频，内部时钟直接用 XCLK(8 MHz) */
    { 0x12U, 0x14U },   /* COM7：QVGA + RGB */
    { 0x0CU, 0x0CU },   /* COM3：缩放 + DCW 使能 */
    { 0x3EU, 0x19U },   /* COM14：DCW 使能、手动缩放、PCLK/2 */
    { 0x70U, 0x3AU },   /* SCALING_XSC：不开测试图案 */
    { 0x71U, 0x35U },   /* SCALING_YSC */
    { 0x72U, 0x11U },   /* SCALING_DCWCTR：行列都 2:1 */
    { 0x73U, 0xF1U },   /* SCALING_PCLK_DIV */
    { 0x40U, 0xD0U },   /* COM15：RGB565、输出范围 [00]-[FF] */
    { 0x3AU, 0x04U },   /* TSLB */
    { 0x13U, 0xE7U },   /* COM8：快速 AGC/AEC + AWB 全开 */
    { 0x14U, 0x4AU },   /* COM9：AGC 上限 8x、AEC 上限 8x */
    { 0x00U, 0x00U },   /* GAIN：自动 */
    { 0x24U, 0x75U },   /* AEW */
    { 0x25U, 0x63U },   /* AEB */
    { 0x26U, 0xE3U },   /* VPT */
    { 0x6BU, 0x4AU },   /* DBLV */
    { 0x42U, 0x00U },   /* 关闭 UV 自动调整，颜色别偏太远 */
    { 0x4DU, 0x40U },   /* 去噪相关 */
    { 0x4EU, 0x20U }
};

int ov7670_init_rgb565_qqvga(void)
{
    uint32_t i;
    uint32_t bad = 0U;

    for (i = 0U; i < (sizeof(qqvga_rgb565_seq) / sizeof(qqvga_rgb565_seq[0])); i++) {
        if (ov7670_write_reg(qqvga_rgb565_seq[i].reg, qqvga_rgb565_seq[i].val) == 0) {
            bad++;
        }

        /* 软复位之后要等一会儿，芯片内部要重新初始化 PLL 和时序 */
        if ((qqvga_rgb565_seq[i].reg == 0x12U) && (qqvga_rgb565_seq[i].val == 0x80U)) {
            dwt_delay_ms(10U);
        }
    }

    /* 改完格式再等一会儿，让 AGC/AEC 跑几帧稳定下来 */
    dwt_delay_ms(50U);

    return (bad == 0U) ? 0 : -1;
}

/* ------------------------------------------------------------------ *
 *  彩条测试图案（S3 调试用）
 *
 *  SCALING_XSC(0x70)/SCALING_YSC(0x71) 的 bit7 组合：
 *      00 = 正常图像      01 = 移位"1"
 *      10 = 八色条        11 = 渐变灰条
 *  打开后输出**与光照无关**的已知图案，用来区分
 *  "接线不对" 和 "画面太暗"（后者数据几乎全 0）。
 * ------------------------------------------------------------------ */
/* COM2 (0x09) 的 bit6 = 软件掉电（sleep）。
 * 用来做"传感器到底有没有驱动数据线"的对照实验：
 * 睡着后输出级应当三态，数据线不再被驱动。 */
int ov7670_sleep(int on)
{
    uint8_t v;

    if (ov7670_read_reg(0x09U, &v) != 0) {
        return -1;
    }
    v = (uint8_t)(on ? (v | 0x40U) : (v & (uint8_t)~0x40U));
    return ov7670_write_reg(0x09U, v);
}

int ov7670_test_pattern(int on)
{
    /* XSC[7]/YSC[7] 组合：00 正常、01 移位1、10 八色条、11 渐变灰条 */
    uint8_t xsc = on ? 0xBAU : 0x3AU;   /* bit7=1 + 原来的 0x3A → 八色条 */
    uint8_t ysc = on ? 0x35U : 0x35U;   /* bit7=0                        */

    if (!ov7670_present()) {
        return -1;
    }
    if (ov7670_write_reg(0x70U, xsc) != 0) {
        return -1;
    }
    if (ov7670_write_reg(0x71U, ysc) != 0) {
        return -1;
    }
    return 0;
}
