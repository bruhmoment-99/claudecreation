/*
 * system_task.h - Lowest-priority housekeeping task, every
 * APP_SYSTEM_TICK_MS: status LED, USB suspend/resume and remote wake-up,
 * saving changed settings, and feeding the watchdog (only while the
 * watched tasks are alive; see watch.h).
 */
#ifndef SYSTEM_TASK_H_
#define SYSTEM_TASK_H_

void system_task(void *arg);

#endif /* SYSTEM_TASK_H_ */
