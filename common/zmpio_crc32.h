#ifndef ZMPIO_CRC32_H
#define ZMPIO_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-32/ISO-HDLC (poly 0xEDB88320, init 0xFFFFFFFF, final XOR), bit serial,
 * no table.
 *
 * It lives in common/ because CPU0 and CPU1 must compute identical values for
 * the ABI v3 command/response CRC (zmpio_abi_v3.h). log_record.c's log_crc32()
 * forwards here, so the on-disk ZLOG CRC is the same function.
 */
uint32_t zmpio_crc32(const void *data, size_t length);

#endif
