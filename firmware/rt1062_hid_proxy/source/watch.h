/*
 * watch.h - Which tasks the watchdog vouches for.
 *
 * The system task (lowest priority) feeds the hardware watchdog only while
 * every watched task has checked in within APP_WDOG_TASK_DEADLINE_MS. So the
 * board resets if:
 *   - a watched task stops looping (stuck in a USB call, deadlock), or
 *   - anything of higher priority spins and starves the system task
 *     (a hung ISR or a busy-looping USB task), or
 *   - app_fatal() halts the CPU.
 *
 * A task that is about to block for a long, known reason (cmd.sleep) parks
 * itself, which exempts it until its next check-in.
 *
 * Pure C: the timing logic is unit tested; watchdog.c does the hardware.
 */
#ifndef WATCH_H_
#define WATCH_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    WATCH_UART = 0, /* uart_cmd: command parser / update receiver  */
    WATCH_USB_HOST, /* usbh_app: enumeration and VBUS supervision  */
    WATCH_COUNT,
} watch_id_t;

void watch_init(uint32_t nowMs);
void watch_checkin(watch_id_t id, uint32_t nowMs); /* also un-parks          */
void watch_park(watch_id_t id);                     /* exempt until checkin  */

/* True if every unparked task checked in within the deadline. Otherwise
 * false, and *late (if not NULL) names the first late task. */
bool watch_all_alive(uint32_t nowMs, watch_id_t *late);

const char *watch_name(watch_id_t id);

#endif /* WATCH_H_ */
