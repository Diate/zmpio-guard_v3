#ifndef FEATURE_STREAM_H
#define FEATURE_STREAM_H

#include <stdint.h>

#include "fpga_dsp_hal.h"
#include "zlog_feature_v2.h"
#include "zmpio_protocol.h"

/*
 * The CPU1 -> Linux telemetry publisher.
 *
 * Without this module, feature_frame_v2 leaves CPU1 only through the microSD
 * card CPU1 owns exclusively, which Linux cannot read at all. This module is
 * the second consumer of the same frames: it wraps each one (plus its
 * paired rule verdict) in an ipc_feature_stream_v1_t and publishes it on the
 * EXISTING ABI v2 TX ring as MSG_TYPE_FEATURE_V2, then rings the PL doorbell
 * so Linux's poll() wakes up instead of polling a timer (R4).
 *
 * Threading contract (deliberately narrow, so no lock is needed on the hot
 * path):
 *   - feature_stream_publish_feature()/_publish_health()/_note_counters()
 *     are called ONLY from fpga_result_task (main.c).  They share one
 *     file-scope ipc_message_t staging buffer for exactly that reason:
 *     putting a 256-byte message on fpga_result_task's stack instead is the
 *     same failure class as the stack overflows recorded in
 *     app_config.h (APP_FPGA_RESULT_TASK_STACK_WORDS), and this task's
 *     frame is already the heaviest in the firmware.
 *   - feature_stream_get_status() is called from ipc_rx_task, i.e. a
 *     DIFFERENT task, so it and the counter updates it reads are the only
 *     things guarded by a critical section.
 *
 * Loss policy (REQ-STR-001): a frame that cannot be published -- ring full
 * because Linux stopped consuming, or the TX mutex still held past the
 * budget -- is dropped immediately and counted.  The count is then carried
 * out-of-band-free inside the NEXT message that does get through
 * (dropped_since_last), so a reader can always account for every frame
 * without correlating a separately-read counter.  Blocking instead would
 * stall the PL FIFO drain, which is a real fault rather than a telemetry
 * gap (R2/R3).
 */

/* Zero all counters and sequence numbers.  Call once from main(), after
 * ipc_init() and before the scheduler starts. */
void feature_stream_init(void);

/* Publish one FEATURE_V2 stream message.  `frame` and `rule` are copied
 * verbatim (same bytes storage_task writes to microSD), which is what makes
 * the 7.2 cross-check against LOGnnnnn.BIN a byte-exact comparison.  Never
 * blocks longer than APP_FEATURE_STREAM_SEND_TIMEOUT_MS. */
void feature_stream_publish_feature(const fpga_feature_frame_t *frame,
                                    const zlog_anomaly_rule_v1_t *rule,
                                    uint64_t timestamp_us);

/* Publish one DSP_HEALTH stream message.  Called at exactly the moments
 * main.c writes a DSP_HEALTH ZLOG record (state change + degraded
 * heartbeat), so any frame_sequence jump Linux sees has an in-band
 * explanation. */
void feature_stream_publish_health(const zlog_dsp_health_t *health,
                                   uint64_t timestamp_us);

/* Cache the PL counter snapshot fpga_result_task has just read, so
 * feature_stream_get_status() can answer MSG_TYPE_STREAM_STATUS without
 * touching zmpio_dsp_ctrl MMIO from ipc_rx_task (fpga_dsp_hal_get_counters()
 * clears the sticky overflow bit as a side effect -- calling it from a
 * second task would steal that event from fpga_result_task's health state
 * machine). */
void feature_stream_note_counters(const fpga_dsp_hal_counters_t *counters,
                                  uint8_t dsp_health_state);

/* Fill the MSG_TYPE_STREAM_STATUS response payload.  Safe to call from any
 * task. */
void feature_stream_get_status(ipc_stream_status_t *out);

#endif
