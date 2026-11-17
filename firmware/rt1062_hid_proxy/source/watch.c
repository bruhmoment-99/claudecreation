/*
 * watch.c - see watch.h.
 */
#include "watch.h"
#include "app_config.h"

#include <stddef.h>

static volatile uint32_t s_lastMs[WATCH_COUNT];
static volatile bool s_parked[WATCH_COUNT];

void watch_init(uint32_t nowMs)
{
    for (uint32_t i = 0U; i < (uint32_t)WATCH_COUNT; i++)
    {
        s_lastMs[i] = nowMs;
        s_parked[i] = false;
    }
}

void watch_checkin(watch_id_t id, uint32_t nowMs)
{
    s_lastMs[id] = nowMs;
    s_parked[id] = false;
}

void watch_park(watch_id_t id)
{
    s_parked[id] = true;
}

bool watch_all_alive(uint32_t nowMs, watch_id_t *late)
{
    for (uint32_t i = 0U; i < (uint32_t)WATCH_COUNT; i++)
    {
        if (!s_parked[i] && ((uint32_t)(nowMs - s_lastMs[i]) > APP_WDOG_TASK_DEADLINE_MS))
        {
            if (late != NULL)
            {
                *late = (watch_id_t)i;
            }
            return false;
        }
    }
    return true;
}

const char *watch_name(watch_id_t id)
{
    static const char *const names[WATCH_COUNT] = {"uart_cmd", "usbh_app"};
    return ((uint32_t)id < (uint32_t)WATCH_COUNT) ? names[id] : "?";
}
