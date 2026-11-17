/*
 * system_task.c - see system_task.h.
 */
#include "system_task.h"
#include "app_config.h"
#include "app_status.h"
#include "board_init.h"
#include "settings.h"
#include "smooth_move.h"
#include "status_led.h"
#include "usb_device_app.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

void system_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;)
    {
        uint32_t now = app_now_ms();

        board_led_set(app_status_led_on(app_status_get(), now));
        usb_device_poll(now);

        /* cmd.smooth survives power cycles: saved once it stops changing. */
        app_settings_t live = {.smoothMs = (uint16_t)smooth_move_get_duration()};
        (void)settings_poll(&live, now);
        app_status_settings(settings_saved()->smoothMs, settings_save_count(), settings_error_count());

        /* Last: a task that hung, or anything starving this task, stops the
         * feeding and WDOG1 resets the board. */
        watchdog_service(app_now_ms());

        vTaskDelayUntil(&last, pdMS_TO_TICKS(APP_SYSTEM_TICK_MS));
    }
}
