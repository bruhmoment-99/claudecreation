/*
 * watchdog.h - WDOG1 supervision and "why did the board restart".
 *
 * watchdog_start() arms WDOG1 with APP_WDOG_TIMEOUT_MS. The system task
 * calls watchdog_service() every APP_SYSTEM_TICK_MS; it feeds WDOG1 only
 * while watch_all_alive() (watch.h) says every watched task is responsive.
 * When a task stops responding its name is written to a RAM area that
 * survives the reset, and so is the code passed to app_fatal(), so
 * cmd.status() can say after the restart what happened.
 *
 * WDOG1 is paused while a debugger halts the core.
 */
#ifndef WATCHDOG_H_
#define WATCHDOG_H_

#include <stdint.h>

/* Read and clear the reset cause registers and the note left before the
 * reset; publish them with app_status_reset(). Call early in main(). */
void watchdog_read_reset_cause(void);

/* Arm WDOG1 (no-op if APP_WATCHDOG_ENABLE is 0). */
void watchdog_start(void);

/* System task: feed if all watched tasks are alive. */
void watchdog_service(uint32_t nowMs);

/* Feed unconditionally (long operations in the system/uart task). */
void watchdog_feed(void);

/* app_fatal(): remember the code for the next boot. Safe with IRQs off. */
void watchdog_note_fatal(uint32_t code);

/* True once watchdog_start() has armed WDOG1. */
int watchdog_running(void);

#endif /* WATCHDOG_H_ */
