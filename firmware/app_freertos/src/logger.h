#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "log_record.h"
#include "queue.h"
#include "zmpio_protocol.h"

typedef enum {
    LOGGER_COMMAND_START = 1,
    LOGGER_COMMAND_STOP,
    LOGGER_COMMAND_FLUSH
} logger_command_t;

typedef ipc_logger_status_t logger_status_t;
typedef ipc_logger_diagnostic_t logger_diagnostic_t;

int logger_init(void);
bool logger_submit(log_source_t source, const void *payload,
                   uint16_t payload_len, uint16_t flags,
                   uint64_t timestamp_us, TickType_t wait_ticks);
bool logger_send_command(logger_command_t command, TickType_t wait_ticks);
void logger_get_status(logger_status_t *status);
void logger_get_diagnostic(logger_diagnostic_t *diagnostic);

/* Storage-task interface. No other module should call these functions. */
bool logger_receive(log_queue_item_t *item, TickType_t wait_ticks);
bool logger_receive_command(logger_command_t *command);
void logger_set_storage_state(bool mounted, bool logging,
                              const char *filename, uint64_t file_bytes);
void logger_note_record_written(uint32_t bytes_written);
void logger_note_io_error(void);
void logger_set_storage_diagnostic(ipc_logger_diagnostic_stage_t stage,
                                   int32_t result,
                                   uint32_t spi_diagnostic,
                                   uint32_t detail);
void logger_clear_storage_diagnostic(void);

#endif
