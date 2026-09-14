#include "storage_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "app_config.h"
#include "cpu1_log.h"
#include "diskio_sd.h"
#include "ff.h"
#include "logger.h"
#include "log_record.h"
#include "platform_time.h"
#include "task.h"

#define LOG_FILE_SEARCH_LIMIT 100000U

typedef struct {
    FATFS filesystem;
    FIL file;
    bool mounted;
    bool open;
    char filename[16];
    uint64_t file_bytes;
} storage_context_t;

static void storage_note_failure(ipc_logger_diagnostic_stage_t stage,
                                 FRESULT result, uint32_t detail)
{
    logger_set_storage_diagnostic(stage, (int32_t)result,
                                  sd_disk_diagnostic(), detail);
}

static FRESULT write_exact(FIL *file, const void *data, UINT length,
                           UINT *written_out)
{
    FRESULT result;
    UINT written = 0U;

    result = f_write(file, data, length, &written);
    if (written_out != NULL) {
        *written_out = written;
    }
    if (result != FR_OK) {
        return result;
    }
    return written == length ? FR_OK : FR_DISK_ERR;
}

static void storage_close(storage_context_t *context)
{
    if (context->open) {
        (void)f_sync(&context->file);
        (void)f_close(&context->file);
        context->open = false;
    }
    if (context->mounted) {
        (void)f_mount(NULL, "0:", 0U);
        context->mounted = false;
    }
    context->filename[0] = '\0';
    context->file_bytes = 0U;
}

static FRESULT find_new_filename(char *filename, size_t filename_size)
{
    uint32_t index;

    for (index = 0U; index < LOG_FILE_SEARCH_LIMIT; ++index) {
        FILINFO info;
        FRESULT result;

        (void)snprintf(filename, filename_size, "0:/LOG%05lu.BIN",
                       (unsigned long)index);
        result = f_stat(filename, &info);
        if (result == FR_NO_FILE) {
            return FR_OK;
        }
        if (result != FR_OK) {
            return result;
        }
    }
    return FR_DENIED;
}

#if APP_SD_SELF_TEST_ENABLED
static void storage_make_self_test_payload(uint8_t *payload, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        /* A non-constant pattern catches offset, bit and repeated-byte faults. */
        payload[index] = (uint8_t)(((index * 37U) ^ (index >> 1U)) + 0x5AU);
    }
}

/*
 * This deliberately uses a normal FatFs file rather than CMD24 on a raw LBA.
 * Consequently it cannot overwrite the partition table or a FAT data sector
 * owned by another file.  The file is retained as a small, inspectable proof
 * of the test; it is replaced on the next successful mount.
 */
static bool storage_run_self_test(void)
{
    FIL file;
    FRESULT result;
    UINT transferred = 0U;
    uint8_t expected[APP_SD_SELF_TEST_BYTES];
    uint8_t actual[APP_SD_SELF_TEST_BYTES];
    uint32_t expected_crc;
    uint32_t actual_crc;
    bool open = false;

    storage_make_self_test_payload(expected, sizeof(expected));
    expected_crc = log_crc32(expected, sizeof(expected));
    CPU1_LOG("CPU1: SD self-test begin file=%s bytes=%lu crc=0x%08lx\r\n",
               APP_SD_SELF_TEST_FILENAME, (unsigned long)sizeof(expected),
               (unsigned long)expected_crc);

    result = f_open(&file, APP_SD_SELF_TEST_FILENAME,
                    FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=open-write fr=%d\r\n",
                   (int)result);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_OPEN_WRITE,
                             result, 0U);
        return false;
    }
    open = true;

    result = f_write(&file, expected, sizeof(expected), &transferred);
    if ((result != FR_OK) || (transferred != sizeof(expected))) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=write fr=%d bytes=%lu\r\n",
                   (int)result, (unsigned long)transferred);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_WRITE,
                             result != FR_OK ? result : FR_DISK_ERR,
                             (uint32_t)transferred);
        (void)f_close(&file);
        return false;
    }
    result = f_sync(&file);
    if (result != FR_OK) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=sync fr=%d\r\n",
                   (int)result);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_SYNC, result, 0U);
        (void)f_close(&file);
        return false;
    }
    result = f_close(&file);
    open = false;
    if (result != FR_OK) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=close-write fr=%d\r\n",
                   (int)result);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_CLOSE_WRITE,
                             result, 0U);
        return false;
    }

    result = f_open(&file, APP_SD_SELF_TEST_FILENAME, FA_READ);
    if (result != FR_OK) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=open-read fr=%d\r\n",
                   (int)result);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_OPEN_READ,
                             result, 0U);
        return false;
    }
    open = true;
    result = f_read(&file, actual, sizeof(actual), &transferred);
    actual_crc = log_crc32(actual, transferred);
    if (result != FR_OK) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=read fr=%d bytes=%lu\r\n",
                   (int)result, (unsigned long)transferred);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_READ, result,
                             (uint32_t)transferred);
    } else if (transferred != sizeof(expected)) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=length expected=%lu got=%lu\r\n",
                   (unsigned long)sizeof(expected),
                   (unsigned long)transferred);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_LENGTH,
                             FR_DISK_ERR, (uint32_t)transferred);
    } else if ((actual_crc != expected_crc) ||
               (memcmp(expected, actual, sizeof(expected)) != 0)) {
        CPU1_LOG("CPU1: SD self-test FAIL stage=compare expected=0x%08lx "
                   "actual=0x%08lx\r\n", (unsigned long)expected_crc,
                   (unsigned long)actual_crc);
        storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_COMPARE,
                             FR_INT_ERR, actual_crc);
    } else {
        result = FR_OK;
    }

    if (open) {
        FRESULT close_result = f_close(&file);

        if (close_result != FR_OK) {
            CPU1_LOG("CPU1: SD self-test FAIL stage=close-read fr=%d\r\n",
                     (int)close_result);
            storage_note_failure(IPC_LOG_DIAG_STAGE_SELFTEST_CLOSE_READ,
                                 close_result, 0U);
            return false;
        }
    }
    if ((result != FR_OK) || (transferred != sizeof(expected)) ||
        (actual_crc != expected_crc) ||
        (memcmp(expected, actual, sizeof(expected)) != 0)) {
        return false;
    }

    CPU1_LOG("CPU1: SD self-test PASS bytes=%lu crc=0x%08lx\r\n",
               (unsigned long)transferred, (unsigned long)actual_crc);
    return true;
}
#endif

static bool storage_open(storage_context_t *context)
{
    log_file_header_t header;
    FRESULT result;

    result = f_mount(&context->filesystem, "0:", 1U);
    if (result != FR_OK) {
        CPU1_LOG("CPU1: SD FATFS mount failed fr=%d\r\n", (int)result);
        storage_note_failure(IPC_LOG_DIAG_STAGE_FATFS_MOUNT, result,
                             sd_disk_diagnostic_detail());
        return false;
    }
    context->mounted = true;

#if APP_SD_SELF_TEST_ENABLED
    if (!storage_run_self_test()) {
        storage_close(context);
        return false;
    }
#endif

    result = find_new_filename(context->filename, sizeof(context->filename));
    if (result != FR_OK) {
        storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_NAME, result, 0U);
        storage_close(context);
        return false;
    }
    result = f_open(&context->file, context->filename, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK) {
        storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_OPEN, result, 0U);
        storage_close(context);
        return false;
    }
    context->open = true;

    memset(&header, 0, sizeof(header));
    header.magic = LOG_FILE_MAGIC;
    header.version = LOG_FORMAT_VERSION;
    header.header_size = sizeof(header);
    header.start_time_us = platform_time_us();
    {
        UINT written = 0U;

        result = write_exact(&context->file, &header, sizeof(header),
                             &written);
        if (result != FR_OK) {
            storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_HEADER_WRITE,
                                 result, (uint32_t)written);
            storage_close(context);
            return false;
        }
    }
    result = f_sync(&context->file);
    if (result != FR_OK) {
        storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_HEADER_SYNC, result, 0U);
        storage_close(context);
        return false;
    }

    context->file_bytes = sizeof(header);
    logger_clear_storage_diagnostic();
    return true;
}

static FRESULT storage_write_record(storage_context_t *context,
                                    const log_queue_item_t *item,
                                    UINT *written_out)
{
    FRESULT result;

    result = write_exact(&context->file, &item->header, sizeof(item->header),
                         written_out);
    if (result != FR_OK) {
        return result;
    }
    return write_exact(&context->file, item->payload, item->header.payload_len,
                       written_out);
}

void storage_task(void *parameters)
{
    storage_context_t context;
    log_queue_item_t pending;
    bool have_pending = false;
    bool logging_enabled = true;
    TickType_t last_sync = xTaskGetTickCount();

    (void)parameters;
    memset(&context, 0, sizeof(context));
    logger_set_storage_state(false, logging_enabled, NULL, 0U);

    for (;;) {
        logger_command_t command;

        while (logger_receive_command(&command)) {
            if (command == LOGGER_COMMAND_STOP) {
                logging_enabled = false;
                storage_close(&context);
            } else if (command == LOGGER_COMMAND_START) {
                if (!logging_enabled) {
                    logging_enabled = true;
                }
            } else if ((command == LOGGER_COMMAND_FLUSH) && context.open) {
                FRESULT result = f_sync(&context.file);

                if (result != FR_OK) {
                    storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_FLUSH_SYNC,
                                         result, 0U);
                    logger_note_io_error();
                    storage_close(&context);
                }
                last_sync = xTaskGetTickCount();
            }
            logger_set_storage_state(context.mounted, logging_enabled,
                                     context.open ? context.filename : NULL,
                                     context.file_bytes);
        }

        if (!logging_enabled) {
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }

        if (!context.open) {
            if (!storage_open(&context)) {
                logger_note_io_error();
                logger_set_storage_state(false, true, NULL, 0U);
                vTaskDelay(pdMS_TO_TICKS(APP_LOG_RETRY_DELAY_MS));
                continue;
            }
            CPU1_LOG("CPU1: logging to %s\r\n", context.filename);
            logger_set_storage_state(true, true, context.filename,
                                     context.file_bytes);
            last_sync = xTaskGetTickCount();
        }

        if (!have_pending) {
            have_pending = logger_receive(&pending, pdMS_TO_TICKS(20U));
        }
        if (have_pending) {
            uint32_t record_bytes = sizeof(pending.header) +
                                    pending.header.payload_len;
            UINT written = 0U;
            FRESULT result;

            result = storage_write_record(&context, &pending, &written);
            if (result == FR_OK) {
                context.file_bytes += record_bytes;
                logger_note_record_written(record_bytes);
                have_pending = false;
            } else {
                storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_RECORD_WRITE,
                                     result, (uint32_t)written);
                logger_note_io_error();
                storage_close(&context);
                logger_set_storage_state(false, true, NULL, 0U);
                continue;
            }
        }

        if ((TickType_t)(xTaskGetTickCount() - last_sync) >=
            pdMS_TO_TICKS(APP_LOG_SYNC_INTERVAL_MS)) {
            FRESULT result = f_sync(&context.file);

            if (result != FR_OK) {
                storage_note_failure(IPC_LOG_DIAG_STAGE_LOG_PERIODIC_SYNC,
                                     result, 0U);
                logger_note_io_error();
                storage_close(&context);
                logger_set_storage_state(false, true, NULL, 0U);
            }
            last_sync = xTaskGetTickCount();
        }
    }
}
