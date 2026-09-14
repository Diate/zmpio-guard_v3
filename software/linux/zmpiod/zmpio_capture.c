#define _GNU_SOURCE

#include "zmpio_capture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zmpio_crc32.h"

#define ZMPIO_CAPTURE_DEFAULT_MAX_BYTES (16ULL * 1024ULL * 1024ULL)
#define ZMPIO_CAPTURE_MAX_FILE_INDEX    99999U

/* Host wall clock at file-open time, purely so a capture file can be placed
 * on a calendar later; every RECORD carries CPU1's own platform_time_us()
 * instead, which is the clock the microSD copy uses and therefore the only
 * one the 7.2 cross-check can compare against. */
static uint64_t host_time_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0U;
    }
    return ((uint64_t)now.tv_sec * 1000000ULL) +
           ((uint64_t)now.tv_nsec / 1000ULL);
}

static int write_file_header(zmpio_capture_t *capture)
{
    log_file_header_t header;

    memset(&header, 0, sizeof(header));
    header.magic = LOG_FILE_MAGIC;
    header.version = (uint16_t)LOG_FORMAT_VERSION;
    header.header_size = (uint16_t)sizeof(header);
    header.start_time_us = host_time_us();

    if (fwrite(&header, sizeof(header), 1U, capture->file) != 1U) {
        return -1;
    }
    capture->file_bytes = sizeof(header);
    return 0;
}

/* Picks the first STREAMnnnnn.ZBIN that does not exist yet, so a daemon
 * restart never truncates the previous run's capture -- the same
 * never-overwrite rule storage_task.c follows for LOGnnnnn.BIN on the card,
 * and what makes the H2 gate ("kill -9, recover, nothing lost") checkable
 * against files rather than only against counters. */
static int open_next_file(zmpio_capture_t *capture)
{
    while (capture->file_index <= ZMPIO_CAPTURE_MAX_FILE_INDEX) {
        int written = snprintf(capture->path, sizeof(capture->path),
                               "%s/STREAM%05u.ZBIN", capture->dir,
                               capture->file_index);

        if ((written < 0) || ((size_t)written >= sizeof(capture->path))) {
            fprintf(stderr, "zmpiod: capture path too long for %s\n",
                    capture->dir);
            return -1;
        }
        if (access(capture->path, F_OK) == 0) {
            ++capture->file_index;
            continue;
        }

        capture->file = fopen(capture->path, "wb");
        if (capture->file == NULL) {
            fprintf(stderr, "zmpiod: cannot create %s: %s\n", capture->path,
                    strerror(errno));
            return -1;
        }
        if (write_file_header(capture) != 0) {
            fprintf(stderr, "zmpiod: cannot write ZLOG header to %s: %s\n",
                    capture->path, strerror(errno));
            fclose(capture->file);
            capture->file = NULL;
            return -1;
        }
        capture->sequence = 0U;
        return 0;
    }

    fprintf(stderr, "zmpiod: capture index exhausted in %s\n", capture->dir);
    return -1;
}

int zmpio_capture_open(zmpio_capture_t *capture, const char *dir,
                       uint64_t max_bytes)
{
    if ((capture == NULL) || (dir == NULL)) {
        return -1;
    }

    memset(capture, 0, sizeof(*capture));
    capture->max_bytes = (max_bytes != 0U) ? max_bytes
                                           : ZMPIO_CAPTURE_DEFAULT_MAX_BYTES;
    strncpy(capture->dir, dir, sizeof(capture->dir) - 1U);

    if ((mkdir(capture->dir, 0755) != 0) && (errno != EEXIST)) {
        fprintf(stderr, "zmpiod: mkdir(%s) failed: %s -- capture disabled\n",
                capture->dir, strerror(errno));
        return -1;
    }
    if (open_next_file(capture) != 0) {
        return -1;
    }

    capture->enabled = 1;
    fprintf(stderr, "zmpiod: capture -> %s (rotate at %llu bytes)\n",
            capture->path, (unsigned long long)capture->max_bytes);
    return 0;
}

static void rotate(zmpio_capture_t *capture)
{
    fclose(capture->file);
    capture->file = NULL;
    ++capture->file_index;
    if (open_next_file(capture) != 0) {
        capture->enabled = 0;
        return;
    }
    ++capture->rotations;
    fprintf(stderr, "zmpiod: capture rotated -> %s\n", capture->path);
}

int zmpio_capture_write(zmpio_capture_t *capture, log_source_t source,
                        uint64_t timestamp_us, uint16_t flags,
                        const void *payload, uint16_t payload_len)
{
    log_record_header_t header;
    size_t record_bytes;

    if ((capture == NULL) || !capture->enabled || (capture->file == NULL)) {
        return -1;
    }
    if ((payload == NULL) || (payload_len > LOG_MAX_PAYLOAD_SIZE)) {
        return -1;
    }

    record_bytes = sizeof(header) + (size_t)payload_len;
    if ((capture->file_bytes + record_bytes) > capture->max_bytes) {
        rotate(capture);
        if (!capture->enabled) {
            return -1;
        }
    }

    memset(&header, 0, sizeof(header));
    header.magic = LOG_RECORD_MAGIC;
    header.version = (uint16_t)LOG_FORMAT_VERSION;
    header.source = (uint16_t)source;
    header.sequence = capture->sequence;
    header.timestamp_us = timestamp_us;
    header.payload_len = payload_len;
    header.flags = flags;
    header.payload_crc32 = zmpio_crc32(payload, payload_len);

    if ((fwrite(&header, sizeof(header), 1U, capture->file) != 1U) ||
        (fwrite(payload, payload_len, 1U, capture->file) != 1U)) {
        ++capture->write_errors;
        /* Do not disable: an ENOSPC that later frees up, or a transient I/O
         * error, must not permanently stop capturing.  The counter is what
         * makes the loss visible (REQ-STR-005). */
        return -1;
    }

    ++capture->sequence;
    ++capture->records_written;
    capture->file_bytes += record_bytes;
    return 0;
}

void zmpio_capture_flush(zmpio_capture_t *capture)
{
    if ((capture != NULL) && capture->enabled && (capture->file != NULL)) {
        (void)fflush(capture->file);
    }
}

void zmpio_capture_close(zmpio_capture_t *capture)
{
    if ((capture == NULL) || (capture->file == NULL)) {
        return;
    }
    (void)fflush(capture->file);
    (void)fclose(capture->file);
    capture->file = NULL;
    capture->enabled = 0;
}
