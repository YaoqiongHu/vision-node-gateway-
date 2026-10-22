#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* =============================================================================
 *  FreeRTOSConfig.h —— 中断优先级、堆、钩子全在这里
 *
 *  板子：STM32F407ZGT6 @ 168 MHz，Cortex-M4F，4 位优先级（16 级）
 *
 *  中断优先级纪律（计划书 §5，违反必炸）：
 *    - 内核用最低优先级 15（configKERNEL_INTERRUPT_PRIORITY）
 *    - 所有会调 FromISR API 的中断，NVIC 优先级数值必须 ≥ 5
 *      （即 configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4）
 *    - 优先级 0..4 的中断不许调用任何 FreeRTOS API（它们不会被临界区屏蔽）
 * ========================================================================== */

extern uint32_t SystemCoreClock;

/* ---------------- 调度 ---------------- */
#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     7
#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)
#define configMAX_TASK_NAME_LEN                  16
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  0
#define configSTACK_DEPTH_TYPE                   uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE         size_t

/* ---------------- 同步原语 ---------------- */
#define configUSE_TASK_NOTIFICATIONS             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_QUEUE_SETS                     0
#define configQUEUE_REGISTRY_SIZE                8

/* ---------------- 内存 ----------------
 * 堆放在 CCM（DMA 到不了），显式告诉内核"堆由应用自己定义"，
 * 这样 ucHeap 就能用段属性落到 .ccmbss。 */
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configAPPLICATION_ALLOCATED_HEAP         1
#define configTOTAL_HEAP_SIZE                    ((size_t)(24 * 1024))

/* ---------------- 钩子与检查 ---------------- */
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0

/* ---------------- 统计 / 协程 / 软件定时器 ---------------- */
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configUSE_CO_ROUTINES                    0

#define configUSE_TIMERS                         0
#define configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             (configMINIMAL_STACK_SIZE * 2)

/* ---------------- 可选 API ---------------- */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xResumeFromISR                   1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_xTaskGetHandle                   1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_uxTaskGetStackHighWaterMark2     1
#define INCLUDE_xQueueGetMutexHolder             1
#define INCLUDE_xSemaphoreGetMutexHolder         1
#define INCLUDE_xTimerPendFunctionCall           0
#define INCLUDE_xTaskAbortDelay                  0

/* ---------------- Cortex-M4 中断优先级 ---------------- */
#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS                          __NVIC_PRIO_BITS
#else
#define configPRIO_BITS                          4
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY       15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ---------------- 异常处理函数对接 ----------------
 * 启动文件里这三个向量是弱符号，port.c 里是强符号，链接时自动覆盖。 */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* ---------------- 断言 ---------------- */
extern void os_assert_failed(const char *file, int line);

#define configASSERT(x)                        \
    do {                                       \
        if ((x) == 0) {                        \
            os_assert_failed(__FILE__, __LINE__); \
        }                                      \
    } while (0)

#endif /* FREERTOS_CONFIG_H */
