/*
 * settings.c - see settings.h.
 */
#include "app_config.h"
#include "settings.h"
#include "crc32.h"

#include <stddef.h>
#include <string.h>

#define SETTINGS_MAGIC 0x31544553UL /* "SET1" */

typedef struct
{
    uint32_t magic;
    uint32_t seq;
    uint16_t smoothMs;
    uint16_t reserved; /* 0xFFFF */
    uint32_t crc;      /* CRC-32 of the 12 bytes above */
} record_t;

_Static_assert(sizeof(record_t) == SETTINGS_RECORD_SIZE, "record size");
_Static_assert((APP_FLASH_PAGE_SIZE % SETTINGS_RECORD_SIZE) == 0U, "records must not straddle pages");

static const flash_if_t *s_flash;
static app_settings_t s_saved;
static uint32_t s_seq;        /* sequence number of the newest record      */
static uint32_t s_sector;     /* 0 or 1: sector holding the newest record  */
static uint32_t s_nextSlot;   /* first free slot in s_sector               */
static uint32_t s_saves;
static uint32_t s_errors;

/* settings_poll() debounce */
static app_settings_t s_pending;
static bool s_havePending;
static uint32_t s_pendingSinceMs;

static uint32_t sector_off(uint32_t sector)
{
    return APP_SETTINGS_OFFSET + sector * APP_FLASH_SECTOR_SIZE;
}

static bool record_valid(const record_t *r)
{
    return (r->magic == SETTINGS_MAGIC) && (r->crc == crc32_update(0U, r, offsetof(record_t, crc)));
}

static bool record_blank(const record_t *r)
{
    const uint8_t *b = (const uint8_t *)r;
    for (uint32_t i = 0U; i < sizeof(*r); i++)
    {
        if (b[i] != 0xFFU)
        {
            return false;
        }
    }
    return true;
}

static void defaults(app_settings_t *v)
{
    memset(v, 0, sizeof(*v));
    v->smoothMs = 0U;
}

bool settings_init(const flash_if_t *flash)
{
    bool found = false;
    s_flash    = flash;
    defaults(&s_saved);
    s_seq      = 0U;
    s_sector   = 0U;
    s_nextSlot = 0U;
    s_havePending = false;

    uint32_t nextFree[2] = {0U, 0U};
    for (uint32_t sec = 0U; sec < 2U; sec++)
    {
        for (uint32_t i = 0U; i < SETTINGS_SLOTS; i++)
        {
            record_t r;
            if (!flash->read(sector_off(sec) + i * SETTINGS_RECORD_SIZE, &r, sizeof(r)))
            {
                return false;
            }
            if (!record_blank(&r))
            {
                nextFree[sec] = i + 1U; /* also skips half-written records */
            }
            if (record_valid(&r) && (!found || (r.seq > s_seq)))
            {
                found            = true;
                s_seq            = r.seq;
                s_sector         = sec;
                s_saved.smoothMs = (r.smoothMs <= APP_SMOOTH_MAX_MS) ? r.smoothMs : APP_SMOOTH_MAX_MS;
            }
        }
    }
    s_nextSlot = nextFree[s_sector];
    return found;
}

const app_settings_t *settings_saved(void)
{
    return &s_saved;
}

bool settings_save(const app_settings_t *v)
{
    if (s_flash == NULL)
    {
        return false;
    }
    if (memcmp(v, &s_saved, sizeof(*v)) == 0)
    {
        return true;
    }

    record_t r;
    memset(&r, 0xFF, sizeof(r));
    r.magic    = SETTINGS_MAGIC;
    r.seq      = s_seq + 1U;
    r.smoothMs = v->smoothMs;
    r.crc      = crc32_update(0U, &r, offsetof(record_t, crc));

    uint32_t sector = s_sector;
    uint32_t slot   = s_nextSlot;
    if (slot >= SETTINGS_SLOTS)
    {
        /* Active sector full: start over in the other one. The old sector
         * keeps the previous value until this record is written. */
        sector = s_sector ^ 1U;
        slot   = 0U;
        if (!s_flash->erase_sector(sector_off(sector)))
        {
            s_errors++;
            return false;
        }
    }

    uint32_t off = sector_off(sector) + slot * SETTINGS_RECORD_SIZE;
    record_t check;
    if (!s_flash->program(off, &r, sizeof(r)) || !s_flash->read(off, &check, sizeof(check)) ||
        (memcmp(&check, &r, sizeof(r)) != 0))
    {
        /* Skip the bad slot next time; keep the old values as "saved". */
        s_sector   = sector;
        s_nextSlot = slot + 1U;
        s_errors++;
        return false;
    }

    s_sector   = sector;
    s_nextSlot = slot + 1U;
    s_seq      = r.seq;
    s_saved    = *v;
    s_saves++;
    return true;
}

bool settings_poll(const app_settings_t *live, uint32_t nowMs)
{
    if (memcmp(live, &s_saved, sizeof(*live)) == 0)
    {
        s_havePending = false;
        return false;
    }
    if (!s_havePending || (memcmp(live, &s_pending, sizeof(*live)) != 0))
    {
        s_pending        = *live; /* changed (again): restart the delay */
        s_havePending    = true;
        s_pendingSinceMs = nowMs;
        return false;
    }
    if ((uint32_t)(nowMs - s_pendingSinceMs) < APP_SETTINGS_SAVE_DELAY_MS)
    {
        return false;
    }
    if (settings_save(live))
    {
        s_havePending = false;
        return true;
    }
    s_pendingSinceMs = nowMs; /* failed: try again after another delay */
    return false;
}

uint32_t settings_save_count(void)
{
    return s_saves;
}

uint32_t settings_error_count(void)
{
    return s_errors;
}
