/*
 * crc32.h - CRC-32 (IEEE 802.3 / zlib: reflected, polynomial 0xEDB88320,
 * initial value and final XOR 0xFFFFFFFF). Same result as Python's
 * zlib.crc32() and binascii.crc32().
 */
#ifndef CRC32_H_
#define CRC32_H_

#include <stddef.h>
#include <stdint.h>

/* Start with crc = 0; feed data in any number of pieces. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif /* CRC32_H_ */
