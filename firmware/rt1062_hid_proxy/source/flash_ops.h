/*
 * flash_ops.h - QSPI NOR erase/program/read through the boot ROM's FlexSPI
 * driver, while the firmware itself runs from the same flash (XIP).
 *
 * Every ROM call runs with interrupts disabled and the instruction cache
 * off, because nothing may be fetched from the flash while it is busy
 * erasing or programming (ISRs live in flash too). One page program takes
 * well under 1 ms; one sector erase about 45 ms. Calls from different tasks
 * are serialised by a mutex.
 */
#ifndef FLASH_OPS_H_
#define FLASH_OPS_H_

#include <stdbool.h>
#include <stdint.h>

#include "flash_if.h"

/* Initialise the ROM driver with the board's own boot header (FCB). Call
 * once from a task before using flash_ops_if(). Returns false on failure. */
bool flash_ops_init(void);

/* The flash_if_t operations (offsets from APP_FLASH_BASE). */
const flash_if_t *flash_ops_if(void);

/* Copy size bytes from the staging area over the running image and reset.
 * Runs from ITCM with interrupts off, feeds the watchdog, never returns. */
void flash_ops_install(uint32_t size) __attribute__((noreturn));

#endif /* FLASH_OPS_H_ */
