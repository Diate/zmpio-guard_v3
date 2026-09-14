#ifndef LIBZMPIO_H
#define LIBZMPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "zmpio_abi_v3.h"
#include "zmpio_protocol.h"

/*
 * libzmpio -- the Linux side of both shared-DDR interfaces to CPU1: the ABI v2
 * message ring and the ABI v3 command/response control block. It is the
 * counterpart of firmware/app_freertos/src/ipc_v3.c on the CPU1 side.
 *
 * Ownership: a zmpio_handle_t must be held by exactly ONE process for the life
 * of that process -- zmpiod -- never a short-lived CLI invocation and never two
 * processes at once.
 *
 * No local ring state is cached: every v3 call re-reads cmd_head/rsp_tail from
 * the shared control block. A process restart therefore resumes exactly where
 * the ring left off with no recovery step, and CPU1's session_id (bumped only
 * at CPU1 boot) is unaffected by that restart.
 */

typedef struct zmpio_handle zmpio_handle_t;

typedef enum {
    ZMPIO_OK = 0,
    ZMPIO_ERR_UIO_NOT_FOUND = -1,
    ZMPIO_ERR_MMAP = -2,
    ZMPIO_ERR_NOT_READY = -3,
    ZMPIO_ERR_TIMEOUT = -4,
    ZMPIO_ERR_RING_FULL = -5,
    ZMPIO_ERR_LAYOUT_MISMATCH = -6,
    ZMPIO_ERR_INVALID_ARG = -7,
    ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH = -8,
    ZMPIO_ERR_INTERRUPTED = -9,
    ZMPIO_ERR_ALREADY_OPEN = -10
} zmpio_status_t;

/*
 * Opens both UIO endpoints (zmpio-shm, zmpio-doorbell -- see
 * deploy/petalinux_overlay/system-user-openamp-template.dtsi) and maps shared
 * DDR. Does NOT wait for CPU1 and does NOT perform HELLO: call
 * zmpio_v2_wait_ready() / zmpio_v3_wait_cpu1_ready() and zmpio_v3_hello()
 * explicitly.
 */
zmpio_status_t zmpio_open(zmpio_handle_t **out_handle);

/* Safe to call on an already-closed or NULL handle. */
void zmpio_close(zmpio_handle_t *handle);

/* ---- ABI v2 (SDD_04) ---- */

zmpio_status_t zmpio_v2_wait_ready(zmpio_handle_t *handle,
                                   uint32_t timeout_ms);

/*
 * Verify the protocol version by heartbeat before issuing any business
 * request. *out_remote_protocol_version is always written when this returns
 * ZMPIO_OK; the caller compares it against IPC_PROTOCOL_VERSION.
 */
zmpio_status_t zmpio_v2_heartbeat(zmpio_handle_t *handle, uint32_t timeout_ms,
                                  uint32_t *out_remote_protocol_version);

/*
 * Generic ABI v2 request/reply -- the primitive every higher-level v2
 * operation is built from (logger status/diagnostic/start/stop/flush, sensor
 * sample, log data).
 *
 * Guarantees:
 *   - A discarded malformed or unmatched reply counts toward timeout_ms, so a
 *     stream of stale traffic cannot push this call past its stated budget.
 *   - payload/out_reply == NULL returns ZMPIO_ERR_INVALID_ARG rather than
 *     being a precondition left to the caller.
 */
zmpio_status_t zmpio_v2_request(zmpio_handle_t *handle, msg_type_t type,
                                const void *payload, uint32_t payload_len,
                                uint32_t timeout_ms, ipc_message_t *out_reply);

/* ---- ABI v2 feature stream (Step 7, SDD_15) ---- */

/*
 * Called once per unsolicited CPU1 -> Linux stream message, from inside
 * whichever library call happened to drain it (zmpio_v2_drain() normally,
 * zmpio_v2_request() if a request was in flight at the time).  `payload`
 * points into a caller-owned buffer that is reused as soon as the callback
 * returns, so a sink that needs the data later must copy it.
 *
 * msg_type is MSG_TYPE_FEATURE_V2 (payload: ipc_feature_stream_v1_t) or
 * MSG_TYPE_DSP_HEALTH (payload: ipc_dsp_health_stream_v1_t); payload_len is
 * already validated to be exactly the size of the matching struct, so the
 * sink may memcpy without re-checking.
 *
 * The sink MUST NOT call back into this library: it runs in the middle of a
 * drain, and a nested drain would reorder the very stream it is consuming.
 * It should also stay short -- a request waiting on the same drain loop pays
 * for whatever the sink does.
 */
typedef void (*zmpio_stream_cb_t)(uint32_t msg_type, const void *payload,
                                  uint32_t payload_len, void *user);

/*
 * Everything the drain loop learned about the stream, for STATUS output and
 * for the Step 7.2/7.4 gates.  64-bit counters: an 8-hour soak at 1.5625
 * frames/s is only ~45k messages, but nothing here should need thinking
 * about again at the 2^32 boundary.
 */
typedef struct {
    uint64_t stream_messages;     /* delivered to the sink */
    uint64_t feature_messages;
    uint64_t health_messages;
    uint64_t reported_drops;      /* sum of dropped_since_last -- frames CPU1
                                   * threw away before they ever reached the
                                   * ring */
    uint64_t stream_gaps;         /* times stream_sequence was not last+1 --
                                   * i.e. loss AFTER CPU1 published, which
                                   * must never happen with one reader */
    uint64_t stream_gap_messages; /* how many messages those gaps span */
    uint64_t malformed;           /* bad magic / wrong length, discarded */
    uint64_t orphan_queued;       /* replies parked for a later caller */
    uint64_t orphan_overflow;     /* parked replies evicted unread */
    uint64_t no_sink_dropped;     /* stream arrived with no sink registered */
    uint32_t last_stream_sequence;
    uint32_t last_frame_sequence;
} zmpio_v2_stream_stats_t;

/*
 * Registers (or, with callback == NULL, removes) the stream sink.  Must be
 * called before the first drain if any telemetry is to be kept: a stream
 * message that arrives with no sink is counted in no_sink_dropped and
 * discarded, because the alternative -- leaving it in the ring -- would stall
 * every later message behind it.
 */
zmpio_status_t zmpio_v2_set_stream_sink(zmpio_handle_t *handle,
                                        zmpio_stream_cb_t callback,
                                        void *user);

/*
 * Drains up to max_messages (0 means "the whole ring") without blocking:
 * stream messages go to the sink, replies nobody is waiting for are parked.
 * This is what zmpiod calls after a doorbell wakeup.  *out_count is the
 * number of messages actually taken off the ring, which is how the 7.2 gate
 * "poll() wakeups == DBELL_COUNT delta" is measured without a timer.
 */
zmpio_status_t zmpio_v2_drain(zmpio_handle_t *handle, uint32_t max_messages,
                              uint32_t *out_count);

void zmpio_v2_get_stream_stats(zmpio_handle_t *handle,
                               zmpio_v2_stream_stats_t *out);

/*
 * CPU1's own view of the stream (MSG_TYPE_STREAM_STATUS request/response).
 * Returns ZMPIO_ERR_PROTOCOL_VERSION_MISMATCH against a CPU1 image built
 * before Step 7 -- it answers such a request with an ERROR ack.
 */
zmpio_status_t zmpio_v2_stream_status(zmpio_handle_t *handle,
                                      uint32_t timeout_ms,
                                      ipc_stream_status_t *out_status);

/* ---- ABI v3 (SDD_05) ---- */

typedef struct {
    uint32_t session_id;         /* CPU1's session_id, learned from HELLO_ACK */
    uint32_t remote_layout_hash; /* echoed in HELLO_ACK */
    bool     online;             /* HELLO_ACK received AND layout hashes matched */
} zmpio_v3_link_t;

zmpio_status_t zmpio_v3_wait_cpu1_ready(zmpio_handle_t *handle,
                                        uint32_t timeout_ms);

/* Direct port of cpu0_ipc_v3_hello() -- see that function's doc comment in
 * firmware/cpu0_application/src/cpu0_ipc_v3.c for the retry/correlation_id
 * discipline this replicates exactly. */
zmpio_status_t zmpio_v3_hello(zmpio_handle_t *handle, uint32_t max_attempts,
                              uint32_t timeout_ms, zmpio_v3_link_t *out_link);

zmpio_status_t zmpio_v3_set_dsp_config(
    zmpio_handle_t *handle, const zmpio_v3_link_t *link,
    const zmpio_v3_set_dsp_config_t *config, uint32_t max_attempts,
    uint32_t timeout_ms, int32_t *out_status);

zmpio_status_t zmpio_v3_dsp_soft_reset(
    zmpio_handle_t *handle, const zmpio_v3_link_t *link,
    uint32_t max_attempts, uint32_t timeout_ms,
    zmpio_v3_dsp_soft_reset_ack_t *out_ack);

zmpio_status_t zmpio_v3_fifo_full_inject(
    zmpio_handle_t *handle, const zmpio_v3_link_t *link, uint32_t hold_ms,
    uint32_t max_attempts, uint32_t timeout_ms,
    zmpio_v3_fifo_full_inject_ack_t *out_ack);

void zmpio_v3_get_counters(zmpio_handle_t *handle,
                           uint32_t *out_cpu1_crc_drop_count,
                           uint32_t *out_cpu0_crc_drop_count);

/* ---- Doorbell (SDD_10) ---- */

/*
 * Blocks until CPU1 rings the doorbell (or a signal interrupts the wait),
 * ACKs it (ACK-before-re-enable ordering, see tools/uio_spike.c and
 * docs/PLAN_BUOC_6.md SS4 6.2), and re-enables the UIO IRQ before
 * returning. The caller's job after this returns is simply "go check the
 * v2/v3 rings now, something is probably there" -- the doorbell itself
 * carries no data (SDD_10 SS2).
 *
 * Returns ZMPIO_OK on a real wakeup, ZMPIO_ERR_INTERRUPTED if a signal
 * (e.g. SIGTERM during zmpiod shutdown) interrupted the wait before any
 * doorbell fired.
 */
zmpio_status_t zmpio_wait_doorbell(zmpio_handle_t *handle);

uint32_t zmpio_doorbell_count(zmpio_handle_t *handle);

/*
 * Raw doorbell UIO file descriptor, for a caller (zmpiod, Step 6.4) that
 * wants to integrate the wakeup into its OWN poll()/select() loop
 * alongside other fds (e.g. a listening socket) instead of blocking
 * exclusively inside zmpio_wait_doorbell(). Once poll() reports this fd
 * readable, call zmpio_wait_doorbell() to actually service it (drain,
 * ACK, re-enable) -- its internal read() will not block at that point.
 * Read-only: never read()/write() this fd directly, that is
 * zmpio_wait_doorbell()'s job (it owns the ACK-before-re-enable
 * sequencing, see its doc comment).
 */
int zmpio_doorbell_fd(zmpio_handle_t *handle);

#endif
