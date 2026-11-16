#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bsp_can.h"
#include "bsp_uart.h"
#include "os_task.h"
#include "app_tasks.h"

/* S1b 只有两个任务：一个闪灯，一个打印。
 * 优先级：报告任务略高（打印是阻塞 IO，长了会拖住闪灯）。 */

extern volatile uint32_t g_led0_toggles;
extern volatile uint32_t g_can_tx_ok;
extern volatile uint32_t g_can_tx_fail;

volatile uint32_t g_report_count;

static void task_led(void *arg)
{
    uint32_t tick = 0U;

    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));

        tick++;
        board_led_toggle(BOARD_LED1);              /* PF10：10 Hz 翻转 */

        if ((tick % 5U) == 0U) {
            board_led_toggle(BOARD_LED0);          /* PF9：1 Hz 闪烁 */
            g_led0_toggles++;
        }
    }
}

static void task_report(void *arg)
{
    bsp_can_frame_t f;
    int can_ok;

    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        g_report_count++;

        /* S2 第三条通路：每秒往 500 kbps 总线上送一帧，树莓派侧 candump 应看到 0x180。
         * S5 之后这里换成真正的状态帧（0x180 会定死报文布局）。
         * 注意：对端没上线时这一句会阻塞到 20 ms 超时再返回 -1，属于预期。 */
        f.id      = 0x180U;
        f.len     = 4U;
        f.data[0] = (uint8_t)(g_report_count & 0xFFU);
        f.data[1] = (uint8_t)((g_report_count >> 8) & 0xFFU);
        f.data[2] = 0U;
        f.data[3] = 0U;
        can_ok    = bsp_can_tx(&f);
        if (can_ok == 0) {
            g_can_tx_ok++;
        } else {
            g_can_tx_fail++;
        }

        printf("[%5lu s] led0=%lu  can=%s  heap free=%lu min=%lu (+%lu B)  stack_hwm=%lu\r\n",
               (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
               (unsigned long)g_led0_toggles,
               (can_ok == 0) ? "ok" : "--",
               (unsigned long)os_heap_free(),
               (unsigned long)os_heap_min_free(),
               (unsigned long)(configTOTAL_HEAP_SIZE - os_heap_min_free()),
               (unsigned long)os_stack_high_water());
    }
}

void app_tasks_start(void)
{
    static const os_task_cfg_t tasks[] = {
        { "led",    task_led,    NULL, 128U, 2U },
        { "report", task_report, NULL, 256U, 3U },
    };
    uint32_t i;

    for (i = 0U; i < (sizeof(tasks) / sizeof(tasks[0])); i++) {
        if (os_task_create(&tasks[i]) != 0) {
            printf("[os] create task '%s' failed: only %lu B left in kernel heap\r\n",
                   tasks[i].name, (unsigned long)os_heap_free());
            for (;;) {
                /* 停在这里，别带着残缺的任务集启动调度器 */
            }
        }
    }

    printf("[os] %lu task(s) created, starting scheduler (tick=%lu Hz)\r\n",
           (unsigned long)(sizeof(tasks) / sizeof(tasks[0])),
           (unsigned long)configTICK_RATE_HZ);
    bsp_uart_flush();

    os_start();

    /* 走到这里说明内核堆连空闲任务都建不出来 */
    for (;;) {
    }
}
