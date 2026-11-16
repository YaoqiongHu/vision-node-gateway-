#include "bsp_i2c_soft.h"

#include <stdio.h>

#include "stm32f4xx.h"
#include "board.h"
#include "board_config.h"
#include "bsp_dwt.h"

/* =============================================================================
 *  位电平操作
 *
 *  用 BSRR 而不是 ODR 的读-改-写：BSRR 是"写 1 生效"的原子寄存器，
 *  置位写低 16 位、清零写高 16 位，不会影响同一端口的其他引脚。
 *
 *  "输出 1" 在开漏模式下等于"释放总线"（高阻），电平由上拉电阻决定 ——
 *  这就是 I²C 能多个器件挂一条线的根本原因。
 * ========================================================================== */

#define SCL_HIGH()  (BOARD_SCCB_SCL_PORT->BSRR = (1UL << BOARD_SCCB_SCL_PIN))
#define SCL_LOW()   (BOARD_SCCB_SCL_PORT->BSRR = (1UL << (BOARD_SCCB_SCL_PIN + 16U)))
#define SDA_HIGH()  (BOARD_SCCB_SDA_PORT->BSRR = (1UL << BOARD_SCCB_SDA_PIN))
#define SDA_LOW()   (BOARD_SCCB_SDA_PORT->BSRR = (1UL << (BOARD_SCCB_SDA_PIN + 16U)))

/** 读引脚的真实电平。输入模式下 IDR 就是外部电平。 */
static unsigned sda_read(void)
{
    return (unsigned)((BOARD_SCCB_SDA_PORT->IDR >> BOARD_SCCB_SDA_PIN) & 1UL);
}

/** 半周期延时。5 µs 时约 100 kHz（含 GPIO 操作开销会略低一点）。 */
static void half_delay(void)
{
    dwt_delay_us(BOARD_SCCB_HALF_US);
}

/* ---------------------------------------------------------------- 引脚方向 */

/* SDA 是双向的：写的时候当开漏输出，读应答/数据的时候要切成输入。
 * 切回输出前先把 ODR 置 1（释放），否则会瞬间把线拉低一下。 */
static void sda_dir_input(void)
{
    uint32_t shift = BOARD_SCCB_SDA_PIN * 2U;

    BOARD_SCCB_SDA_PORT->MODER &= ~(3UL << shift);          /* 00 = 输入 */
}

static void sda_dir_output(void)
{
    uint32_t shift = BOARD_SCCB_SDA_PIN * 2U;

    SDA_HIGH();                                              /* 先释放 */
    BOARD_SCCB_SDA_PORT->MODER =
        (BOARD_SCCB_SDA_PORT->MODER & ~(3UL << shift)) | (1UL << shift);  /* 01 = 输出 */
}

/* ---------------------------------------------------------------- 时序原语 */

static void i2c_start(void)
{
    sda_dir_output();
    SDA_HIGH();
    SCL_HIGH();
    half_delay();
    SDA_LOW();          /* SCL 为高时 SDA 下降 —— 起始条件 */
    half_delay();
    SCL_LOW();
    half_delay();
}

static void i2c_stop(void)
{
    sda_dir_output();
    SDA_LOW();
    half_delay();
    SCL_HIGH();
    half_delay();
    SDA_HIGH();         /* SCL 为高时 SDA 上升 —— 停止条件 */
    half_delay();
}

/** 发一个字节（高位在前），返回从机在第 9 个时钟是否把 SDA 拉低。 */
static int i2c_write_byte(uint8_t byte)
{
    unsigned nack;
    int      i;

    for (i = 7; i >= 0; i--) {
        SCL_LOW();
        if (((byte >> i) & 1U) != 0U) {
            SDA_HIGH();
        } else {
            SDA_LOW();
        }
        half_delay();
        SCL_HIGH();
        half_delay();
    }

    /* 第 9 个时钟：主机释放 SDA，让从机表态 */
    SCL_LOW();
    sda_dir_input();
    half_delay();
    SCL_HIGH();
    half_delay();
    nack = sda_read();          /* 0 = 被从机拉低 = 应答 */
    SCL_LOW();
    sda_dir_output();
    half_delay();

    return (nack == 0U) ? 1 : 0;
}

/** 读一个字节。ack 非 0 表示主机在本字节后回 ACK，0 表示回 NACK。 */
static uint8_t i2c_read_byte(int ack)
{
    uint8_t  byte = 0U;
    int      i;

    sda_dir_input();
    for (i = 7; i >= 0; i--) {
        SCL_LOW();
        half_delay();
        SCL_HIGH();
        half_delay();
        byte = (uint8_t)((byte << 1) | (uint8_t)sda_read());
    }

    /* 第 9 个时钟：主机给出 ACK（拉低）或 NACK（释放） */
    SCL_LOW();
    sda_dir_output();
    if (ack != 0) {
        SDA_LOW();
    } else {
        SDA_HIGH();
    }
    half_delay();
    SCL_HIGH();
    half_delay();
    SCL_LOW();
    SDA_HIGH();
    half_delay();

    return byte;
}

static void i2c_pin_init(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = pin * 2U;

    board_gpio_enable_clock(port);

    port->MODER = (port->MODER & ~(3UL << shift)) | (1UL << shift);  /* 01 = 通用输出 */
    port->OTYPER |= (1UL << pin);                                    /* 1  = 开漏 */
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | (2UL << shift); /* 10 = 高速 */
    port->PUPDR = (port->PUPDR & ~(3UL << shift)) | (1UL << shift);  /* 01 = 内部上拉兜底 */
    port->BSRR = (1UL << pin);                                       /* 释放总线 */
}

/* ---------------------------------------------------------------- 对外接口 */

void bsp_i2c_init(void)
{
    i2c_pin_init(BOARD_SCCB_SCL_PORT, BOARD_SCCB_SCL_PIN);
    i2c_pin_init(BOARD_SCCB_SDA_PORT, BOARD_SCCB_SDA_PIN);
}

int bsp_i2c_probe(uint8_t addr7)
{
    int ack;

    i2c_start();
    ack = i2c_write_byte((uint8_t)(addr7 << 1));
    i2c_stop();

    return ack;
}

int bsp_i2c_write(uint8_t addr7, const uint8_t *buf, uint32_t n)
{
    uint32_t i;
    int      ack;

    i2c_start();
    ack = i2c_write_byte((uint8_t)(addr7 << 1));
    for (i = 0U; i < n; i++) {
        (void)i2c_write_byte(buf[i]);
    }
    i2c_stop();

    return ack;
}

int bsp_i2c_read(uint8_t addr7, uint8_t *buf, uint32_t n)
{
    uint32_t i;
    int      ack;

    i2c_start();
    ack = i2c_write_byte((uint8_t)((addr7 << 1) | 1U));
    for (i = 0U; i < n; i++) {
        buf[i] = i2c_read_byte((i + 1U) < n);
    }
    i2c_stop();

    return ack;
}

int bsp_i2c_scan(void)
{
    uint8_t addr;
    int     found = 0;

    printf("[i2c] scan 0x08-0x77:");

    for (addr = 0x08U; addr <= 0x77U; addr++) {
        if (bsp_i2c_probe(addr) != 0) {
            printf(" find device 0x%02X", (unsigned)addr);
            found++;
        }
    }

    if (found == 0) {
        printf(" (none)");
    }
    printf("  -> %d device(s)\r\n", found);

    return found;
}
