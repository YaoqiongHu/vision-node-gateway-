#include <stddef.h>

#include "bsp_can.h"

#include "board.h"
#include "board_config.h"
#include "bsp_dwt.h"

/* =============================================================================
 *  bxCAN1 轮询版
 *
 *  初始化顺序（RM0090 §32.4）：
 *    1. 开 APB1 的 CAN1 时钟，配 PD0/PD1 为 AF9
 *    2. 置 MCR.INRQ 进初始化模式，等 MSR.INAK = 1
 *    3. 写 BTR（位定时）、BTR.LBKM（要不要回环）
 *    4. 配过滤器（这里用"全通过"）
 *    5. 清 MCR.INRQ，等 MSR.INAK = 0 —— 此后控制器才真正挂到总线上
 *
 *  两个容易踩的点：
 *    - **MCR 与 BTR 只有在初始化模式里才能改**，否则写不进去、也不报错；
 *    - 退出初始化模式时要等 INAK 清零，硬件内部还要同步到总线空闲位。
 * ========================================================================== */

#define CAN_TIMEOUT_US      10000U   /* 10 ms，等任何一个状态位都绰绰有余 */
#define CAN_TX_TIMEOUT_US   20000U   /* 一帧 500 kbps 约 250 µs；没 ACK 时会一直重发，20 ms 足够判定失败 */

#define CAN_STD_ID_MAX      0x7FFU

/* S2 调试用：把回环自测每一步的返回值留下来，用调试器读。
 * 正式版本（S5 之后）会删掉。 */
volatile uint32_t g_can_dbg[12];

/* 等 MSR.INAK 变成 want（1 = 进初始化模式，0 = 退出） */
static int can_wait_inak(uint32_t want)
{
    uint32_t t0 = dwt_cycles();

    while (1) {
        uint32_t inak = ((BOARD_CAN->MSR & CAN_MSR_INAK) != 0U) ? 1U : 0U;
        if (inak == want) {
            return 0;
        }
        if (dwt_elapsed_us(t0) > CAN_TIMEOUT_US) {
            return -1;
        }
    }
}

static int can_enter_init(void)
{
    BOARD_CAN->MCR |= CAN_MCR_INRQ;
    return can_wait_inak(1U);
}

static int can_leave_init(void)
{
    BOARD_CAN->MCR &= ~CAN_MCR_INRQ;
    return can_wait_inak(0U);
}

/* 过滤器 0：32 位掩码模式，ID 与 mask 全 0 → 任何帧都收进 FIFO0。
 * 只开 1 组；F407 有 28 组，以后要按 ID 分组时再用。 */
static void can_filter_accept_all(void)
{
    BOARD_CAN->FMR |= CAN_FMR_FINIT;          /* 进过滤器初始化模式 */
    BOARD_CAN->FA1R = 0U;                     /* 先全关 */

    BOARD_CAN->FS1R = CAN_FS1R_FSC0;          /* 组 0 = 单个 32 位过滤器 */
    BOARD_CAN->FM1R = CAN_FM1R_FBM0;          /* 组 0 = 掩码模式（0 = ID 列表模式）*/
    BOARD_CAN->FFA1R &= ~CAN_FFA1R_FFA0;      /* 组 0 的帧进 FIFO0 */

    BOARD_CAN->sFilterRegister[0].FR1 = 0U;   /* ID   = 0 */
    BOARD_CAN->sFilterRegister[0].FR2 = 0U;   /* mask = 0 → 全不关心 */

    BOARD_CAN->FA1R = CAN_FA1R_FACT0;         /* 使能组 0 */
    BOARD_CAN->FMR &= ~CAN_FMR_FINIT;         /* 退出过滤器初始化模式 */
}

/* 直接把一帧塞进邮箱 0 发出去，等到 RQCP0 置位再返回。
 * 返回 0 = TXOK0 置位（对方 ACK 了），-1 = 超时或总线错误。 */
static int can_send_raw(const bsp_can_frame_t *f)
{
    uint32_t t0;
    uint32_t ok;

    if ((f == NULL) || (f->len > 8U) || (f->id > CAN_STD_ID_MAX)) {
        return -1;
    }

    t0 = dwt_cycles();
    while ((BOARD_CAN->TSR & CAN_TSR_TME0) == 0U) {       /* 邮箱 0 空了吗 */
        if (dwt_elapsed_us(t0) > CAN_TIMEOUT_US) {
            return -1;
        }
    }

    BOARD_CAN->sTxMailBox[0].TDTR = (uint32_t)f->len;     /* 标准帧、无时间戳 */
    BOARD_CAN->sTxMailBox[0].TDLR = (uint32_t)f->data[0]
                                  | ((uint32_t)f->data[1] << 8)
                                  | ((uint32_t)f->data[2] << 16)
                                  | ((uint32_t)f->data[3] << 24);
    BOARD_CAN->sTxMailBox[0].TDHR = (uint32_t)f->data[4]
                                  | ((uint32_t)f->data[5] << 8)
                                  | ((uint32_t)f->data[6] << 16)
                                  | ((uint32_t)f->data[7] << 24);
    /* 写 TIR 的瞬间硬件开始发（TXRQ 必须最后写） */
    BOARD_CAN->sTxMailBox[0].TIR = ((f->id & CAN_STD_ID_MAX) << CAN_TI0R_STID_Pos)
                                 | CAN_TI0R_TXRQ;

    t0 = dwt_cycles();
    while ((BOARD_CAN->TSR & CAN_TSR_RQCP0) == 0U) {      /* 这一帧发完了吗 */
        if (dwt_elapsed_us(t0) > CAN_TX_TIMEOUT_US) {
            return -1;
        }
    }

    ok = BOARD_CAN->TSR & CAN_TSR_TXOK0;
    BOARD_CAN->TSR = CAN_TSR_RQCP0;                       /* 写 1 清标志 */

    return (ok != 0U) ? 0 : -1;
}

int bsp_can_init(void)
{
    uint32_t t0;

    /* 1. 时钟与引脚 */
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;
    (void)RCC->APB1ENR;                        /* 读回，等时钟稳定 */

    /* board_gpio_af() 只写 MODER/AFR，不开端口时钟 —— 必须先开，
     * 否则对 GPIOD 的写全部被硬件丢弃（现象：MODER 读回来一直是 0）。 */
    board_gpio_enable_clock(BOARD_CAN_TX_PORT);
    board_gpio_enable_clock(BOARD_CAN_RX_PORT);

    /* CAN1_TX = PD1，推挽、高速；CAN1_RX = PD0，上拉（收发器没接时不至于悬空乱收） */
    board_gpio_af(BOARD_CAN_TX_PORT, BOARD_CAN_TX_PIN, BOARD_CAN_AF);
    board_gpio_speed(BOARD_CAN_TX_PORT, BOARD_CAN_TX_PIN, 2U);
    board_gpio_pull(BOARD_CAN_TX_PORT, BOARD_CAN_TX_PIN, 0U);

    board_gpio_af(BOARD_CAN_RX_PORT, BOARD_CAN_RX_PIN, BOARD_CAN_AF);
    board_gpio_speed(BOARD_CAN_RX_PORT, BOARD_CAN_RX_PIN, 2U);
    board_gpio_pull(BOARD_CAN_RX_PORT, BOARD_CAN_RX_PIN, 1U);

    /* 2. 先退出睡眠：复位后 MCR.SLEEP = 1，
     *    不唤醒的话控制器不收不发、也不报错（这个坑很隐蔽）。 */
    BOARD_CAN->MCR &= ~CAN_MCR_SLEEP;
    t0 = dwt_cycles();
    while ((BOARD_CAN->MSR & CAN_MSR_SLAK) != 0U) {
        if (dwt_elapsed_us(t0) > CAN_TIMEOUT_US) {
            return -1;
        }
    }

    /* 3. 进初始化模式（MCR/BTR 只有在这里才写得进去） */
    if (can_enter_init() != 0) {
        return -1;
    }

    /* 自动离线恢复（bus-off 之后硬件自己回来）；NART=0 → 保留自动重发 */
    BOARD_CAN->MCR |= CAN_MCR_ABOM;

    /* 4. 位定时：正常模式，LBKM = 0 */
    BOARD_CAN->BTR = BOARD_CAN_BTR;

    /* 5. 过滤器 */
    can_filter_accept_all();

    /* 6. 退出初始化模式 */
    if (can_leave_init() != 0) {
        return -1;
    }

    return 0;
}

int bsp_can_tx(const bsp_can_frame_t *f)
{
    return can_send_raw(f);
}

int bsp_can_rx(bsp_can_frame_t *f)
{
    uint32_t rir;
    uint32_t rdtr;
    uint32_t rdlr;
    uint32_t rdhr;
    uint32_t i;

    if (f == NULL) {
        return -1;
    }
    if ((BOARD_CAN->RF0R & CAN_RF0R_FMP0) == 0U) {
        return 0;                              /* FIFO0 里没有帧 */
    }

    rir  = BOARD_CAN->sFIFOMailBox[0].RIR;
    rdtr = BOARD_CAN->sFIFOMailBox[0].RDTR;
    rdlr = BOARD_CAN->sFIFOMailBox[0].RDLR;
    rdhr = BOARD_CAN->sFIFOMailBox[0].RDHR;

    f->id  = (rir >> CAN_RI0R_STID_Pos) & CAN_STD_ID_MAX;
    f->len = (uint8_t)(rdtr & CAN_RDT0R_DLC);
    if (f->len > 8U) {
        f->len = 8U;
    }

    for (i = 0U; i < 4U; i++) {
        f->data[i]      = (uint8_t)(rdlr >> (8U * i));
        f->data[i + 4U] = (uint8_t)(rdhr >> (8U * i));
    }

    BOARD_CAN->RF0R = CAN_RF0R_RFOM0;          /* 释放邮箱，FIFO 里下一帧才能进来 */

    return 1;
}

int bsp_can_loopback_selftest(void)
{
    static const uint8_t pattern[4] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    bsp_can_frame_t tx;
    bsp_can_frame_t rx;
    uint32_t t0;
    uint32_t i;
    int      ret = -1;

    /* 进初始化模式才能改 BTR */
    g_can_dbg[0] = (uint32_t)can_enter_init();
    if (g_can_dbg[0] != 0U) {
        return -1;
    }
    BOARD_CAN->BTR = BOARD_CAN_BTR | CAN_BTR_LBKM;    /* 回环模式 */
    g_can_dbg[1] = (uint32_t)can_leave_init();
    if (g_can_dbg[1] != 0U) {
        return -1;
    }
    g_can_dbg[2] = (uint32_t)BOARD_CAN->BTR;

    tx.id  = 0x123U;
    tx.len = 4U;
    for (i = 0U; i < 8U; i++) {
        tx.data[i] = (i < 4U) ? pattern[i] : 0U;
    }

    g_can_dbg[3] = (uint32_t)can_send_raw(&tx);

    /* 发完立刻采样：MSR/RF0R/TSR/ESR —— 判断是不是"其实收到了但被漏掉" */
    g_can_dbg[8]  = (uint32_t)BOARD_CAN->MSR;
    g_can_dbg[9]  = (uint32_t)BOARD_CAN->RF0R;
    g_can_dbg[10] = (uint32_t)BOARD_CAN->TSR;
    g_can_dbg[11] = (uint32_t)BOARD_CAN->ESR;

    t0 = dwt_cycles();
    while (dwt_elapsed_us(t0) < 2000U) {          /* 回环是内部的，2 ms 内必然到 */
        g_can_dbg[4] = (uint32_t)bsp_can_rx(&rx);
        if (g_can_dbg[4] == 1U) {
            if ((rx.id == tx.id) && (rx.len == tx.len)) {
                ret = 0;
                for (i = 0U; i < tx.len; i++) {
                    if (rx.data[i] != tx.data[i]) {
                        ret = -1;
                    }
                }
            }
            break;
        }
    }

    g_can_dbg[5] = (uint32_t)BOARD_CAN->ESR;
    g_can_dbg[6] = (uint32_t)BOARD_CAN->TSR;
    g_can_dbg[7] = (uint32_t)BOARD_CAN->RF0R;

    /* 不管成败，都要切回正常模式 —— 否则后面发不到总线上 */
    if (can_enter_init() == 0) {
        BOARD_CAN->BTR = BOARD_CAN_BTR;
        (void)can_leave_init();
    }

    return ret;
}

uint32_t bsp_can_btr(void)
{
    return BOARD_CAN->BTR;
}

void bsp_can_error_counters(uint8_t *tec, uint8_t *rec)
{
    uint32_t esr = BOARD_CAN->ESR;

    if (tec != NULL) {
        *tec = (uint8_t)((esr & CAN_ESR_TEC) >> CAN_ESR_TEC_Pos);
    }
    if (rec != NULL) {
        *rec = (uint8_t)((esr & CAN_ESR_REC) >> CAN_ESR_REC_Pos);
    }
}
