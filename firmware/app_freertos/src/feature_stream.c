#include "feature_stream.h"

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "app_config.h"
#include "cpu1_log.h"
#include "pl_doorbell.h"
#include "task.h"
#include "zmpio_ipc_logic.h"

/*
 * Typed views of the one staged message.  A union rather than a cast from
 * ipc_message_t::payload, so no code here takes the address of a member of a
 * packed struct (which is both a -Waddress-of-packed-member warning and, on
 * a platform less forgiving than Cortex-A9, a real unaligned access).
 */
typedef struct __attribute__((packed)) {
    msg_header_t header;
    ipc_feature_stream_v1_t payload;
} feature_stream_message_t;

typedef struct __attribute__((packed)) {
    msg_header_t header;
    ipc_dsp_health_stream_v1_t payload;
} dsp_health_message_t;

_Static_assert(sizeof(feature_stream_message_t) <= sizeof(ipc_message_t),
               "feature stream message does not fit an ABI v2 slot");
_Static_assert(sizeof(dsp_health_message_t) <= sizeof(ipc_message_t),
               "DSP health stream message does not fit an ABI v2 slot");

/*
 * Staging buffer for the message being published.  File scope, not stack:
 * see the threading contract in feature_stream.h -- only fpga_result_task
 * ever reaches this, and that task's stack is the one this project has
 * already overflowed twice (app_config.h).
 */
static union {
    ipc_message_t            raw;
    feature_stream_message_t feature;
    dsp_health_message_t     health;
} stream_buffer;

/* Everything ipc_rx_task may read via feature_stream_get_status(); updated
 * from fpga_result_task, so every read and every write of this struct goes
 * through a critical section.  It is small enough (48 bytes) that a critical
 * section costs less than a mutex would, and matches how main.c already
 * shares latest_sensor_sample between the same two tasks. */
static struct {
    uint32_t stream_sequence;      /* last sequence actually published */
    uint32_t dropped_since_last;   /* not yet reported to Linux */
    uint32_t frames_published;
    uint32_t frames_dropped;
    uint32_t health_published;
    uint32_t health_dropped;
    uint32_t last_frame_sequence;
    uint32_t feature_count;
    uint32_t ctrl_drop_count;
    uint32_t bridge_drop_count;
    uint32_t dsp_health_state;
    uint32_t doorbell_rings;
} stream_state;

void feature_stream_init(void)
{
    memset(&stream_buffer, 0, sizeof(stream_buffer));
    taskENTER_CRITICAL();
    memset(&stream_state, 0, sizeof(stream_state));
    taskEXIT_CRITICAL();
}

/*
 * Reserves the next stream_sequence and reads back the losses accumulated
 * since the last successful publish.  The reservation is only made
 * PERMANENT by stream_commit() below, so a message that fails to send never
 * burns a sequence value: a gap in stream_sequence on the Linux side
 * therefore always means "a message was lost after CPU1 published it", never
 * "CPU1 chose not to send it".  That is what keeps the two loss classes
 * distinguishable (SDD_15 Sec.6).
 */
static void stream_reserve(uint32_t *out_sequence, uint32_t *out_dropped)
{
    taskENTER_CRITICAL();
    *out_sequence = stream_state.stream_sequence + 1U;
    *out_dropped = stream_state.dropped_since_last;
    taskEXIT_CRITICAL();
}

/* Returns the drop-streak length after the update (0 on success), so the
 * caller can log the first drop of a streak without flooding UART1. */
static uint32_t stream_commit(bool is_feature, uint32_t sequence,
                              int send_result)
{
    uint32_t streak = 0U;

    taskENTER_CRITICAL();
    if (send_result == IPC_SEND_OK) {
        stream_state.stream_sequence = sequence;
        stream_state.dropped_since_last = 0U;
        if (is_feature) {
            ++stream_state.frames_published;
        } else {
            ++stream_state.health_published;
        }
    } else {
        streak = ++stream_state.dropped_since_last;
        if (is_feature) {
            ++stream_state.frames_dropped;
        } else {
            ++stream_state.health_dropped;
        }
    }
    taskEXIT_CRITICAL();

    if (send_result != IPC_SEND_OK) {
        return streak;
    }

    /*
     * R4: without this the message would sit in the ring until some
     * unrelated command woke Linux up, i.e. silent timer polling.  The
     * doorbell's irq_out is a LEVEL, so a ring landing between Linux's ACK
     * and its UIO re-enable is not lost (SDD_10 Sec.5).
     */
    pl_doorbell_ring();
    taskENTER_CRITICAL();
    ++stream_state.doorbell_rings;
    taskEXIT_CRITICAL();
    return 0U;
}

#if APP_CPU1_UART_LOG_ENABLED
/* First drop of a streak, then every APP_FEATURE_STREAM_DROP_LOG_INTERVAL:
 * a fully stalled consumer holds the ring full for ~164 s (SS3 R3), and one
 * line per lost frame for that long buries everything else on UART1. */
static bool drop_should_log(uint32_t streak)
{
    return (streak == 1U) ||
           ((streak % APP_FEATURE_STREAM_DROP_LOG_INTERVAL) == 0U);
}
#endif

void feature_stream_publish_feature(const fpga_feature_frame_t *frame,
                                    const zlog_anomaly_rule_v1_t *rule,
                                    uint64_t timestamp_us)
{
    uint32_t sequence;
    uint32_t dropped;
    uint32_t streak;
    int send_result;

    if ((frame == NULL) || (rule == NULL)) {
        return;
    }

    memset(&stream_buffer, 0, sizeof(stream_buffer));
    stream_buffer.feature.header.magic = IPC_MAGIC;
    stream_buffer.feature.header.type = (uint32_t)MSG_TYPE_FEATURE_V2;
    stream_buffer.feature.header.length =
        (uint32_t)sizeof(stream_buffer.feature.payload);

    stream_reserve(&sequence, &dropped);
    stream_buffer.feature.header.timestamp = sequence;
    stream_buffer.feature.payload.stream_sequence = sequence;
    stream_buffer.feature.payload.dropped_since_last = dropped;
    stream_buffer.feature.payload.timestamp_us = timestamp_us;
    /* memcpy, not field-by-field assignment: the wire structs are asserted
     * layout-identical to the firmware ones in zlog_feature_v2.h, so a copy
     * is both correct and immune to a field being forgotten when either side
     * grows. */
    memcpy(&stream_buffer.feature.payload.frame, frame,
           sizeof(stream_buffer.feature.payload.frame));
    memcpy(&stream_buffer.feature.payload.rule, rule,
           sizeof(stream_buffer.feature.payload.rule));

    send_result = ipc_send_timeout(&stream_buffer.raw,
                                   APP_FEATURE_STREAM_SEND_TIMEOUT_MS);
    streak = stream_commit(true, sequence, send_result);

    taskENTER_CRITICAL();
    stream_state.last_frame_sequence = frame->frame_sequence;
    taskEXIT_CRITICAL();

#if APP_CPU1_UART_LOG_ENABLED
    if ((streak != 0U) && drop_should_log(streak)) {
        CPU1_LOG("CPU1: feature stream drop frame=%lu streak=%lu reason=%d\r\n",
                 (unsigned long)frame->frame_sequence,
                 (unsigned long)streak, send_result);
    }
#else
    (void)streak;
#endif
}

void feature_stream_publish_health(const zlog_dsp_health_t *health,
                                   uint64_t timestamp_us)
{
    uint32_t sequence;
    uint32_t dropped;
    int send_result;

    if (health == NULL) {
        return;
    }

    memset(&stream_buffer, 0, sizeof(stream_buffer));
    stream_buffer.health.header.magic = IPC_MAGIC;
    stream_buffer.health.header.type = (uint32_t)MSG_TYPE_DSP_HEALTH;
    stream_buffer.health.header.length =
        (uint32_t)sizeof(stream_buffer.health.payload);

    stream_reserve(&sequence, &dropped);
    stream_buffer.health.header.timestamp = sequence;
    stream_buffer.health.payload.stream_sequence = sequence;
    stream_buffer.health.payload.dropped_since_last = dropped;
    stream_buffer.health.payload.timestamp_us = timestamp_us;
    stream_buffer.health.payload.health_version = health->health_version;
    stream_buffer.health.payload.state = health->state;
    stream_buffer.health.payload.fault = health->fault;
    stream_buffer.health.payload.reserved0 = 0U;
    stream_buffer.health.payload.consecutive_fault_count =
        health->consecutive_fault_count;
    stream_buffer.health.payload.feature_count = health->feature_count;
    stream_buffer.health.payload.ctrl_drop_count = health->ctrl_drop_count;
    stream_buffer.health.payload.bridge_drop_count = health->bridge_drop_count;

    send_result = ipc_send_timeout(&stream_buffer.raw,
                                   APP_FEATURE_STREAM_SEND_TIMEOUT_MS);
    (void)stream_commit(false, sequence, send_result);
}

void feature_stream_note_counters(const fpga_dsp_hal_counters_t *counters,
                                  uint8_t dsp_health_state)
{
    if (counters == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    stream_state.feature_count = counters->feature_count;
    stream_state.ctrl_drop_count = counters->ctrl_drop_count;
    stream_state.bridge_drop_count = counters->bridge_drop_count;
    stream_state.dsp_health_state = (uint32_t)dsp_health_state;
    taskEXIT_CRITICAL();
}

void feature_stream_get_status(ipc_stream_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->stream_abi_version = IPC_STREAM_ABI_VERSION;

    taskENTER_CRITICAL();
    out->feature_count = stream_state.feature_count;
    out->frames_published = stream_state.frames_published;
    out->frames_dropped = stream_state.frames_dropped;
    out->health_published = stream_state.health_published;
    out->health_dropped = stream_state.health_dropped;
    out->last_frame_sequence = stream_state.last_frame_sequence;
    out->last_stream_sequence = stream_state.stream_sequence;
    out->ctrl_drop_count = stream_state.ctrl_drop_count;
    out->bridge_drop_count = stream_state.bridge_drop_count;
    out->dsp_health_state = stream_state.dsp_health_state;
    out->doorbell_rings = stream_state.doorbell_rings;
    taskEXIT_CRITICAL();
}
