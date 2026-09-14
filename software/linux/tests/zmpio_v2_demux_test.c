/*
 * Host regression test for the Step 7.2 ABI v2 drain/demux rework
 * (docs/PLAN_BUOC_7.md SS3 R1, docs/sdd_sad/SDD_15_FEATURE_STREAM.md).
 *
 * R1 is the most dangerous change in Step 7: the TX ring now carries both
 * the unsolicited feature stream and command replies, and the pre-Step-7
 * wait_for_reply() advanced `tail` past -- i.e. destroyed -- anything that
 * was not the reply it wanted.  That failure is silent, data-dependent and
 * needs no hardware to reproduce, so it is tested here rather than being
 * left for the board session to discover.
 *
 * The test drives the real zmpio_v2.c against a plain malloc'd buffer laid
 * out exactly like the shared DDR window, writing into the TX ring the way
 * CPU1's ipc_send() does.  No UIO, no board, no root:
 *
 *   cc -std=c11 -Wall -Wextra -I../../../common -I../libzmpio/include \
 *      -I../libzmpio/src -o demux_test zmpio_v2_demux_test.c && ./demux_test
 *
 * It is also wired into software/linux/CMakeLists.txt as the `zmpio_v2_demux`
 * test target, so a native build on the PetaLinux VM runs it via ctest.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
/* MinGW has no nanosleep().  Nothing this test exercises ever sleeps (the
 * ring always has data or the call is expected to return immediately), so a
 * stub keeps the file buildable on the development host too. */
static int zmpio_test_nanosleep(const struct timespec *request,
                                struct timespec *remaining);
#define nanosleep(request, remaining) \
    zmpio_test_nanosleep((request), (remaining))
#endif

#include "zmpio_v2.c"

#if defined(_WIN32)
static int zmpio_test_nanosleep(const struct timespec *request,
                                struct timespec *remaining)
{
    (void)request;
    (void)remaining;
    return 0;
}
#endif

static int g_failures;

#define CHECK(condition, ...)                                              \
    do {                                                                   \
        if (!(condition)) {                                                \
            ++g_failures;                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* ---------------- fake shared memory + CPU1 side ---------------- */

typedef struct {
    uint32_t types[512];
    uint32_t sequences[512];
    uint32_t frames[512];
    unsigned count;
} sink_log_t;

static void sink(uint32_t msg_type, const void *payload, uint32_t payload_len,
                 void *user)
{
    sink_log_t *log = (sink_log_t *)user;

    if (log->count >= (sizeof(log->types) / sizeof(log->types[0]))) {
        return;
    }
    log->types[log->count] = msg_type;

    if (msg_type == (uint32_t)MSG_TYPE_FEATURE_V2) {
        ipc_feature_stream_v1_t feature;

        if (payload_len != sizeof(feature)) {
            ++g_failures;
            printf("FAIL sink: feature payload_len=%u\n", payload_len);
            return;
        }
        memcpy(&feature, payload, sizeof(feature));
        log->sequences[log->count] = feature.stream_sequence;
        log->frames[log->count] = feature.frame.frame_sequence;
    } else {
        ipc_dsp_health_stream_v1_t health;

        if (payload_len != sizeof(health)) {
            ++g_failures;
            printf("FAIL sink: health payload_len=%u\n", payload_len);
            return;
        }
        memcpy(&health, payload, sizeof(health));
        log->sequences[log->count] = health.stream_sequence;
        log->frames[log->count] = 0U;
    }
    ++log->count;
}

static uint8_t *g_shared;

static zmpio_handle_t *make_handle(sink_log_t *log)
{
    zmpio_handle_t *handle = (zmpio_handle_t *)calloc(1U, sizeof(*handle));
    volatile ipc_control_t *tx;
    volatile ipc_control_t *rx;

    handle->shm.fd = -1;
    handle->doorbell.fd = -1;
    handle->shm.base = g_shared;
    handle->shm.size = SHARED_MEM_SIZE;
    handle->next_v2_request_id = 1U;

    tx = TX_CTRL(handle);
    rx = RX_CTRL(handle);
    tx->head = 0U;
    tx->tail = 0U;
    tx->size = IPC_BUFFER_SIZE;
    rx->head = 0U;
    rx->tail = 0U;
    rx->size = IPC_BUFFER_SIZE;

    zmpio_v2_set_stream_sink(handle, sink, log);
    return handle;
}

/* Exactly what firmware ipc_send() does: copy into the slot at head, then
 * publish the new head. */
static void cpu1_publish(zmpio_handle_t *handle, const ipc_message_t *message)
{
    volatile ipc_control_t *tx = TX_CTRL(handle);
    uint32_t head = tx->head;
    uint32_t next = (head + 1U) % IPC_BUFFER_SIZE;

    if (next == tx->tail) {
        ++g_failures;
        printf("FAIL cpu1_publish: ring full\n");
        return;
    }
    memcpy((void *)&TX_BUF(handle)[head], message, sizeof(*message));
    tx->head = next;
}

static void cpu1_publish_feature(zmpio_handle_t *handle, uint32_t stream_seq,
                                 uint32_t frame_seq, uint32_t dropped)
{
    ipc_message_t message;
    ipc_feature_stream_v1_t feature;

    memset(&message, 0, sizeof(message));
    memset(&feature, 0, sizeof(feature));
    feature.stream_sequence = stream_seq;
    feature.dropped_since_last = dropped;
    feature.timestamp_us = 1000000ULL + stream_seq;
    feature.frame.frame_sequence = frame_seq;
    feature.frame.rms_q24_8 = 1000U + frame_seq;
    feature.rule.rule_version = 2U;
    feature.rule.frame_sequence = frame_seq;

    message.header.magic = IPC_MAGIC;
    message.header.type = (uint32_t)MSG_TYPE_FEATURE_V2;
    message.header.timestamp = stream_seq;
    message.header.length = (uint32_t)sizeof(feature);
    memcpy(message.payload, &feature, sizeof(feature));
    cpu1_publish(handle, &message);
}

static void cpu1_publish_health(zmpio_handle_t *handle, uint32_t stream_seq,
                                uint8_t state)
{
    ipc_message_t message;
    ipc_dsp_health_stream_v1_t health;

    memset(&message, 0, sizeof(message));
    memset(&health, 0, sizeof(health));
    health.stream_sequence = stream_seq;
    health.health_version = 1U;
    health.state = state;

    message.header.magic = IPC_MAGIC;
    message.header.type = (uint32_t)MSG_TYPE_DSP_HEALTH;
    message.header.timestamp = stream_seq;
    message.header.length = (uint32_t)sizeof(health);
    memcpy(message.payload, &health, sizeof(health));
    cpu1_publish(handle, &message);
}

static void cpu1_publish_ack(zmpio_handle_t *handle, uint32_t request_id,
                             uint32_t request_type)
{
    ipc_message_t message;
    ipc_ack_payload_t ack;

    memset(&message, 0, sizeof(message));
    memset(&ack, 0, sizeof(ack));
    ack.request_type = request_type;
    ack.status = 0;
    ack.detail = IPC_PROTOCOL_VERSION;

    message.header.magic = IPC_MAGIC;
    message.header.type = (uint32_t)MSG_TYPE_ACK;
    message.header.timestamp = request_id;
    message.header.length = (uint32_t)sizeof(ack);
    memcpy(message.payload, &ack, sizeof(ack));
    cpu1_publish(handle, &message);
}

/* ---------------- the cases ---------------- */

/*
 * The R1 regression itself: a request issued while the stream is flowing
 * must find its reply AND leave every stream message intact and in order.
 * Before Step 7 this test would have lost all five feature messages.
 */
static void test_request_does_not_eat_stream(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    ipc_message_t reply;
    zmpio_v2_stream_stats_t stats;
    zmpio_status_t status;
    unsigned i;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);

    cpu1_publish_feature(handle, 1U, 100U, 0U);
    cpu1_publish_feature(handle, 2U, 101U, 0U);
    cpu1_publish_feature(handle, 3U, 102U, 0U);
    cpu1_publish_ack(handle, 1U, (uint32_t)MSG_TYPE_HEARTBEAT);
    cpu1_publish_feature(handle, 4U, 103U, 0U);
    cpu1_publish_feature(handle, 5U, 104U, 0U);

    status = zmpio_v2_request(handle, MSG_TYPE_HEARTBEAT, NULL, 0U, 500U,
                              &reply);
    CHECK(status == ZMPIO_OK, "request status=%d", (int)status);
    CHECK(reply.header.type == (uint32_t)MSG_TYPE_ACK, "reply type=%u",
          reply.header.type);

    /* The two frames published after the ack are still in the ring at this
     * point -- the request stopped as soon as it matched.  Drain them. */
    CHECK(zmpio_v2_drain(handle, 0U, NULL) == ZMPIO_OK, "drain failed");

    CHECK(log.count == 5U, "sink got %u messages, expected 5", log.count);
    for (i = 0U; i < log.count; ++i) {
        CHECK(log.sequences[i] == i + 1U, "message %u has stream_seq %u", i,
              log.sequences[i]);
        CHECK(log.frames[i] == 100U + i, "message %u has frame %u", i,
              log.frames[i]);
    }

    zmpio_v2_get_stream_stats(handle, &stats);
    CHECK(stats.stream_gaps == 0U, "unexpected stream_gaps=%llu",
          (unsigned long long)stats.stream_gaps);
    CHECK(stats.feature_messages == 5U, "feature_messages=%llu",
          (unsigned long long)stats.feature_messages);
    free(handle);
}

/* A CPU1-side loss shows up as dropped_since_last, NOT as a stream_sequence
 * gap -- that is the whole point of reserving the sequence only on a
 * successful send (SDD_15 Sec.6). */
static void test_reported_drops_are_not_gaps(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    zmpio_v2_stream_stats_t stats;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);

    cpu1_publish_feature(handle, 1U, 10U, 0U);
    /* CPU1 dropped frames 11 and 12: sequence still advances by one, and
     * the loss is described in-band. */
    cpu1_publish_feature(handle, 2U, 13U, 2U);

    CHECK(zmpio_v2_drain(handle, 0U, NULL) == ZMPIO_OK, "drain failed");
    zmpio_v2_get_stream_stats(handle, &stats);
    CHECK(stats.stream_gaps == 0U, "stream_gaps=%llu (should be 0)",
          (unsigned long long)stats.stream_gaps);
    CHECK(stats.reported_drops == 2U, "reported_drops=%llu",
          (unsigned long long)stats.reported_drops);
    free(handle);
}

/* A gap in stream_sequence means loss AFTER CPU1 published -- with one
 * reader it must never happen, so it gets counted loudly. */
static void test_sequence_gap_is_detected(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    zmpio_v2_stream_stats_t stats;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);

    cpu1_publish_feature(handle, 1U, 10U, 0U);
    cpu1_publish_feature(handle, 5U, 14U, 0U); /* 2,3,4 vanished */

    CHECK(zmpio_v2_drain(handle, 0U, NULL) == ZMPIO_OK, "drain failed");
    zmpio_v2_get_stream_stats(handle, &stats);
    CHECK(stats.stream_gaps == 1U, "stream_gaps=%llu",
          (unsigned long long)stats.stream_gaps);
    CHECK(stats.stream_gap_messages == 3U, "stream_gap_messages=%llu",
          (unsigned long long)stats.stream_gap_messages);
    free(handle);
}

/* Malformed and wrong-length messages are discarded and counted, and must
 * not wedge the ring for the good messages behind them. */
static void test_malformed_is_counted_not_fatal(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    zmpio_v2_stream_stats_t stats;
    ipc_message_t bad;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);

    memset(&bad, 0, sizeof(bad));
    bad.header.magic = 0xDEADBEEFU;
    bad.header.type = (uint32_t)MSG_TYPE_FEATURE_V2;
    bad.header.length = (uint32_t)sizeof(ipc_feature_stream_v1_t);
    cpu1_publish(handle, &bad);

    /* Right magic and type, wrong length: an ABI drift, which must be
     * refused rather than decoded at the wrong offsets. */
    memset(&bad, 0, sizeof(bad));
    bad.header.magic = IPC_MAGIC;
    bad.header.type = (uint32_t)MSG_TYPE_FEATURE_V2;
    bad.header.length = 40U;
    cpu1_publish(handle, &bad);

    cpu1_publish_feature(handle, 1U, 10U, 0U);
    cpu1_publish_health(handle, 2U, 1U);

    CHECK(zmpio_v2_drain(handle, 0U, NULL) == ZMPIO_OK, "drain failed");
    zmpio_v2_get_stream_stats(handle, &stats);
    CHECK(stats.malformed == 2U, "malformed=%llu",
          (unsigned long long)stats.malformed);
    CHECK(log.count == 2U, "sink got %u messages, expected 2", log.count);
    CHECK(log.types[0] == (uint32_t)MSG_TYPE_FEATURE_V2, "types[0]=%u",
          log.types[0]);
    CHECK(log.types[1] == (uint32_t)MSG_TYPE_DSP_HEALTH, "types[1]=%u",
          log.types[1]);
    CHECK(stats.stream_gaps == 0U, "stream_gaps=%llu",
          (unsigned long long)stats.stream_gaps);
    free(handle);
}

/*
 * A reply drained before its caller asked for it is parked, not dropped:
 * this is what stops a reply that arrives during someone else's drain from
 * turning into a spurious timeout.
 */
static void test_orphan_reply_is_kept(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    ipc_message_t reply;
    zmpio_v2_stream_stats_t stats;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);

    /* request id 1 will be issued below; its reply shows up during an
     * unrelated drain first. */
    cpu1_publish_ack(handle, 1U, (uint32_t)MSG_TYPE_HEARTBEAT);
    CHECK(zmpio_v2_drain(handle, 0U, NULL) == ZMPIO_OK, "drain failed");

    zmpio_v2_get_stream_stats(handle, &stats);
    CHECK(stats.orphan_queued == 1U, "orphan_queued=%llu",
          (unsigned long long)stats.orphan_queued);

    CHECK(zmpio_v2_request(handle, MSG_TYPE_HEARTBEAT, NULL, 0U, 100U,
                           &reply) == ZMPIO_OK,
          "request did not find the parked reply");
    CHECK(reply.header.timestamp == 1U, "reply id=%u",
          reply.header.timestamp);

    zmpio_v2_get_stream_stats(handle, &stats);
    CHECK(stats.orphan_overflow == 0U, "orphan_overflow=%llu",
          (unsigned long long)stats.orphan_overflow);
    free(handle);
}

/* Sending a stream type as a request would wait out the full timeout for a
 * reply CPU1 never produces. */
static void test_stream_type_rejected_as_request(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    ipc_message_t reply;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);
    CHECK(zmpio_v2_request(handle, MSG_TYPE_FEATURE_V2, NULL, 0U, 10U,
                           &reply) == ZMPIO_ERR_INVALID_ARG,
          "stream type accepted as a request");
    free(handle);
}

/* A full ring must drain completely -- 255 usable slots, then empty. */
static void test_full_ring_drains(void)
{
    sink_log_t log;
    zmpio_handle_t *handle;
    uint32_t drained = 0U;
    unsigned i;

    memset(&log, 0, sizeof(log));
    handle = make_handle(&log);

    for (i = 0U; i < IPC_BUFFER_SIZE - 1U; ++i) {
        cpu1_publish_feature(handle, i + 1U, 1000U + i, 0U);
    }
    CHECK(zmpio_v2_drain(handle, 0U, &drained) == ZMPIO_OK, "drain failed");
    CHECK(drained == IPC_BUFFER_SIZE - 1U, "drained %u of %u", drained,
          IPC_BUFFER_SIZE - 1U);
    CHECK(TX_CTRL(handle)->head == TX_CTRL(handle)->tail,
          "ring not empty after drain");
    free(handle);
}

int main(void)
{
    g_shared = (uint8_t *)calloc(1U, SHARED_MEM_SIZE);
    if (g_shared == NULL) {
        printf("FAIL: cannot allocate the fake shared window\n");
        return 1;
    }

    test_request_does_not_eat_stream();
    test_reported_drops_are_not_gaps();
    test_sequence_gap_is_detected();
    test_malformed_is_counted_not_fatal();
    test_orphan_reply_is_kept();
    test_stream_type_rejected_as_request();
    test_full_ring_drains();

    free(g_shared);
    if (g_failures != 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("zmpio_v2 demux: all checks passed\n");
    return 0;
}
