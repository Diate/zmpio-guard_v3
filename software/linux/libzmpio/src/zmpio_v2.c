/* nanosleep() (glibc feature-test gate, POSIX.1-2001) -- must be defined
 * before any system header is pulled in transitively by libzmpio.h/
 * zmpio_internal.h below, not just before <time.h>'s own #include. */
#define _GNU_SOURCE

#include "libzmpio.h"
#include "zmpio_internal.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#define ZMPIO_V2_POLL_MS       10U
#define ZMPIO_V2_READY_TIMEOUT_MS_DEFAULT 5000U

/* Full compiler+CPU barrier around every ring head/tail update -- needed
 * here (unlike the doorbell path in libzmpio.c) because these rings live in
 * DDR reached through a UIO mapping whose cacheability is Step 6.2's own
 * open question (docs/PLAN_BUOC_6.md T6.2-4's non-cached verification); a
 * device-memory MMIO register access (pl_doorbell.c on the firmware side)
 * needs no such barrier, but a DDR read/write reordered relative to the
 * neighbouring index update would corrupt the ring regardless of caching. */
static void zmpio_memory_barrier(void)
{
    __sync_synchronize();
}

#define TX_CTRL(h) ((volatile ipc_control_t *)ZMPIO_SHM_PTR(h, IPC_TX_CTRL_BASE))
#define TX_BUF(h)  ((volatile ipc_message_t *)ZMPIO_SHM_PTR(h, IPC_TX_BUFFER_BASE))
#define RX_CTRL(h) ((volatile ipc_control_t *)ZMPIO_SHM_PTR(h, IPC_RX_CTRL_BASE))
#define RX_BUF(h)  ((volatile ipc_message_t *)ZMPIO_SHM_PTR(h, IPC_RX_BUFFER_BASE))

static void sleep_ms(uint32_t milliseconds)
{
    struct timespec remaining;

    remaining.tv_sec = (time_t)(milliseconds / 1000U);
    remaining.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        /* Resume for the remaining duration. */
    }
}

static int control_is_valid(const volatile ipc_control_t *control)
{
    return (control->size == IPC_BUFFER_SIZE) &&
           (control->head < control->size) &&
           (control->tail < control->size);
}

zmpio_status_t zmpio_v2_wait_ready(zmpio_handle_t *handle, uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;

    if (handle == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0U) {
        timeout_ms = ZMPIO_V2_READY_TIMEOUT_MS_DEFAULT;
    }

    while (elapsed_ms < timeout_ms) {
        zmpio_memory_barrier();
        if (control_is_valid(TX_CTRL(handle)) &&
            control_is_valid(RX_CTRL(handle))) {
            return ZMPIO_OK;
        }
        sleep_ms(ZMPIO_V2_POLL_MS);
        elapsed_ms += ZMPIO_V2_POLL_MS;
    }
    return ZMPIO_ERR_TIMEOUT;
}

/* Returns 0 on success, -1 ring not ready, -2 ring full. Internal only. */
static int ipc_send_to_cpu1(zmpio_handle_t *handle, const ipc_message_t *message)
{
    uint32_t current_head;
    uint32_t next_head;
    volatile ipc_control_t *rx_ctrl = RX_CTRL(handle);

    zmpio_memory_barrier();
    if (!control_is_valid(rx_ctrl)) {
        return -1;
    }

    current_head = rx_ctrl->head;
    next_head = (current_head + 1U) % IPC_BUFFER_SIZE;
    if (next_head == rx_ctrl->tail) {
        return -2;
    }

    memcpy((void *)&RX_BUF(handle)[current_head], message, sizeof(*message));
    zmpio_memory_barrier();
    rx_ctrl->head = next_head;
    zmpio_memory_barrier();
    return 0;
}

/* Returns 0 on success, -1 ring empty, -2 ring not ready. */
static int ipc_recv_from_cpu1(zmpio_handle_t *handle, ipc_message_t *message)
{
    uint32_t current_tail;
    volatile ipc_control_t *tx_ctrl = TX_CTRL(handle);

    zmpio_memory_barrier();
    if (!control_is_valid(tx_ctrl)) {
        return -2;
    }
    if (tx_ctrl->head == tx_ctrl->tail) {
        return -1;
    }

    current_tail = tx_ctrl->tail;
    zmpio_memory_barrier();
    memcpy(message, (const void *)&TX_BUF(handle)[current_tail],
           sizeof(*message));

    tx_ctrl->tail = (current_tail + 1U) % IPC_BUFFER_SIZE;
    zmpio_memory_barrier();
    return 0;
}

/*
 * ---------------------------------------------------------------------
 * Step 7 R1 (docs/PLAN_BUOC_7.md SS3): ONE drain loop, then demux
 * ---------------------------------------------------------------------
 * Before Step 7 the TX ring carried nothing but replies to requests this
 * process had just sent, so wait_for_reply() could advance `tail` past
 * anything that did not match and lose nothing that mattered.  With the
 * feature stream sharing the ring that behaviour becomes silent data loss:
 * a `zmpioctl status` landing between two feature frames would consume and
 * discard them.
 *
 * So every message now leaves the ring through exactly one function,
 * drain_one(), which routes it by type:
 *   - stream types (FEATURE_V2 / DSP_HEALTH) -> the registered stream sink;
 *   - anything else -> the caller waiting for it, or the orphan queue if
 *     no one is waiting yet.
 * Nothing is thrown away except a message that is structurally invalid
 * (bad magic / impossible length), which cannot be routed anywhere, and a
 * stream message arriving with no sink registered.  Both are counted.
 *
 * This is single-reader by construction and stays correct only while
 * exactly ONE process holds the handle (REQ-STR-002) -- the same
 * single-owner rule libzmpio.h already states, now load-bearing rather than
 * merely advisable.
 */

static int is_stream_type(uint32_t type)
{
    return (type == (uint32_t)MSG_TYPE_FEATURE_V2) ||
           (type == (uint32_t)MSG_TYPE_DSP_HEALTH);
}

/* Exact expected payload size for a stream type, 0 if the type is not one.
 * Checked strictly rather than as a lower bound: a length that does not
 * match means CPU1 and this build disagree about the layout, and decoding
 * it anyway would produce plausible-looking wrong numbers -- the one failure
 * mode REQ-STR-005 forbids. */
static uint32_t stream_payload_size(uint32_t type)
{
    if (type == (uint32_t)MSG_TYPE_FEATURE_V2) {
        return (uint32_t)sizeof(ipc_feature_stream_v1_t);
    }
    if (type == (uint32_t)MSG_TYPE_DSP_HEALTH) {
        return (uint32_t)sizeof(ipc_dsp_health_stream_v1_t);
    }
    return 0U;
}

/* Both stream payloads open with the same two fields, by design (see
 * common/zmpio_protocol.h), so one accessor serves both. */
static void stream_header_of(const ipc_message_t *message,
                             uint32_t *out_sequence, uint32_t *out_dropped)
{
    ipc_feature_stream_v1_t header;

    memset(&header, 0, sizeof(header));
    memcpy(&header, message->payload,
           sizeof(header.stream_sequence) + sizeof(header.dropped_since_last));
    *out_sequence = header.stream_sequence;
    *out_dropped = header.dropped_since_last;
}

static void account_stream_message(zmpio_handle_t *handle,
                                   const ipc_message_t *message)
{
    uint32_t sequence;
    uint32_t dropped;

    stream_header_of(message, &sequence, &dropped);

    handle->stream_stats.reported_drops += (uint64_t)dropped;

    if (handle->stream_seq_valid) {
        uint32_t expected = handle->stream_stats.last_stream_sequence + 1U;

        if (sequence != expected) {
            /* Unsigned wrap makes this correct across the 2^32 rollover and
             * still non-zero for an out-of-order/duplicate sequence, which
             * on an SPSC ring can only mean corruption -- worth counting
             * either way (REQ-STR-005: never silent). */
            handle->stream_stats.stream_gaps += 1U;
            handle->stream_stats.stream_gap_messages +=
                (uint64_t)(uint32_t)(sequence - expected);
        }
    }
    handle->stream_stats.last_stream_sequence = sequence;
    handle->stream_seq_valid = 1;

    handle->stream_stats.stream_messages += 1U;
    if (message->header.type == (uint32_t)MSG_TYPE_FEATURE_V2) {
        ipc_feature_stream_v1_t feature;

        /* Length was already validated exactly in drain_one(). */
        memcpy(&feature, message->payload, sizeof(feature));
        handle->stream_stats.feature_messages += 1U;
        handle->stream_stats.last_frame_sequence = feature.frame.frame_sequence;
    } else {
        handle->stream_stats.health_messages += 1U;
    }
}

static void orphan_push(zmpio_handle_t *handle, const ipc_message_t *message)
{
    if (handle->orphan_count >= ZMPIO_V2_ORPHAN_MAX) {
        /* Oldest-out: a stale reply from a request that already timed out is
         * worth less than the one the caller is waiting for right now. */
        handle->orphan_head = (handle->orphan_head + 1U) % ZMPIO_V2_ORPHAN_MAX;
        handle->orphan_count -= 1U;
        handle->stream_stats.orphan_overflow += 1U;
    }
    handle->orphan[(handle->orphan_head + handle->orphan_count) %
                   ZMPIO_V2_ORPHAN_MAX] = *message;
    handle->orphan_count += 1U;
    handle->stream_stats.orphan_queued += 1U;
}

/* Removes and returns the queued reply whose correlation id is request_id.
 * Returns 1 when one was found. */
static int orphan_take(zmpio_handle_t *handle, uint32_t request_id,
                       ipc_message_t *out)
{
    unsigned i;

    for (i = 0U; i < handle->orphan_count; ++i) {
        unsigned slot = (handle->orphan_head + i) % ZMPIO_V2_ORPHAN_MAX;

        if (handle->orphan[slot].header.timestamp != request_id) {
            continue;
        }
        *out = handle->orphan[slot];
        /* Close the hole by shifting the entries behind it forward; the
         * queue holds at most ZMPIO_V2_ORPHAN_MAX entries, so this stays
         * trivially cheap and keeps arrival order intact. */
        for (; i + 1U < handle->orphan_count; ++i) {
            unsigned dst = (handle->orphan_head + i) % ZMPIO_V2_ORPHAN_MAX;
            unsigned src = (handle->orphan_head + i + 1U) %
                           ZMPIO_V2_ORPHAN_MAX;
            handle->orphan[dst] = handle->orphan[src];
        }
        handle->orphan_count -= 1U;
        return 1;
    }
    return 0;
}

typedef enum {
    DRAIN_EMPTY = 0,
    DRAIN_NOT_READY,
    DRAIN_STREAM,     /* routed to the sink */
    DRAIN_RESPONSE,   /* written to *out_response */
    DRAIN_DISCARDED   /* structurally invalid, or stream with no sink */
} drain_result_t;

/*
 * Pops at most one message.  When it is a response it is handed back in
 * *out_response (never queued) so the caller can decide whether it is the
 * one it wants; a stream message goes straight to the sink.
 */
static drain_result_t drain_one(zmpio_handle_t *handle,
                                ipc_message_t *out_response)
{
    ipc_message_t message;
    int result = ipc_recv_from_cpu1(handle, &message);

    if (result == -1) {
        return DRAIN_EMPTY;
    }
    if (result == -2) {
        return DRAIN_NOT_READY;
    }

    if ((message.header.magic != IPC_MAGIC) ||
        (message.header.length > IPC_PAYLOAD_SIZE)) {
        handle->stream_stats.malformed += 1U;
        return DRAIN_DISCARDED;
    }

    if (is_stream_type(message.header.type)) {
        if (message.header.length != stream_payload_size(message.header.type)) {
            handle->stream_stats.malformed += 1U;
            return DRAIN_DISCARDED;
        }
        if (handle->stream_cb == NULL) {
            /* No consumer registered (a bare zmpio_open() with no sink):
             * count it rather than pretend it never existed, so a
             * misconfigured client is visible in STATUS instead of quietly
             * eating telemetry. */
            handle->stream_stats.no_sink_dropped += 1U;
            return DRAIN_DISCARDED;
        }
        account_stream_message(handle, &message);
        handle->stream_cb(message.header.type, message.payload,
                          message.header.length, handle->stream_user);
        return DRAIN_STREAM;
    }

    *out_response = message;
    return DRAIN_RESPONSE;
}

zmpio_status_t zmpio_v2_set_stream_sink(zmpio_handle_t *handle,
                                        zmpio_stream_cb_t callback,
                                        void *user)
{
    if (handle == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    handle->stream_cb = callback;
    handle->stream_user = user;
    return ZMPIO_OK;
}

zmpio_status_t zmpio_v2_drain(zmpio_handle_t *handle, uint32_t max_messages,
                              uint32_t *out_count)
{
    uint32_t drained = 0U;

    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (handle == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if (max_messages == 0U) {
        max_messages = IPC_BUFFER_SIZE;
    }

    while (drained < max_messages) {
        ipc_message_t response;
        drain_result_t result = drain_one(handle, &response);

        if (result == DRAIN_EMPTY) {
            break;
        }
        if (result == DRAIN_NOT_READY) {
            return ZMPIO_ERR_NOT_READY;
        }
        if (result == DRAIN_RESPONSE) {
            /* Nobody is waiting inside zmpio_v2_request() right now -- hold
             * it instead of discarding it, so a reply that arrives just
             * after its caller timed out is still available to the retry
             * rather than being lost with the ring slot. */
            orphan_push(handle, &response);
        }
        ++drained;
    }

    if (out_count != NULL) {
        *out_count = drained;
    }
    return ZMPIO_OK;
}

void zmpio_v2_get_stream_stats(zmpio_handle_t *handle,
                               zmpio_v2_stream_stats_t *out)
{
    if ((handle == NULL) || (out == NULL)) {
        return;
    }
    *out = handle->stream_stats;
}

/*
 * Every message that goes past without matching the expected reply --
 * stream, orphan or malformed -- advances elapsed_ms by the same poll
 * quantum a ring-empty iteration would, so a busy ring cannot hold this call
 * open past timeout_ms. Stream messages are forwarded to the sink and stale
 * replies are routed to the orphan queue while this loop runs, which is
 * what makes it safe to call zmpio_v2_request() at any time while the
 * feature stream is flowing.
 */
static zmpio_status_t wait_for_reply(zmpio_handle_t *handle,
                                     uint32_t request_id,
                                     ipc_message_t *reply,
                                     uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;

    /* A reply that arrived while some earlier call was draining is already
     * in hand -- check before touching the ring at all. */
    if (orphan_take(handle, request_id, reply)) {
        return ZMPIO_OK;
    }

    while (elapsed_ms < timeout_ms) {
        drain_result_t result = drain_one(handle, reply);

        if (result == DRAIN_NOT_READY) {
            return ZMPIO_ERR_NOT_READY;
        }
        if (result == DRAIN_RESPONSE) {
            if (reply->header.timestamp == request_id) {
                return ZMPIO_OK;
            }
            orphan_push(handle, reply);
            elapsed_ms += ZMPIO_V2_POLL_MS;
            continue;
        }
        if ((result == DRAIN_STREAM) || (result == DRAIN_DISCARDED)) {
            elapsed_ms += ZMPIO_V2_POLL_MS;
            continue;
        }
        sleep_ms(ZMPIO_V2_POLL_MS);
        elapsed_ms += ZMPIO_V2_POLL_MS;
    }
    return ZMPIO_ERR_TIMEOUT;
}

zmpio_status_t zmpio_v2_request(zmpio_handle_t *handle, msg_type_t type,
                                const void *payload, uint32_t payload_len,
                                uint32_t timeout_ms, ipc_message_t *out_reply)
{
    ipc_message_t message;
    uint32_t request_id;
    int send_result;

    if ((handle == NULL) || (out_reply == NULL)) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if ((payload_len > IPC_PAYLOAD_SIZE) ||
        ((payload_len != 0U) && (payload == NULL))) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if (is_stream_type((uint32_t)type)) {
        /* The stream types are unsolicited CPU1 -> Linux telemetry; sending
         * one as a request would put a message CPU1 has no handler for on
         * the RX ring and then wait out the full timeout for a reply that
         * cannot come. */
        return ZMPIO_ERR_INVALID_ARG;
    }

    memset(&message, 0, sizeof(message));
    message.header.magic = IPC_MAGIC;
    message.header.type = (uint32_t)type;
    request_id = handle->next_v2_request_id++;
    message.header.timestamp = request_id;
    message.header.length = payload_len;
    if (payload_len != 0U) {
        memcpy(message.payload, payload, payload_len);
    }

    send_result = ipc_send_to_cpu1(handle, &message);
    if (send_result == -1) {
        return ZMPIO_ERR_NOT_READY;
    }
    if (send_result == -2) {
        return ZMPIO_ERR_RING_FULL;
    }

    return wait_for_reply(handle, request_id, out_reply, timeout_ms);
}

zmpio_status_t zmpio_v2_heartbeat(zmpio_handle_t *handle, uint32_t timeout_ms,
                                  uint32_t *out_remote_protocol_version)
{
    ipc_message_t reply;
    ipc_ack_payload_t ack;
    zmpio_status_t status;

    if (out_remote_protocol_version == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }

    status = zmpio_v2_request(handle, MSG_TYPE_HEARTBEAT, NULL, 0U,
                              timeout_ms, &reply);
    if (status != ZMPIO_OK) {
        return status;
    }
    if ((reply.header.type != (uint32_t)MSG_TYPE_ACK) ||
        (reply.header.length != sizeof(ack))) {
        return ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH;
    }
    memcpy(&ack, reply.payload, sizeof(ack));
    *out_remote_protocol_version = ack.detail;
    if ((ack.request_type != (uint32_t)MSG_TYPE_HEARTBEAT) ||
        (ack.status != 0) || (ack.detail != IPC_PROTOCOL_VERSION)) {
        return ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH;
    }
    return ZMPIO_OK;
}

zmpio_status_t zmpio_v2_stream_status(zmpio_handle_t *handle,
                                      uint32_t timeout_ms,
                                      ipc_stream_status_t *out_status)
{
    ipc_message_t reply;
    zmpio_status_t status;

    if (out_status == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }

    status = zmpio_v2_request(handle, MSG_TYPE_STREAM_STATUS, NULL, 0U,
                              timeout_ms, &reply);
    if (status != ZMPIO_OK) {
        return status;
    }
    if ((reply.header.type != (uint32_t)MSG_TYPE_STREAM_STATUS) ||
        (reply.header.length != sizeof(*out_status))) {
        /* CPU1 answered something else -- most likely an ERROR ack from a
         * firmware built before Step 7, which does not know this type. */
        return ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH;
    }
    memcpy(out_status, reply.payload, sizeof(*out_status));
    if (out_status->stream_abi_version != IPC_STREAM_ABI_VERSION) {
        return ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH;
    }
    return ZMPIO_OK;
}
