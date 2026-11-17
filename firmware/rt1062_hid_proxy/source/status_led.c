/*
 * status_led.c - Fatal error handling and the millisecond clock (see
 * status_led.h). The LED itself is refreshed by system_task.c.
 */
#include "status_led.h"
#include "app_config.h"
#include "app_status.h"
#include "board_init.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_common.h"

volatile uint32_t g_appFatalCode;
const char *volatile g_appFatalFile;
volatile int g_appFatalLine;

uint32_t app_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void app_fatal(uint32_t code)
{
    __disable_irq();
    g_appFatalCode = code;
    watchdog_note_fatal(code);
    /* Nothing feeds the watchdog any more: if it runs, the board resets
     * after APP_WDOG_TIMEOUT_MS; if not (early bring-up), this flickers
     * forever. */

    /* The scheduler and SysTick are gone; time the flicker with a busy loop.
     * It is approximate, which is fine for a blink. */
    for (;;)
    {
        board_led_set(true);
        SDK_DelayAtLeastUs(25000U, SystemCoreClock);
        board_led_set(false);
        SDK_DelayAtLeastUs(25000U, SystemCoreClock);
    }
}

/* ---- FreeRTOS hooks (enabled in FreeRTOSConfig.h) ------------------------ */

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name; /* visible in a debugger via the caller's frame */
    app_fatal(APP_FATAL_STACK_OVERFLOW);
}

void vApplicationMallocFailedHook(void)
{
    app_fatal(APP_FATAL_MALLOC);
}

/* ---- Fault handler (overrides the weak one in startup_MIMXRT1062.S) ------ */

void HardFault_Handler(void)
{
    app_fatal(APP_FATAL_HARDFAULT);
}

/* ---- assert() (Debug builds; replaces newlib's, which would print) ------- */

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    (void)func;
    (void)expr;
    g_appFatalFile = file;
    g_appFatalLine = line;
    app_fatal(APP_FATAL_ASSERT);
}
