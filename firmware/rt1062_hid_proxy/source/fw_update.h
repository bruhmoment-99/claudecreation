/*
 * fw_update.h - Firmware update over the command UART.
 *
 * Protocol (tools/fw_update.py implements the PC side):
 *
 *   PC:     cmd.update(<size>,<crc32>)\r\n        size of the .bin, zlib CRC-32
 *   board:  cmd.update(<size>,<crc32>)\r\n
 *           erasing\r\n                           staging area, ~45 ms per 4 KB
 *           ready\r\n
 *   PC:     up to APP_UPDATE_CHUNK (1024) raw bytes
 *   board:  ok <bytes received so far>\r\n       after every full chunk and
 *                                                 after the last byte
 *   ... repeat until <size> bytes are sent ...
 *   board:  installing\r\n                        CRC and image header checked,
 *                                                 then the board copies the image
 *                                                 over the running one and resets
 *     or:   error <reason>\r\n>>>                 back in command mode, nothing
 *                                                 installed
 *
 * Reasons: erase, program, timeout (no data for APP_UPDATE_TIMEOUT_MS), crc
 * (data did not match <crc32>), readback (flash content did not match),
 * image (no boot header / entry point outside the image).
 *
 * The image is received into a separate staging area, so a failed or
 * interrupted transfer never touches the running firmware. Only the final
 * copy (about 1 s for a 64 KB image) is critical: losing power during it
 * leaves an unbootable board that needs the ROM serial downloader or a
 * debug probe.
 *
 * This module is pure C (flash access through flash_if_t) so the protocol
 * runs in the PC unit tests; the copy itself is flash_ops_install().
 */
#ifndef FW_UPDATE_H_
#define FW_UPDATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_if.h"

typedef enum
{
    FWU_IDLE = 0, /* command mode                                    */
    FWU_ACTIVE,   /* receiving                                       */
    FWU_INSTALL,  /* complete and verified: call flash_ops_install() */
    FWU_ABORTED,  /* error reported, back in command mode            */
} fwu_state_t;

typedef void (*fwu_write_fn)(const char *data, size_t len);

/* Erase the staging area for size bytes and start receiving. yield (may be
 * NULL) runs between sector erases. Prints "erasing"; the caller prints
 * "ready" once it has discarded any input that arrived meanwhile. Returns
 * false (and has printed the error) if the erase failed. */
bool fw_update_begin(const flash_if_t *flash, uint32_t size, uint32_t crc, uint32_t nowMs, fwu_write_fn write,
                     void (*yield)(void));

/* Image bytes from the UART. */
fwu_state_t fw_update_feed(const uint8_t *data, size_t len, uint32_t nowMs);

/* Call regularly while active: aborts after APP_UPDATE_TIMEOUT_MS of silence. */
fwu_state_t fw_update_poll(uint32_t nowMs);

fwu_state_t fw_update_state(void);
uint32_t fw_update_size(void); /* of the image being / just received */

/* Checks the first bytes of an image for the i.MX RT boot header (FCB tag,
 * IVT, entry point inside the image). head must hold 0x1030 bytes. */
bool fw_update_image_ok(const uint8_t *head, uint32_t imageSize);
#define FW_UPDATE_HEAD_BYTES 0x1030U

#endif /* FW_UPDATE_H_ */
