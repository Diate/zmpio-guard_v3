#ifndef ZMPIO_ZLOG_H
#define ZMPIO_ZLOG_H

#include <stdint.h>

/*
 * The ZLOG container -- the on-disk format every capture in this project uses.
 *
 * Two producers write it: CPU1's storage_task on microSD, and zmpiod into
 * /var/log/zmpio/STREAMnnnnn.ZBIN. software/linux/parse_zlog.py decodes both,
 * so there is exactly one format and one parser.
 *
 * It lives in common/ (shared by CPU0, CPU1 and Linux) rather than in
 * firmware/, so the dependency points the right way.
 *
 * Byte layout is FROZEN -- older captures must keep parsing byte for byte.
 * The _Static_asserts below enforce that.
 */

#define LOG_FILE_MAGIC       0x474F4C5AU /* "ZLOG" */
#define LOG_RECORD_MAGIC     0x52474F4CU /* "LOGR" */
#define LOG_FORMAT_VERSION   1U
#define LOG_MAX_PAYLOAD_SIZE 240U

typedef enum {
    LOG_SOURCE_MPU6050 = 1,
    LOG_SOURCE_LINUX = 2,
    LOG_SOURCE_EVENT = 3,
    /* A reader that does not know a source type must still skip it safely:
     * payload_len + payload_crc32 in log_record_header_t are enough to do that
     * generically. */
    LOG_SOURCE_FEATURE_V2 = 4,
    LOG_SOURCE_ANOMALY_RULE_V1 = 5,
    /* RESERVED. Machine learning is out of scope in V3 (ADR-017), so nothing
     * emits this record type. The value is kept rather than removed so older
     * captures stay self-describing; reusing 6 would silently mis-decode them. */
    LOG_SOURCE_ANOMALY_ML_V1 = 6,
    LOG_SOURCE_DSP_HEALTH = 7
} log_source_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t start_time_us;
    uint32_t reserved[4];
} log_file_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t source;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint16_t payload_len;
    uint16_t flags;
    uint32_t payload_crc32;
} log_record_header_t;

_Static_assert(sizeof(log_file_header_t) == 32U,
               "log file header ABI changed");
_Static_assert(sizeof(log_record_header_t) == 28U,
               "log record header ABI changed");

#endif
