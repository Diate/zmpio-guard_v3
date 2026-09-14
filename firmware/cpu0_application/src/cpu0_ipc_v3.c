#include "cpu0_ipc_v3.h"

#include <stddef.h>
#include <string.h>

#include "sleep.h"
#include "xil_printf.h"
#include "zmpio_crc32.h"

#define CPU0_IPC_V3_POLL_DELAY_US 1000U

static volatile zmpio_v3_control_t *const v3_control =
    (volatile zmpio_v3_control_t *)ZMPIO_ABI_V3_CONTROL_BASE;
static volatile zmpio_v3_cmd_slot_t *const v3_cmd_ring =
    (volatile zmpio_v3_cmd_slot_t *)ZMPIO_ABI_V3_CMD_RING_BASE;
static volatile zmpio_v3_rsp_slot_t *const v3_rsp_ring =
    (volatile zmpio_v3_rsp_slot_t *)ZMPIO_ABI_V3_RSP_RING_BASE;

static uint32_t next_correlation_id = 1U;
static zmpio_v3_cmd_slot_t last_sent_command;
static bool last_sent_command_valid;

static void memory_barrier(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

static uint32_t compute_crc32_with_zeroed_field(const void *slot, size_t slot_size,
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
    cmd->crc32 = compute_crc32_with_zeroed_field(cmd, sizeof(*cmd),
                                                 offsetof(zmpio_v3_cmd_slot_t, crc32));
}

static bool response_crc_is_valid(const zmpio_v3_rsp_slot_t *response)
{
    uint32_t expected = compute_crc32_with_zeroed_field(
        response, sizeof(*response), offsetof(zmpio_v3_rsp_slot_t, crc32));
    return expected == response->crc32;
}

int cpu0_ipc_v3_wait_for_cpu1_ready(uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms) {
        memory_barrier();
        if ((v3_control->magic == ZMPIO_ABI_V3_MAGIC) &&
            (v3_control->cpu1_link_state != (uint32_t)ZMPIO_V3_LINK_CPU1_NOT_READY)) {
            return CPU0_IPC_V3_OK;
        }
        usleep(CPU0_IPC_V3_POLL_DELAY_US);
        ++elapsed_ms;
    }
    return CPU0_IPC_V3_TIMEOUT;
}

/* Raw push, no correlation_id bookkeeping -- used both by the normal send
 * path and by the corrupt-slot lab test, which needs to sign a command and
 * then deliberately break its CRC afterward. */
static int push_command(const zmpio_v3_cmd_slot_t *cmd)
{
    uint32_t current_head;
    uint32_t next_head;

    memory_barrier();
    if (v3_control->magic != ZMPIO_ABI_V3_MAGIC) {
        return CPU0_IPC_V3_NOT_READY;
    }

    current_head = v3_control->cmd_head;
    next_head = (current_head + 1U) % v3_control->cmd_ring_size;
    if (next_head == v3_control->cmd_tail) {
        return CPU0_IPC_V3_RING_FULL;
    }

    memcpy((void *)&v3_cmd_ring[current_head], cmd, sizeof(*cmd));
    memory_barrier();
    v3_control->cmd_head = next_head;
    memory_barrier();
    return CPU0_IPC_V3_OK;
}

/* Polls the response ring until a reply with a valid CRC and a matching
 * correlation_id shows up, or timeout_ms elapses. A CRC-invalid reply is
 * counted and skipped rather than treated as a match or a fatal error --
 * matching the same "must not stall" rule ipc_v3.c applies to bad commands,
 * just on the response side. A reply for a stale correlation_id (e.g. a
 * response that arrived after this call already gave up on it) is discarded
 * the same way cpu0_ipc_wait_for_reply() discards unmatched ABI v2 replies. */
static int wait_for_response(uint32_t correlation_id, zmpio_v3_rsp_slot_t *out,
                             uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms) {
        uint32_t current_tail;
        zmpio_v3_rsp_slot_t candidate;

        memory_barrier();
        if (v3_control->rsp_head == v3_control->rsp_tail) {
            usleep(CPU0_IPC_V3_POLL_DELAY_US);
            ++elapsed_ms;
            continue;
        }

        current_tail = v3_control->rsp_tail;
        memcpy(&candidate, (const void *)&v3_rsp_ring[current_tail], sizeof(candidate));
        memory_barrier();
        v3_control->rsp_tail = (current_tail + 1U) % v3_control->rsp_ring_size;
        memory_barrier();

        if (!response_crc_is_valid(&candidate)) {
            v3_control->cpu0_crc_drop_count++;
            memory_barrier();
            xil_printf("CPU0: ABI v3 response CRC mismatch, dropped (total=%lu)\r\n",
                       (unsigned long)v3_control->cpu0_crc_drop_count);
            /* Counts toward the timeout, same discipline cpu0_ipc_wait_for_reply()
             * (ABI v2) documents: a stream of bad/unmatched traffic must not
             * hang this call forever. */
            ++elapsed_ms;
            continue;
        }
        if (candidate.correlation_id != correlation_id) {
            xil_printf("CPU0: ABI v3 discarded unmatched reply corr=%lu (want %lu)\r\n",
                       (unsigned long)candidate.correlation_id,
                       (unsigned long)correlation_id);
            ++elapsed_ms;
            continue;
        }

        *out = candidate;
        return CPU0_IPC_V3_OK;
    }
    return CPU0_IPC_V3_TIMEOUT;
}

static int send_with_retry(zmpio_v3_cmd_slot_t *cmd, uint32_t max_attempts,
                           uint32_t timeout_ms, zmpio_v3_rsp_slot_t *out_response)
{
    uint32_t attempt;

    if (max_attempts == 0U) {
        max_attempts = 1U;
    }

    for (attempt = 0U; attempt < max_attempts; ++attempt) {
        int status;

        /* Every attempt, including retries, gets a fresh correlation_id --
         * a timeout retry is always a new command, never a resend of the old
         * one (this is what makes CPU1's duplicate-suppression cache never
         * see two attempts of the same logical retry as "the same
         * command"). */
        cmd->correlation_id = next_correlation_id++;
        sign_command(cmd);
        last_sent_command = *cmd;
        last_sent_command_valid = true;

        status = push_command(cmd);
        if (status != CPU0_IPC_V3_OK) {
            return status;
        }

        status = wait_for_response(cmd->correlation_id, out_response, timeout_ms);
        if (status == CPU0_IPC_V3_OK) {
            return CPU0_IPC_V3_OK;
        }
    }
    return CPU0_IPC_V3_TIMEOUT;
}

int cpu0_ipc_v3_hello(uint32_t max_attempts, uint32_t timeout_ms,
                     cpu0_ipc_v3_link_t *out_link)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    int status;

    memset(out_link, 0, sizeof(*out_link));
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_HELLO;
    cmd.session_id = 0U; /* meaningless for HELLO -- CPU0 does not know it yet */
    cmd.payload_len = 0U;

    status = send_with_retry(&cmd, max_attempts, timeout_ms, &response);
    if (status != CPU0_IPC_V3_OK) {
        return status;
    }
    if ((response.type != (uint32_t)ZMPIO_V3_RSP_HELLO_ACK) ||
        (response.payload_len != sizeof(uint32_t))) {
        return CPU0_IPC_V3_TIMEOUT;
    }

    out_link->session_id = response.session_id;
    memcpy(&out_link->remote_layout_hash, response.payload, sizeof(uint32_t));

    if (out_link->remote_layout_hash != ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH) {
        xil_printf("CPU0: ABI v3 layout_hash MISMATCH local=0x%08lx remote=0x%08lx\r\n",
                   (unsigned long)ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH,
                   (unsigned long)out_link->remote_layout_hash);
        out_link->online = false;
        return CPU0_IPC_V3_LAYOUT_MISMATCH;
    }

    out_link->online = true;
    xil_printf("CPU0: ABI v3 ONLINE session_id=0x%08lx layout_hash=0x%08lx\r\n",
               (unsigned long)out_link->session_id,
               (unsigned long)out_link->remote_layout_hash);
    return CPU0_IPC_V3_OK;
}

int cpu0_ipc_v3_set_dsp_config(const cpu0_ipc_v3_link_t *link,
                               const zmpio_v3_set_dsp_config_t *config,
                               uint32_t max_attempts, uint32_t timeout_ms,
                               int32_t *out_status)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    int status;

    if (!link->online) {
        return CPU0_IPC_V3_NOT_READY;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_SET_DSP_CONFIG;
    cmd.session_id = link->session_id;
    cmd.payload_len = sizeof(*config);
    memcpy(cmd.payload, config, sizeof(*config));

    status = send_with_retry(&cmd, max_attempts, timeout_ms, &response);
    if (status != CPU0_IPC_V3_OK) {
        return status;
    }

    *out_status = response.status;
    xil_printf("CPU0: ABI v3 SET_DSP_CONFIG %s status=%ld\r\n",
               response.type == (uint32_t)ZMPIO_V3_RSP_CONFIG_ACK ? "ACK" : "NACK",
               (long)response.status);
    return CPU0_IPC_V3_OK;
}

int cpu0_ipc_v3_dsp_soft_reset(const cpu0_ipc_v3_link_t *link,
                               uint32_t max_attempts, uint32_t timeout_ms,
                               zmpio_v3_dsp_soft_reset_ack_t *out_ack)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    int status;

    if (!link->online) {
        return CPU0_IPC_V3_NOT_READY;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_DSP_SOFT_RESET;
    cmd.session_id = link->session_id;
    cmd.payload_len = 0U;

    status = send_with_retry(&cmd, max_attempts, timeout_ms, &response);
    if (status != CPU0_IPC_V3_OK) {
        return status;
    }

    if ((response.type != (uint32_t)ZMPIO_V3_RSP_DSP_SOFT_RESET_ACK) ||
        (response.payload_len != sizeof(*out_ack))) {
        xil_printf("CPU0: ABI v3 DSP_SOFT_RESET NACK status=%ld\r\n",
                   (long)response.status);
        return CPU0_IPC_V3_NOT_READY;
    }

    memcpy(out_ack, response.payload, sizeof(*out_ack));
    xil_printf("CPU0: ABI v3 DSP_SOFT_RESET ACK feature_count_before=%lu drop_count_before=%lu\r\n",
               (unsigned long)out_ack->feature_count_before,
               (unsigned long)out_ack->drop_count_before);
    return CPU0_IPC_V3_OK;
}

int cpu0_ipc_v3_fifo_full_inject(const cpu0_ipc_v3_link_t *link, uint32_t hold_ms,
                                 uint32_t max_attempts, uint32_t timeout_ms,
                                 zmpio_v3_fifo_full_inject_ack_t *out_ack)
{
    zmpio_v3_cmd_slot_t cmd;
    zmpio_v3_rsp_slot_t response;
    zmpio_v3_fifo_full_inject_cmd_t request;
    int status;

    if (!link->online) {
        return CPU0_IPC_V3_NOT_READY;
    }

    memset(&request, 0, sizeof(request));
    request.hold_ms = hold_ms;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_FIFO_FULL_INJECT;
    cmd.session_id = link->session_id;
    cmd.payload_len = sizeof(request);
    memcpy(cmd.payload, &request, sizeof(request));

    status = send_with_retry(&cmd, max_attempts, timeout_ms, &response);
    if (status != CPU0_IPC_V3_OK) {
        return status;
    }

    if ((response.type != (uint32_t)ZMPIO_V3_RSP_FIFO_FULL_INJECT_ACK) ||
        (response.payload_len != sizeof(*out_ack))) {
        xil_printf("CPU0: ABI v3 FIFO_FULL_INJECT NACK status=%ld\r\n",
                   (long)response.status);
        return CPU0_IPC_V3_NOT_READY;
    }

    memcpy(out_ack, response.payload, sizeof(*out_ack));
    xil_printf("CPU0: ABI v3 FIFO_FULL_INJECT ACK feature_count_before=%lu "
               "drop_count_before=%lu hold_ms_applied=%lu\r\n",
               (unsigned long)out_ack->feature_count_before,
               (unsigned long)out_ack->drop_count_before,
               (unsigned long)out_ack->hold_ms_applied);
    return CPU0_IPC_V3_OK;
}

int cpu0_ipc_v3_debug_send_corrupt_command(void)
{
    zmpio_v3_cmd_slot_t cmd;
    int status;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint32_t)ZMPIO_V3_CMD_HELLO;
    cmd.correlation_id = next_correlation_id++;
    sign_command(&cmd);
    /* Break the CRC deliberately, after signing, so the rest of the slot is
     * otherwise a well-formed HELLO. */
    cmd.crc32 ^= 0xFFFFFFFFU;

    status = push_command(&cmd);
    if (status == CPU0_IPC_V3_OK) {
        xil_printf("CPU0: ABI v3 lab test -- pushed HELLO corr=%lu with corrupt CRC\r\n",
                   (unsigned long)cmd.correlation_id);
    }
    return status;
}

int cpu0_ipc_v3_debug_resend_last(cpu0_ipc_v3_link_t *out_link, int32_t *out_status)
{
    zmpio_v3_rsp_slot_t response;
    int status;

    if (!last_sent_command_valid) {
        xil_printf("CPU0: ABI v3 lab test -- no previous command to resend\r\n");
        return CPU0_IPC_V3_NOT_READY;
    }

    xil_printf("CPU0: ABI v3 lab test -- resending corr=%lu unchanged (duplicate)\r\n",
               (unsigned long)last_sent_command.correlation_id);
    status = push_command(&last_sent_command);
    if (status != CPU0_IPC_V3_OK) {
        return status;
    }

    status = wait_for_response(last_sent_command.correlation_id, &response, 2000U);
    if (status != CPU0_IPC_V3_OK) {
        return status;
    }

    if (response.type == (uint32_t)ZMPIO_V3_RSP_HELLO_ACK) {
        memset(out_link, 0, sizeof(*out_link));
        out_link->session_id = response.session_id;
        memcpy(&out_link->remote_layout_hash, response.payload, sizeof(uint32_t));
        out_link->online = (out_link->remote_layout_hash == ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH);
    } else if (out_status != NULL) {
        *out_status = response.status;
    }
    xil_printf("CPU0: ABI v3 lab test -- duplicate reply type=%lu status=%ld "
               "(compare CONFIG_SEQ/session_id against the original reply by hand)\r\n",
               (unsigned long)response.type, (long)response.status);
    return CPU0_IPC_V3_OK;
}

uint32_t cpu0_ipc_v3_debug_wrap_stress(uint32_t count, uint32_t timeout_ms)
{
    uint32_t accepted = 0U;
    uint32_t index;

    xil_printf("CPU0: ABI v3 lab test -- firing %lu back-to-back HELLOs "
               "(ring size %u)\r\n",
               (unsigned long)count, ZMPIO_ABI_V3_CMD_SLOTS);
    for (index = 0U; index < count; ++index) {
        cpu0_ipc_v3_link_t link;

        if (cpu0_ipc_v3_hello(1U, timeout_ms, &link) == CPU0_IPC_V3_OK) {
            ++accepted;
        }
    }
    xil_printf("CPU0: ABI v3 lab test -- wrap stress %lu/%lu accepted\r\n",
               (unsigned long)accepted, (unsigned long)count);
    return accepted;
}

void cpu0_ipc_v3_get_counters(uint32_t *out_cpu1_crc_drop_count,
                              uint32_t *out_cpu0_crc_drop_count)
{
    memory_barrier();
    *out_cpu1_crc_drop_count = v3_control->cpu1_crc_drop_count;
    *out_cpu0_crc_drop_count = v3_control->cpu0_crc_drop_count;
}
