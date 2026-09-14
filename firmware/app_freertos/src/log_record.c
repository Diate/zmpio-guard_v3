#include "log_record.h"

#include "zmpio_crc32.h"

/* Forwards to the shared bit-serial CRC32 in common/zmpio_crc32.c, so ABI v3
 * (zmpio_abi_v3.h) and CPU0 use the same implementation as the on-disk ZLOG
 * checksum. */
uint32_t log_crc32(const void *data, size_t length)
{
    return zmpio_crc32(data, length);
}
