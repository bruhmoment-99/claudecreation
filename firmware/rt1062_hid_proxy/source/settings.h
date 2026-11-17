/*
 * settings.h - Settings kept across power cycles (currently cmd.smooth).
 *
 * Storage: two flash sectors at APP_SETTINGS_OFFSET used as a log. Each save
 * appends a 16-byte record (magic, sequence number, values, CRC) to the
 * active sector; only when a sector is full is the other one erased and
 * written. That is one erase per 256 saves, and a power cut during a save
 * leaves the previous record intact. On boot the valid record with the
 * highest sequence number wins; with none, the defaults apply.
 *
 * Saving is automatic: settings_poll() writes a changed value once it has
 * stayed the same for APP_SETTINGS_SAVE_DELAY_MS, so a script that sweeps
 * cmd.smooth does not write the flash on every command.
 */
#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "flash_if.h"

typedef struct
{
    uint16_t smoothMs; /* cmd.smooth(), 0 = off */
} app_settings_t;

#define SETTINGS_RECORD_SIZE   16U
#define SETTINGS_SLOTS         (APP_FLASH_SECTOR_SIZE / SETTINGS_RECORD_SIZE)

/* Load the newest valid record (or defaults). Returns true if one was found. */
bool settings_init(const flash_if_t *flash);

/* Values last loaded or saved. */
const app_settings_t *settings_saved(void);

/* Write now if different from what is saved. Returns false on flash error. */
bool settings_save(const app_settings_t *values);

/* Call periodically with the live values: saves after they have been stable
 * and different from the saved ones for APP_SETTINGS_SAVE_DELAY_MS.
 * Returns true when a save happened. */
bool settings_poll(const app_settings_t *live, uint32_t nowMs);

/* Saves done since boot, failures since boot (for cmd.status). */
uint32_t settings_save_count(void);
uint32_t settings_error_count(void);

#endif /* SETTINGS_H_ */
