/*
 * status_led.h - The fatal-error handler and the millisecond clock.
 *
 * The LED shows the state from app_status.h (see app_status_led_on() for
 * the patterns; system_task.c refreshes it). app_fatal() is the single
 * place every unrecoverable error ends up: FreeRTOS asserts, stack
 * overflow, heap exhaustion, HardFault and failed bring-up steps. It stops
 * everything and flickers the LED at 20 Hz, so a crash is visible without a
 * debugger. The code is kept in g_appFatalCode for inspection with a probe,
 * and in a note that survives the reset: once the watchdog runs, it resets
 * the board after APP_WDOG_TIMEOUT_MS and cmd.status() then shows the code
 * on its "reset:" line.
 */
#ifndef STATUS_LED_H_
#define STATUS_LED_H_

#include <stdint.h>

/* app_fatal() codes. Values below 0x10000 are FreeRTOS configASSERT line
 * numbers (see FreeRTOSConfig.h). */
#define APP_FATAL_HARDFAULT       0xFA000001U
#define APP_FATAL_STACK_OVERFLOW  0xFA000002U
#define APP_FATAL_MALLOC          0xFA000003U
#define APP_FATAL_USB_DEVICE_INIT 0xFA000004U
#define APP_FATAL_UART_INIT       0xFA000005U
#define APP_FATAL_SCHEDULER       0xFA000006U
#define APP_FATAL_ASSERT          0xFA000007U /* SDK assert(); see g_appFatalFile/Line */

extern volatile uint32_t g_appFatalCode;
extern const char *volatile g_appFatalFile; /* set by a failed assert() */
extern volatile int g_appFatalLine;

/* Milliseconds since the scheduler started (wraps after ~49 days). */
uint32_t app_now_ms(void);

/* Never returns. Safe from any context, including HardFault. */
void app_fatal(uint32_t code) __attribute__((noreturn));

#endif /* STATUS_LED_H_ */
