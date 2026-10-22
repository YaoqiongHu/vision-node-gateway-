#ifndef OS_TASK_H
#define OS_TASK_H

#include <stdint.h>

/* =============================================================================
 *  RTOS 薄封装：全工程只有 os/ 这一层允许出现 FreeRTOS 头。
 *  drv/ 与 lib/ 永远不知道 RTOS 存在（check-layers 会拦）。
 *
 *  S1b 先用动态任务（heap_4，堆在 CCM）。等任务结构稳定了再决定要不要
 *  全部换成 xTaskCreateStatic —— 那时缓冲区大小已知，静态更可预测。
 * ========================================================================== */

typedef void (*os_task_fn_t)(void *arg);

typedef struct {
    const char *name;
    os_task_fn_t fn;
    void *arg;
    uint32_t stack_words;   /* 单位：字（4 字节），不是字节 */
    uint32_t priority;      /* 0 = 最低（空闲），configMAX_PRIORITIES-1 = 最高 */
} os_task_cfg_t;

/** 建任务；返回 0 成功，-1 失败（堆不够）。 */
int os_task_create(const os_task_cfg_t *cfg);

/** 启动调度器，成功后不返回。 */
void os_start(void);

/** 内核堆剩余 / 历史最小剩余（字节） */
uint32_t os_heap_free(void);
uint32_t os_heap_min_free(void);

/** 本任务栈的历史最小剩余（字） */
uint32_t os_stack_high_water(void);

#endif /* OS_TASK_H */
