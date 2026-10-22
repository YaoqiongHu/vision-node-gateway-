#ifndef OS_HOOKS_H
#define OS_HOOKS_H

#include <stdint.h>

/* 钩子/断言留下现场：出事后用 openocd `mdw &g_assert_line` 直接读，
 * 比在串口里猜要快。定义在 os/os_task.c。 */
extern volatile const char *g_assert_file;
extern volatile int g_assert_line;
extern volatile uint32_t g_assert_count;
extern volatile uint32_t g_malloc_failed_count;
extern volatile uint32_t g_stack_overflow_count;

#endif /* OS_HOOKS_H */
