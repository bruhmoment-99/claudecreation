/*
 * flash_if.h - The few flash operations the settings store and the firmware
 * updater need. flash_ops.c implements them on the QSPI NOR through the
 * boot ROM's FlexSPI driver; the unit tests use a RAM-backed fake.
 *
 * Offsets are from the start of the flash (0 = APP_FLASH_BASE).
 */
#ifndef FLASH_IF_H_
#define FLASH_IF_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    /* Copy len bytes at off into dst. */
    bool (*read)(uint32_t off, void *dst, uint32_t len);
    /* Erase the APP_FLASH_SECTOR_SIZE sector starting at off (aligned). */
    bool (*erase_sector)(uint32_t off);
    /* Program len bytes at off. The range must lie inside one
     * APP_FLASH_PAGE_SIZE page and be erased (NOR: bits only go 1 -> 0). */
    bool (*program)(uint32_t off, const void *src, uint32_t len);
} flash_if_t;

#endif /* FLASH_IF_H_ */
