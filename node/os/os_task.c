#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "os_task.h"
#include "os_hooks.h"

/* ---- 内核堆：显式落在 CCM，DMA 永远拿不到它 ----
 * configAPPLICATION_ALLOCATED_HEAP=1 时 heap_4.c 会用 extern 引用这个名字。 */
uint8_t ucHeap[configTOTAL_HEAP_SIZE]
    __attribute__((section(".ccmbss"), aligned(8), used));

int os_task_create(const os_task_cfg_t *cfg)
{
    BaseType_t rc;

    if ((cfg == NULL) || (cfg->fn == NULL)) {
        return -1;
    }

    rc = xTaskCreate(cfg->fn, cfg->name,
                     (configSTACK_DEPTH_TYPE)cfg->stack_words,
                     cfg->arg,
                     (UBaseType_t)cfg->priority,
                     NULL);

    return (rc == pdPASS) ? 0 : -1;
}

void os_start(void)
{
    vTaskStartScheduler();

    /* 能走到这里说明内核堆已经不够建空闲任务了 */
    os_assert_failed(__FILE__, __LINE__);
}

uint32_t os_heap_free(void)
{
    return (uint32_t)xPortGetFreeHeapSize();
}

uint32_t os_heap_min_free(void)
{
    return (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

uint32_t os_stack_high_water(void)
{
    return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

/* ---------------------------------------------------------------------------
 * 钩子与断言：全部停在死循环里，调试器 attach 上去就能看到 pc 落在哪个函数，
 * 同时配套的全局变量留下现场（别忘了 os_hooks.h 里的 extern 声明）。
 * ------------------------------------------------------------------------- */

volatile const char *g_assert_file;
volatile int g_assert_line;
volatile uint32_t g_assert_count;
volatile uint32_t g_malloc_failed_count;
volatile uint32_t g_stack_overflow_count;

void os_assert_failed(const char *file, int line)
{
    g_assert_file = file;
    g_assert_line = line;
    g_assert_count++;

    taskDISABLE_INTERRUPTS();
    for (;;) {
        /* 停机，等调试器 */
    }
}

void vApplicationMallocFailedHook(void)
{
    g_malloc_failed_count++;

    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;

    g_stack_overflow_count++;

    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

/* ---------------------------------------------------------------------------
 *  静态分配：空闲任务的内存由我们给（configSUPPORT_STATIC_ALLOCATION=1）。
 *  放在 CCM，和内核堆同一个道理 —— 任务栈永远不该被 DMA 碰到。
 * ------------------------------------------------------------------------- */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE]
    __attribute__((section(".ccmbss"), aligned(8), used));

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   configSTACK_DEPTH_TYPE *puxIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *puxIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
