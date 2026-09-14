#include "logger.h"

#include <stddef.h>
#include <string.h>

#include "app_config.h"
#include "zmpio_protocol.h"

static QueueHandle_t log_queue;
static QueueHandle_t command_queue;
static logger_status_t current_status;
static logger_diagnostic_t current_diagnostic;
static uint32_t sensor_sequence;
static uint32_t linux_sequence;
static uint32_t event_sequence;

static uint32_t next_sequence(log_source_t source)
{
    uint32_t sequence;

    taskENTER_CRITICAL();
    switch (source) {
    case LOG_SOURCE_MPU6050:
        sequence = sensor_sequence++;
        break;
    case LOG_SOURCE_LINUX:
        sequence = linux_sequence++;
        break;
    default:
        sequence = event_sequence++;
        break;
    }
    taskEXIT_CRITICAL();
    return sequence;
}

int logger_init(void)
{
    memset(&current_status, 0, sizeof(current_status));
    memset(&current_diagnostic, 0, sizeof(current_diagnostic));
    current_status.protocol_version = IPC_PROTOCOL_VERSION;
    current_status.logging = 1U;

    log_queue = xQueueCreate(APP_LOG_QUEUE_LENGTH, sizeof(log_queue_item_t));
    command_queue = xQueueCreate(APP_LOG_COMMAND_QUEUE_LENGTH,
                                 sizeof(logger_command_t));
    if ((log_queue == NULL) || (command_queue == NULL)) {
        return -1;
    }
    return 0;
}

bool logger_submit(log_source_t source, const void *payload,
                   uint16_t payload_len, uint16_t flags,
                   uint64_t timestamp_us, TickType_t wait_ticks)
{
    log_queue_item_t item;
    BaseType_t result;

    if ((log_queue == NULL) || (payload == NULL) ||
        (payload_len > LOG_MAX_PAYLOAD_SIZE)) {
        return false;
    }

    memset(&item, 0, sizeof(item));
    item.header.magic = LOG_RECORD_MAGIC;
    item.header.version = LOG_FORMAT_VERSION;
    item.header.source = (uint16_t)source;
    item.header.sequence = next_sequence(source);
    item.header.timestamp_us = timestamp_us;
    item.header.payload_len = payload_len;
    item.header.flags = flags;
    item.header.payload_crc32 = log_crc32(payload, payload_len);
    memcpy(item.payload, payload, payload_len);

    result = xQueueSend(log_queue, &item, wait_ticks);

    taskENTER_CRITICAL();
    if (result == pdTRUE) {
        if (source == LOG_SOURCE_MPU6050) {
            ++current_status.sensor_accepted;
        } else if (source == LOG_SOURCE_LINUX) {
            ++current_status.linux_accepted;
        }
    } else if (source == LOG_SOURCE_MPU6050) {
        ++current_status.sensor_dropped;
    } else if (source == LOG_SOURCE_LINUX) {
        ++current_status.linux_dropped;
    }
    current_status.queue_depth = uxQueueMessagesWaiting(log_queue);
    taskEXIT_CRITICAL();

    return result == pdTRUE;
}

bool logger_send_command(logger_command_t command, TickType_t wait_ticks)
{
    if (command_queue == NULL) {
        return false;
    }
    return xQueueSend(command_queue, &command, wait_ticks) == pdTRUE;
}

bool logger_receive(log_queue_item_t *item, TickType_t wait_ticks)
{
    BaseType_t result;

    if ((log_queue == NULL) || (item == NULL)) {
        return false;
    }
    result = xQueueReceive(log_queue, item, wait_ticks);
    taskENTER_CRITICAL();
    current_status.queue_depth = uxQueueMessagesWaiting(log_queue);
    taskEXIT_CRITICAL();
    return result == pdTRUE;
}

bool logger_receive_command(logger_command_t *command)
{
    if ((command_queue == NULL) || (command == NULL)) {
        return false;
    }
    return xQueueReceive(command_queue, command, 0U) == pdTRUE;
}

void logger_get_status(logger_status_t *status)
{
    if (status == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *status = current_status;
    taskEXIT_CRITICAL();
}

void logger_get_diagnostic(logger_diagnostic_t *diagnostic)
{
    if (diagnostic == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *diagnostic = current_diagnostic;
    taskEXIT_CRITICAL();
}

void logger_set_storage_state(bool mounted, bool logging,
                              const char *filename, uint64_t file_bytes)
{
    taskENTER_CRITICAL();
    current_status.mounted = mounted ? 1U : 0U;
    current_status.logging = logging ? 1U : 0U;
    current_status.file_bytes = file_bytes;
    memset(current_status.filename, 0, sizeof(current_status.filename));
    if (filename != NULL) {
        strncpy(current_status.filename, filename,
                sizeof(current_status.filename) - 1U);
    }
    taskEXIT_CRITICAL();
}

void logger_note_record_written(uint32_t bytes_written)
{
    taskENTER_CRITICAL();
    ++current_status.records_written;
    current_status.file_bytes += bytes_written;
    taskEXIT_CRITICAL();
}

void logger_note_io_error(void)
{
    taskENTER_CRITICAL();
    ++current_status.io_errors;
    taskEXIT_CRITICAL();
}

void logger_set_storage_diagnostic(ipc_logger_diagnostic_stage_t stage,
                                   int32_t result,
                                   uint32_t spi_diagnostic,
                                   uint32_t detail)
{
    taskENTER_CRITICAL();
    if (current_diagnostic.stage == IPC_LOG_DIAG_STAGE_NONE) {
        current_diagnostic.first_stage = (uint32_t)stage;
        current_diagnostic.first_result = result;
        current_diagnostic.first_spi_diagnostic = spi_diagnostic;
        current_diagnostic.first_detail = detail;
    }
    current_diagnostic.stage = (uint32_t)stage;
    current_diagnostic.result = result;
    current_diagnostic.spi_diagnostic = spi_diagnostic;
    current_diagnostic.detail = detail;
    taskEXIT_CRITICAL();
}

void logger_clear_storage_diagnostic(void)
{
    taskENTER_CRITICAL();
    memset(&current_diagnostic, 0, sizeof(current_diagnostic));
    taskEXIT_CRITICAL();
}
