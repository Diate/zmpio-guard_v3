#ifndef ZMPIO_CAPTURE_H
#define ZMPIO_CAPTURE_H

#include <stdint.h>
#include <stdio.h>

#include "zmpio_zlog.h"

/*
 * Step 7.2 T7.2-5: the Linux-side ZLOG capture writer.
 *
 * zmpiod writes what it receives on the feature stream into
 * <dir>/STREAMnnnnn.ZBIN using the SAME container CPU1 writes to microSD
 * (common/zmpio_zlog.h), one FEATURE_V2 record plus one ANOMALY_RULE_V1
 * record per feature message and one DSP_HEALTH record per health message.
 * Deliberately not a new format: software/linux/parse_zlog.py validates
 * and decodes these files unchanged, so the 7.2 cross-check
 * ("same frame_sequence -> identical 48 bytes on card and on host") is a
 * diff of two CSVs produced by one parser rather than a comparison between
 * two decoders that could disagree.
 *
 * The only field that legitimately differs from the microSD copy of the same
 * frame is log_record_header_t::sequence: CPU1's logger numbers records
 * across every source it writes, while this writer numbers them within the
 * capture.  Payload bytes, timestamp_us, source and flags all match.
 */

#define ZMPIO_CAPTURE_NAME_MAX 256U

typedef struct {
    FILE    *file;
    char     path[ZMPIO_CAPTURE_NAME_MAX];
    char     dir[ZMPIO_CAPTURE_NAME_MAX];
    uint32_t file_index;      /* the nnnnn in STREAMnnnnn.ZBIN */
    uint32_t sequence;        /* record sequence within this file */
    uint64_t file_bytes;
    uint64_t max_bytes;       /* rotate once file_bytes would exceed this */
    uint64_t records_written;
    uint64_t rotations;
    uint64_t write_errors;
    int      enabled;
} zmpio_capture_t;

/*
 * Prepares the writer and opens the first file.  `dir` is created if
 * missing.  max_bytes == 0 selects the default.  Returns 0 on success; on
 * failure the capture is left disabled and zmpiod keeps running (losing the
 * on-disk copy must not take the live stream down with it) -- the caller
 * reports it via STATUS.
 */
int zmpio_capture_open(zmpio_capture_t *capture, const char *dir,
                       uint64_t max_bytes);

/* Appends one record.  Returns 0 on success, -1 on a write error (counted
 * in write_errors; the writer stays open so a transient ENOSPC recovers by
 * itself once space frees up). */
int zmpio_capture_write(zmpio_capture_t *capture, log_source_t source,
                        uint64_t timestamp_us, uint16_t flags,
                        const void *payload, uint16_t payload_len);

/* Flushes buffered data to the filesystem.  Called on the daemon's 1 Hz
 * tick, so a power cut costs at most a second of capture rather than
 * whatever libc happened to be holding. */
void zmpio_capture_flush(zmpio_capture_t *capture);

void zmpio_capture_close(zmpio_capture_t *capture);

#endif
