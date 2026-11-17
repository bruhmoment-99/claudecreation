/*
 * main.c - i.MX RT1062 USB HID proxy (FreeRTOS).
 *
 *   Physical mouse --USB2 host (HS/FS)--> [ RT1062 ] --USB1 device (HS, 8 kHz)--> PC
 *                                             ^
 *   Control PC --CH343P--6 Mbaud LPUART3------+  "cmd.move(10,-5)\r\n" ...
 *
 * Execution contexts (highest first)
 * ----------------------------------
 *   USB_OTG1_IRQHandler   NVIC 2  USB1 device: ch9 + HID callbacks, re-prime IN EP
 *   USB_OTG2_IRQHandler   NVIC 3  USB2 host: signals the host stack task
 *   PIT_IRQHandler        NVIC 4  8 kHz smooth-move tick (only while enabled)
 *   LPUART3_IRQHandler    NVIC 5  idle-line -> wakes UART task (RX itself is DMA)
 *   DMA0_DMA16_IRQHandler NVIC 5  RX ring wrapped (overflow detection)
 *   usbh_stack            prio 5  USB_HostEhciTaskFunction(); mouse IN callback ->
 *                                 parse -> merge -> USB_DeviceHidSend()
 *   usbh_app              prio 4  VBUS sequencing, enumeration, fault supervision
 *   uart_cmd              prio 3  command parser, firmware update receiver
 *   system                prio 1  LED, suspend/wake-up, settings, watchdog
 *
 * The merge path never blocks and never waits for a task switch: the
 * physical report is decoded and the merged report is queued on USB1 inside
 * the host completion callback itself.
 *
 * FreeRTOSConfig.h notes:
 *   configTICK_RATE_HZ                        1000
 *   configMAX_PRIORITIES                      >= 6
 *   configUSE_PREEMPTION                      1
 *   configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  2  (see app_config.h NVIC prios)
 *   configTOTAL_HEAP_SIZE                     >= 24 KB (USB stacks use OSA heap)
 */
#include "app_config.h"
#include "app_status.h"
#include "board_init.h"
#include "flash_ops.h"
#include "hid_merge.h"
#include "settings.h"
#include "smooth_move.h"
#include "status_led.h"
#include "system_task.h"
#include "uart_parser.h"
#include "usb_device_app.h"
#include "usb_host_app.h"
#include "watch.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

static void app_init_task(void *arg)
{
    (void)arg;

    /*
     * Bring-up runs inside a task (not before vTaskStartScheduler) because the
     * USB stacks create OSA/FreeRTOS objects and may take mutexes during init,
     * as in the SDK's *_freertos examples.
     */
    app_status_init();
    watchdog_read_reset_cause(); /* before anything can overwrite the note */

    /* From here on a hung task or a crash resets the board (watch.h). */
    watch_init(app_now_ms());
    watchdog_start();

    hid_merge_init();
    smooth_move_init(); /* OFF by default ... */

    /* ... unless a saved cmd.smooth value says otherwise. */
    (void)flash_ops_init();
    (void)settings_init(flash_ops_if());
    smooth_move_set_duration(settings_saved()->smoothMs);

    /* USB1 to the PC: comes up first so the PC sees the mouse immediately,
     * even before a physical mouse is attached (injection-only operation). */
    usb_device_app_init();

    /* PIT tick for smoothed moves (only runs while cmd.smooth(ms) > 0). */
    smooth_move_hw_init();

    /* LPUART3 + eDMA ring. */
    uart_parser_hw_init();

    (void)xTaskCreate(system_task, "system", APP_STACK_SYSTEM, NULL, APP_PRIO_SYSTEM, NULL);
    (void)xTaskCreate(uart_parser_task, "uart_cmd", APP_STACK_UART_PARSER, NULL, APP_PRIO_UART_PARSER, NULL);

    /* USB2 host: this task does VBUS LOW->HIGH, then USB_HostInit(), then
     * spawns the high-priority host stack task. */
    (void)xTaskCreate(usb_host_app_task, "usbh_app", APP_STACK_USB_HOST_APP, NULL, APP_PRIO_USB_HOST_APP, NULL);

    vTaskDelete(NULL);
}

int main(void)
{
    /* MPU/caches, 600 MHz clocks, UART clock root, pin mux, VBUS EN = LOW. */
    board_init();

    (void)xTaskCreate(app_init_task, "init", APP_STACK_INIT, NULL, APP_PRIO_INIT, NULL);
    vTaskStartScheduler();

    /* Only reached if the FreeRTOS heap is too small for the idle task. */
    app_fatal(APP_FATAL_SCHEDULER);
}
