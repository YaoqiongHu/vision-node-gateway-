#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>

/* =============================================================================
 *  bxCAN1 —— 元数据总线，500 kbps，标准帧（11 位 ID）
 *
 *  节点侧只需要干三件事：初始化、发一帧、收一帧。
 *  收发都走**轮询**，不开中断：
 *    - S2 只验"这条通路通不通"，轮询最简单、出错时一眼能看出卡在哪；
 *    - 真上量（S5 之后）再改中断或 DMA，和 UART 的演进路线保持一致。
 *
 *  引脚见 board_config.h：PD0 = RX、PD1 = TX（AF9）。
 * ========================================================================== */

typedef struct {
    uint32_t id;        /* 11 位标准 ID，**未左移**，直接给数值（0x000 ~ 0x7FF） */
    uint8_t  data[8];
    uint8_t  len;       /* 0 ~ 8 */
} bsp_can_frame_t;

/** 初始化：开时钟、配 PD0/PD1、写位定时 500 kbps、过滤器全通过、进正常模式。
 *  返回 0 = 成功，-1 = 等 INAK/TME 超时（基本只有没接电源才会这样）。 */
int bsp_can_init(void);

/** 发送一帧，等这一帧真正发出去（RQCP0/TXOK0）再返回。
 *  返回 0 = 成功，-1 = 超时或总线错误（比如没接收方 ACK）。 */
int bsp_can_tx(const bsp_can_frame_t *f);

/** 非阻塞收一帧。返回 1 = 收到，0 = 没有数据，-1 = 参数错误。 */
int bsp_can_rx(bsp_can_frame_t *f);

/** 内部回环自测：不需要收发器、不需要对端，控制器自己发自己收。
 *  这是 S2 验收 CAN 的第一步 —— 通了才说明位定时和寄存器配置是对的。
 *  自测结束会自动切回正常模式。返回 0 = 通过。 */
int bsp_can_loopback_selftest(void);

/** 直接把 BTR 读回来，用于打印核对（自测时和 board_config 里的常量对照）。 */
uint32_t bsp_can_btr(void);

/** 错误计数器：TEC（发送）与 REC（接收），都在 CANx->ESR 里。 */
void bsp_can_error_counters(uint8_t *tec, uint8_t *rec);

#endif /* BSP_CAN_H */
