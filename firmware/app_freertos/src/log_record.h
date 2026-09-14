#ifndef LOG_RECORD_H
#define LOG_RECORD_H

#include <stddef.h>
#include <stdint.h>

/*
 * The container itself (magics, version, log_source_t, file/record headers)
 * moved to common/zmpio_zlog.h so that zmpiod can write the same
 * format for the Linux-side feature-stream capture without depending on a
 * firmware header.  Nothing about the bytes changed, and every existing
 * include of "log_record.h" keeps working -- this file still supplies the
 * CPU1-only pieces below.
 */
#include "zmpio_zlog.h"

typedef struct {
    log_record_header_t header;
    uint8_t payload[LOG_MAX_PAYLOAD_SIZE];
} log_queue_item_t;

uint32_t log_crc32(const void *data, size_t length);

#endif
