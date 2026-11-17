/*
 * fw_update.c - see fw_update.h.
 */
#include "fw_update.h"
#include "app_config.h"
#include "crc32.h"

#include <string.h>

typedef struct
{
    const flash_if_t *flash;
    fwu_write_fn write;
    fwu_state_t state;
    uint32_t size;
    uint32_t expectedCrc;
    uint32_t received;
    uint32_t crc;
    uint32_t lastRxMs;
    uint32_t pageFill;
    uint8_t page[APP_FLASH_PAGE_SIZE];
} fwu_t;

static fwu_t s_u;

static void say(const char *s)
{
    s_u.write(s, strlen(s));
}

static void say_uint(uint32_t u)
{
    char buf[10];
    size_t i = sizeof(buf);
    do
    {
        buf[--i] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);
    s_u.write(&buf[i], sizeof(buf) - i);
}

static fwu_state_t fail(const char *reason)
{
    say("error ");
    say(reason);
    say("\r\n>>> ");
    s_u.state = FWU_ABORTED;
    return s_u.state;
}

bool fw_update_begin(const flash_if_t *flash, uint32_t size, uint32_t crc, uint32_t nowMs, fwu_write_fn write,
                     void (*yield)(void))
{
    memset(&s_u, 0, sizeof(s_u));
    s_u.flash       = flash;
    s_u.write       = write;
    s_u.size        = size;
    s_u.expectedCrc = crc;
    s_u.lastRxMs    = nowMs;

    say("erasing\r\n");
    for (uint32_t off = 0U; off < size; off += APP_FLASH_SECTOR_SIZE)
    {
        if (!flash->erase_sector(APP_STAGING_OFFSET + off))
        {
            (void)fail("erase");
            return false;
        }
        if (yield != NULL)
        {
            yield();
        }
    }
    s_u.state = FWU_ACTIVE;
    return true;
}

static bool flush_page(void)
{
    if (s_u.pageFill == 0U)
    {
        return true;
    }
    uint32_t off = APP_STAGING_OFFSET + s_u.received - s_u.pageFill;
    bool ok      = s_u.flash->program(off, s_u.page, s_u.pageFill);
    s_u.pageFill = 0U;
    return ok;
}

/* All bytes are in: check everything before anything is installed. */
static fwu_state_t finish(void)
{
    if (s_u.crc != s_u.expectedCrc)
    {
        return fail("crc");
    }

    /* Read the staging area back: proves the flash holds what was sent. */
    uint8_t buf[APP_FLASH_PAGE_SIZE];
    uint32_t crc = 0U;
    for (uint32_t off = 0U; off < s_u.size; off += sizeof(buf))
    {
        uint32_t n = ((s_u.size - off) < sizeof(buf)) ? (s_u.size - off) : (uint32_t)sizeof(buf);
        if (!s_u.flash->read(APP_STAGING_OFFSET + off, buf, n))
        {
            return fail("readback");
        }
        crc = crc32_update(crc, buf, n);
    }
    if (crc != s_u.expectedCrc)
    {
        return fail("readback");
    }

    static uint8_t head[FW_UPDATE_HEAD_BYTES];
    if ((s_u.size < FW_UPDATE_HEAD_BYTES) || !s_u.flash->read(APP_STAGING_OFFSET, head, sizeof(head)) ||
        !fw_update_image_ok(head, s_u.size))
    {
        return fail("image");
    }

    say("installing\r\n");
    s_u.state = FWU_INSTALL;
    return s_u.state;
}

fwu_state_t fw_update_feed(const uint8_t *data, size_t len, uint32_t nowMs)
{
    if (s_u.state != FWU_ACTIVE)
    {
        return s_u.state;
    }
    s_u.lastRxMs = nowMs;

    for (size_t i = 0U; (i < len) && (s_u.received < s_u.size); i++)
    {
        s_u.page[s_u.pageFill++] = data[i];
        s_u.crc                  = crc32_update(s_u.crc, &data[i], 1U);
        s_u.received++;

        bool pageFull = (s_u.pageFill == APP_FLASH_PAGE_SIZE);
        bool last     = (s_u.received == s_u.size);
        if ((pageFull || last) && !flush_page())
        {
            return fail("program");
        }
        if (((s_u.received % APP_UPDATE_CHUNK) == 0U) || last)
        {
            say("ok ");
            say_uint(s_u.received);
            say("\r\n");
        }
        if (last)
        {
            return finish();
        }
    }
    /* Bytes past <size> (none in a well-behaved transfer) are ignored. */
    return s_u.state;
}

fwu_state_t fw_update_poll(uint32_t nowMs)
{
    if ((s_u.state == FWU_ACTIVE) && ((uint32_t)(nowMs - s_u.lastRxMs) >= APP_UPDATE_TIMEOUT_MS))
    {
        return fail("timeout");
    }
    return s_u.state;
}

fwu_state_t fw_update_state(void)
{
    return s_u.state;
}

uint32_t fw_update_size(void)
{
    return s_u.size;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fw_update_image_ok(const uint8_t *head, uint32_t imageSize)
{
    /* FlexSPI configuration block: tag "FCFB" at offset 0. */
    if (rd32(&head[0]) != 0x42464346UL)
    {
        return false;
    }
    /* Image vector table at 0x1000: header tag 0xD1, then the entry point
     * (address of the vector table), then (at +0x20) boot data whose first
     * word is the image start address. */
    const uint8_t *ivt = &head[0x1000];
    uint32_t entry     = rd32(&ivt[4]);
    uint32_t start     = rd32(&ivt[0x20]);
    if ((ivt[0] != 0xD1U) || (start != APP_FLASH_BASE))
    {
        return false;
    }
    if ((entry < (APP_FLASH_BASE + 0x2000U)) || (entry >= (APP_FLASH_BASE + imageSize)))
    {
        return false;
    }
    return imageSize <= APP_SLOT_SIZE;
}
