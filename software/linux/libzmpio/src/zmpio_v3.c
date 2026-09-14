/* clock_gettime()/CLOCK_MONOTONIC and nanosleep() (glibc feature-test
 * gate) -- must be defined before any system header is pulled in
 * transitively by libzmpio.h/zmpio_internal.h below, not just before
 * <time.h>'s own #include. */
#define _GNU_SOURCE

#include "libzmpio.h"
#include "zmpio_internal.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "zmpio_crc32.h"

/*
 * Direct Linux port of firmware/cpu0_application/src/cpu0_ipc_v3.c -- same
 * ring/CRC/correlation_id/retry discipline, byte for byte where the
 * platform allows it. Kept deliberately parallel (not "improved") so a
 * board-side ABI v3 bug and a libzmpio bug are never confused with each
 * other: if cpu0_ipc_v3.c passes a gate and zmpio_v3.c does not (or vice
 * versa), the difference is the platform, not the protocol logic.
 *
 * The one deliberate deviation: next_v3_correlation_id is seeded from a
 * clock reading at zmpio_open() (see below) instead of always starting at
 * 1 -- cpu0_ipc_v3.c can hardcode 1 because a bare-metal CPU0 test build
 * only ever runs once per power cycle, but zmpiod is expected to restart
 * under kill -9 while CPU1 keeps running, and session_id must stay
 * unchanged across such a restart. Starting a fresh 1 after every restart
 * risks colliding with a correlation_id from just before the restart if a
 * stale response for it is still sitting unconsumed in the rsp ring.
 * cmd_head/rsp_tail themselves need no such seeding -- they live IN the
 * shared control block (zmpio_v3_control_t), not in this process, so
 * push_command()/wait_for_response() below read them fresh from shared
 * memory on every call exactly like cpu0_ipc_v3.c does; a restart
 * continues the ring from wherever it was, automatically.
 */

/* See the identical comment in zmpio_v2.c -- same reasoning applies to
 * every ABI v3 ring/control-block field, all of which also live in the
 * zmpio-shm DDR mapping. */
static void zmpio_memory_barrier(void)
{
    __sync_synchronize();
}

#define ZMPIO_V3_CONTROL(h) \
    ((volatile zmpio_v3_control_t *)ZMPIO_SHM_PTR(h, ZMPIO_ABI_V3_CONTROL_BASE))
#define ZMPIO_V3_CMD_RING(h) \
    ((volatile zmpio_v3_cmd_slot_t *)ZMPIO_SHM_PTR(h, ZMPIO_ABI_V3_CMD_RING_BASE))
#define ZMPIO_V3_RSP_RING(h) \
    ((volatile zmpio_v3_rsp_slot_t *)ZMPIO_SHM_PTR(h, ZMPIO_ABI_V3_RSP_RING_BASE))

static uint32_t compute_crc32_with_zeroed_field(const void *slot,
                                                size_t slot_size,
                                                size_t crc_field_offset)
{
    uint8_t scratch[128];

    memcpy(scratch, slot, slot_size);
    memset(&scratch[crc_field_offset], 0, sizeof(uint32_t));
    return zmpio_crc32(scratch, slot_size);
}

static void sign_command(zmpio_v3_cmd_slot_t *cmd)
{
    cmd->crc32 = 0U;
    cmd->crc32 = compute_crc32_with_zeroed_field(
        cmd, sizeof(*cmd), offsetof(zmpio_v3_cmd_slot_t, crc32));
}

static int response_crc_is_valid(const zmpio_v3_rsp_slot_t *response)
{
    uint32_t expected = compute_crc32_with_zeroed_field(
        response, sizeof(*response), offsetof(zmpio_v3_rsp_slot_t, crc32));
    return expected == response->crc32;
}

/* Seed for next_v3_correlation_id, called once from zmpio_open(). Truncated
 * microsecond timestamp, same non-cryptographic "differs from last boot
 * with overwhelming probability" reasoning ipc_v3_init() uses for
 * session_id (firmware/app_freertos/src/ipc_v3.c) -- never 0, since 0 would
 * be indistinguishable from an unsent command in a zeroed slot. */
uint32_t zmpio_v3_seed_correlation_id(void)
{
    struct timespec ts;
    uint32_t seed;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seed = (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL +
                      (uint64_t)(ts.tv_nsec / 1000));
    return (seed == 0U) ? 1U : seed;
}

zmpio_status_t zmpio_v3_wait_cpu1_ready(zmpio_handle_t *handle,
                                        uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;
    struct timespec one_ms = {0, 1000000L};
    volatile zmpio_v3_control_t *control;

    if (handle == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    control = ZMPIO_V3_CONTROL(handle);

    while (elapsed_ms < timeout_ms) {
        zmpio_memory_barrier();
        if ((control->magic == ZMPIO_ABI_V3_MAGIC) &&
            (control->cpu1_link_state !=
             (uint32_t)ZMPIO_V3_LINK_CPU1_NOT_READY)) {
            return ZMPIO_OK;
        }
        nanosleep(&one_ms, NULL);
        ++elapsed_ms;
    }
    return ZMPIO_ERR_TIMEOUT;
}

/* Returns 0 OK, -1 not ready (bad magic), -2 ring full. */
static int push_command(zmpio_handle_t *handle, const zmpio_v3_cmd_slot_t *cmd)
{
    uint32_t current_head;
    uint32_t next_head;
    volatile zmpio_v3_control_t *control = ZMPIO_V3_CONTROL(handle);

    zmpio_memory_barrier();
    if (control->magic != ZMPIO_ABI_V3_MAGIC) {
        return -1;
    }

    current_head = control->cmd_head;
    next_head = (current_head + 1U) % control->cmd_ring_size;
    if (next_head == control->cmd_tail) {
        return -2;
    }

    memcpy((void *)&ZMPIO_V3_CMD_RING(handle)[current_head], cmd, sizeof(*cmd));
    zmpio_memory_barrier();
    control->cmd_head = next_head;
    zmpio_memory_barrier();
    return 0;
}

static zmpio_status_t wait_for_response(zmpio_handle_t *handle,
                                        uint32_t correlation_id,
                                        zmpio_v3_rsp_slot_t *out,
                                        uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;
    struct timespec one_ms = {0, 1000000L};
    volatile zmpio_v3_control_t *control = ZMPIO_V3_CONTROL(handle);

    while (elapsed_ms < timeout_ms) {
        uint32_t current_tail;
        zmpio_v3_rsp_slot_t candidate;

        zmpio_memory_barrier();
        if (control->rsp_head == control->rsp_tail) {
            nanosleep(&one_ms, NULL);
            ++elapsed_ms;
            continue;
        }

        current_tail = control->rsp_tail;
        memcpy(&candidate, (const void *)&ZMPIO_V3_RSP_RING(handle)[current_tail],
               sizeof(candidate));
        zmpio_memory_barrier();
        control->rsp_tail = (current_tail + 1U) % control->rsp_ring_size;
        zmpio_memory_barrier();

        if (!response_crc_is_valid(&candidate)) {
            control->cpu0_crc_drop_count++;
            zmpio_memory_barrier();
            ++elapsed_ms;
            continue;
        }
        if (candidate.correlation_id != correlation_id) {
            ++elapsed_ms;
            continue;
        }

        *out = candidate;
        return ZMPIO_OK;
    }
    return ZMPIO_ERR_TIMEOUT;
}

static zmpio_status_t send_with_retry(zmpio_handle_t *handle,
                                      zmpio_v3_cmd_slot_t *cmd,
                                      uint32_t max_attempts,
                                      uint32_t timeout_ms,
                                      zmpio_v3_rsp_slot_t *out_response)
{
    uint32_t attempt;

    if (max_attempts == 0U) {
        max_attempts = 1U;
    }

    for (attempt = 0U; attempt < max_attempts; ++attempt) {
        int push_result;
        zmpio_status_t status;

        /* Every attempt, including retries, gets a fresh correlation_id --
         * a timeout retry is a new command, never a resend of the old one
         * (matches cpu0_ipc_v3.c / SDD_05's rule). */
        cmd->correlation_id = handle->next_v3_correlation_id++;
        sign_command(cmd);
        handle->last_v3_command = *cmd;
        handle->last_v3_command_valid = 1;

        push_result = push_command(handle, cmd);
        if (push_result == -1) {
            return ZMPIO_ERR_NOT_READY;
        }
        if (push_result == -2) {
            return ZMPIO_ERR_RING_FULL;
        }

        status = wait_for_response(handle, cmd->correlation_id, out_response,
                                   timeout_ms);
        if (status == ZMPIO_OK) {
            return ZMPIO_OK;
        }
    }
    return ZMPIO_ERR_TIMEOUT;
}

zmpio_status_t zmpio_v3_hello(zmpio_handle_t *handle, uint32_t max_attempts,
                              uint32_t timeout_ms, zmpio_v3_link_t *out_link)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    zmpio_status_t status;

    if ((handle == NULL) || (out_link == NULL)) {
        return ZMPIO_ERR_INVALID_ARG;
    }

    memset(out_link, 0, sizeof(*out_link));
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_HELLO;
    cmd.session_id = 0U; /* meaningless for HELLO -- not known yet */
    cmd.payload_len = 0U;

    status = send_with_retry(handle, &cmd, max_attempts, timeout_ms, &response);
    if (status != ZMPIO_OK) {
        return status;
    }
    if ((response.type != (uint32_t)ZMPIO_V3_RSP_HELLO_ACK) ||
        (response.payload_len != sizeof(uint32_t))) {
        return ZMPIO_ERR_TIMEOUT;
    }

    out_link->session_id = response.session_id;
    memcpy(&out_link->remote_layout_hash, response.payload, sizeof(uint32_t));

    if (out_link->remote_layout_hash != ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH) {
        out_link->online = false;
        return ZMPIO_ERR_LAYOUT_MISMATCH;
    }
    out_link->online = true;
    return ZMPIO_OK;
}

zmpio_status_t zmpio_v3_set_dsp_config(zmpio_handle_t *handle,
                                       const zmpio_v3_link_t *link,
                                       const zmpio_v3_set_dsp_config_t *config,
                                       uint32_t max_attempts,
                                       uint32_t timeout_ms,
                                       int32_t *out_status)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    zmpio_status_t status;

    if ((handle == NULL) || (link == NULL) || (config == NULL) ||
        (out_status == NULL)) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if (!link->online) {
        return ZMPIO_ERR_NOT_READY;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_SET_DSP_CONFIG;
    cmd.session_id = link->session_id;
    cmd.payload_len = sizeof(*config);
    memcpy(cmd.payload, config, sizeof(*config));

    status = send_with_retry(handle, &cmd, max_attempts, timeout_ms, &response);
    if (status != ZMPIO_OK) {
        return status;
    }
    *out_status = response.status;
    return ZMPIO_OK;
}

zmpio_status_t zmpio_v3_dsp_soft_reset(zmpio_handle_t *handle,
                                       const zmpio_v3_link_t *link,
                                       uint32_t max_attempts,
                                       uint32_t timeout_ms,
                                       zmpio_v3_dsp_soft_reset_ack_t *out_ack)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    zmpio_status_t status;

    if ((handle == NULL) || (link == NULL) || (out_ack == NULL)) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if (!link->online) {
        return ZMPIO_ERR_NOT_READY;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_DSP_SOFT_RESET;
    cmd.session_id = link->session_id;
    cmd.payload_len = 0U;

    status = send_with_retry(handle, &cmd, max_attempts, timeout_ms, &response);
    if (status != ZMPIO_OK) {
        return status;
    }
    if ((response.type != (uint32_t)ZMPIO_V3_RSP_DSP_SOFT_RESET_ACK) ||
        (response.payload_len != sizeof(*out_ack))) {
        return ZMPIO_ERR_NOT_READY;
    }
    memcpy(out_ack, response.payload, sizeof(*out_ack));
    return ZMPIO_OK;
}

zmpio_status_t zmpio_v3_fifo_full_inject(zmpio_handle_t *handle,
                                         const zmpio_v3_link_t *link,
                                         uint32_t hold_ms,
                                         uint32_t max_attempts,
                                         uint32_t timeout_ms,
                                         zmpio_v3_fifo_full_inject_ack_t *out_ack)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    zmpio_v3_fifo_full_inject_cmd_t request;
    zmpio_status_t status;

    if ((handle == NULL) || (link == NULL) || (out_ack == NULL)) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    if (!link->online) {
        return ZMPIO_ERR_NOT_READY;
    }

    memset(&request, 0, sizeof(request));
    request.hold_ms = hold_ms;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_FIFO_FULL_INJECT;
    cmd.session_id = link->session_id;
    cmd.payload_len = sizeof(request);
    memcpy(cmd.payload, &request, sizeof(request));

    status = send_with_retry(handle, &cmd, max_attempts, timeout_ms, &response);
    if (status != ZMPIO_OK) {
        return status;
    }
    if ((response.type != (uint32_t)ZMPIO_V3_RSP_FIFO_FULL_INJECT_ACK) ||
        (response.payload_len != sizeof(*out_ack))) {
        return ZMPIO_ERR_NOT_READY;
    }
    memcpy(out_ack, response.payload, sizeof(*out_ack));
    return ZMPIO_OK;
}

void zmpio_v3_get_counters(zmpio_handle_t *handle,
                           uint32_t *out_cpu1_crc_drop_count,
                           uint32_t *out_cpu0_crc_drop_count)
{
    volatile zmpio_v3_control_t *control;

    if (handle == NULL) {
        return;
    }
    control = ZMPIO_V3_CONTROL(handle);
    zmpio_memory_barrier();
    if (out_cpu1_crc_drop_count != NULL) {
        *out_cpu1_crc_drop_count = control->cpu1_crc_drop_count;
    }
    if (out_cpu0_crc_drop_count != NULL) {
        *out_cpu0_crc_drop_count = control->cpu0_crc_drop_count;
    }
}
