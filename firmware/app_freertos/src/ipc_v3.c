#include "ipc_v3.h"

#include <stddef.h>
#include <string.h>

#include "app_config.h"
#include "cpu1_log.h"
#include "fpga_dsp_hal.h"
#include "pl_doorbell.h"
#include "platform_time.h"
#include "zmpio_crc32.h"

static volatile zmpio_v3_control_t *const v3_control =
    (volatile zmpio_v3_control_t *)ZMPIO_ABI_V3_CONTROL_BASE;
static zmpio_v3_cmd_slot_t *const v3_cmd_ring =
    (zmpio_v3_cmd_slot_t *)ZMPIO_ABI_V3_CMD_RING_BASE;
static zmpio_v3_rsp_slot_t *const v3_rsp_ring =
    (zmpio_v3_rsp_slot_t *)ZMPIO_ABI_V3_RSP_RING_BASE;

/* Idempotency cache: a command replayed
 * with the SAME correlation_id as the last one this task actually processed
 * is answered from this cache instead of being re-validated/re-applied, so a
 * duplicate SET_DSP_CONFIG cannot bump CONFIG_SEQ twice. A NEW correlation_id
 * -- which is what a real CPU0 timeout-retry always uses -- is processed as a
 * brand new command, per the same pass gate's "retry" case. */
static uint32_t last_correlation_id;
static bool last_correlation_valid;
static zmpio_v3_rsp_slot_t last_response;

static void memory_barrier(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

_Static_assert(sizeof(zmpio_v3_cmd_slot_t) <= 128U && sizeof(zmpio_v3_rsp_slot_t) <= 128U,
               "compute_crc32_with_zeroed_field's scratch buffer is too small");

static uint32_t compute_crc32_with_zeroed_field(const void *slot, size_t slot_size,
                                                size_t crc_field_offset)
{
    uint8_t scratch[128];

    memcpy(scratch, slot, slot_size);
    memset(&scratch[crc_field_offset], 0, sizeof(uint32_t));
    return zmpio_crc32(scratch, slot_size);
}

static void sign_response(zmpio_v3_rsp_slot_t *response)
{
    response->crc32 = 0U;
    response->crc32 = compute_crc32_with_zeroed_field(
        response, sizeof(*response), offsetof(zmpio_v3_rsp_slot_t, crc32));
}

static bool cmd_crc_is_valid(const zmpio_v3_cmd_slot_t *cmd)
{
    uint32_t expected = compute_crc32_with_zeroed_field(
        cmd, sizeof(*cmd), offsetof(zmpio_v3_cmd_slot_t, crc32));
    return expected == cmd->crc32;
}

int ipc_v3_init(void)
{
    memset((void *)v3_cmd_ring, 0, sizeof(zmpio_v3_cmd_slot_t) * ZMPIO_ABI_V3_CMD_SLOTS);
    memset((void *)v3_rsp_ring, 0, sizeof(zmpio_v3_rsp_slot_t) * ZMPIO_ABI_V3_RSP_SLOTS);
    memory_barrier();

    v3_control->cmd_head = 0U;
    v3_control->cmd_tail = 0U;
    v3_control->rsp_head = 0U;
    v3_control->rsp_tail = 0U;
    v3_control->cmd_ring_size = ZMPIO_ABI_V3_CMD_SLOTS;
    v3_control->rsp_ring_size = ZMPIO_ABI_V3_RSP_SLOTS;
    v3_control->cpu1_crc_drop_count = 0U;
    v3_control->cpu0_crc_drop_count = 0U;
    v3_control->abi_version = ZMPIO_ABI_V3_VERSION;
    v3_control->layout_hash = ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH;
    /* Truncated microsecond timestamp: not cryptographically unique, only
     * needs to differ from the previous boot's value with overwhelming
     * probability, which any nonzero uptime at the read guarantees. Kept
     * integer-only like the rest of this firmware (no float/double, per
     * CLAUDE.md Sec.6). */
    v3_control->session_id = (uint32_t)platform_time_us();
    memory_barrier();
    v3_control->cpu1_link_state = (uint32_t)ZMPIO_V3_LINK_CPU1_READY;
    memory_barrier();
    v3_control->magic = ZMPIO_ABI_V3_MAGIC;
    memory_barrier();

    last_correlation_valid = false;
    CPU1_LOG("CPU1: ABI v3 ready, session_id=0x%08lx layout_hash=0x%08lx\r\n",
             (unsigned long)v3_control->session_id,
             (unsigned long)v3_control->layout_hash);
    return 0;
}

static bool pop_command(zmpio_v3_cmd_slot_t *out)
{
    uint32_t current_tail;

    memory_barrier();
    if (v3_control->cmd_head == v3_control->cmd_tail) {
        return false;
    }

    current_tail = v3_control->cmd_tail;
    memory_barrier();
    memcpy(out, &v3_cmd_ring[current_tail], sizeof(*out));

    /* Advance the tail unconditionally, before CRC validation -- a corrupt
     * slot must free its place in the ring, never stall it: a CRC error must
     * never stall the ring. */
    v3_control->cmd_tail = (current_tail + 1U) % v3_control->cmd_ring_size;
    memory_barrier();
    return true;
}

static bool push_response(const zmpio_v3_rsp_slot_t *response)
{
    uint32_t current_head;
    uint32_t next_head;

    memory_barrier();

    current_head = v3_control->rsp_head;
    next_head = (current_head + 1U) % v3_control->rsp_ring_size;
    if (next_head == v3_control->rsp_tail) {
        CPU1_LOG("CPU1: ABI v3 response ring full, dropping reply\r\n");
        return false;
    }

    memcpy(&v3_rsp_ring[current_head], response, sizeof(*response));
    memory_barrier();

    v3_control->rsp_head = next_head;
    memory_barrier();

    /* Wake CPU0 instead of leaving it to
     * find this response only on its next poll -- additive, CPU0's existing
     * rsp_ring polling in cpu0_ipc_v3.c keeps working unchanged whether or
     * not it acts on this. */
    pl_doorbell_ring();
    return true;
}

static void build_hello_ack(const zmpio_v3_cmd_slot_t *cmd, zmpio_v3_rsp_slot_t *response)
{
    memset(response, 0, sizeof(*response));
    response->type = (uint32_t)ZMPIO_V3_RSP_HELLO_ACK;
    response->correlation_id = cmd->correlation_id;
    response->session_id = v3_control->session_id;
    response->status = 0;
    response->payload_len = sizeof(uint32_t);
    /* Echo this build's layout hash so CPU0 can compare it against its own
     * compiled-in value. The rejection on mismatch happens on the CPU0 side,
     * see cpu0_ipc_v3.c. */
    {
        uint32_t layout_hash = v3_control->layout_hash;
        memcpy(response->payload, &layout_hash, sizeof(layout_hash));
    }

    v3_control->cpu1_link_state = (uint32_t)ZMPIO_V3_LINK_ONLINE;
    memory_barrier();
}

static void build_config_response(const zmpio_v3_cmd_slot_t *cmd, zmpio_v3_rsp_slot_t *response)
{
    zmpio_v3_set_dsp_config_t config;
    zmpio_v3_config_status_t status;

    memset(response, 0, sizeof(*response));
    response->correlation_id = cmd->correlation_id;
    response->session_id = v3_control->session_id;
    response->payload_len = cmd->payload_len;
    if (cmd->payload_len == sizeof(config)) {
        memcpy(response->payload, cmd->payload, sizeof(config));
    }

    if (cmd->session_id != v3_control->session_id) {
        status = ZMPIO_V3_CONFIG_ERR_STALE_SESSION;
    } else if (cmd->payload_len != sizeof(config)) {
        status = ZMPIO_V3_CONFIG_ERR_UNKNOWN_COMMAND;
    } else {
        memcpy(&config, cmd->payload, sizeof(config));
        if (config.coeff_set_id != APP_ZMPIO_V3_COEFF_SET_ID_VALID) {
            status = ZMPIO_V3_CONFIG_ERR_BAD_COEFF_SET;
        } else if (config.fft_scale_shift > APP_ZMPIO_V3_FFT_SCALE_SHIFT_MAX) {
            status = ZMPIO_V3_CONFIG_ERR_BAD_FFT_SCALE;
        } else if ((config.feature_mask == 0U) ||
                   ((config.feature_mask & ~APP_ZMPIO_V3_FEATURE_MASK_VALID) != 0U)) {
            status = ZMPIO_V3_CONFIG_ERR_BAD_FEATURE_MASK;
        } else {
            status = ZMPIO_V3_CONFIG_OK;
        }
    }

    response->status = (int32_t)status;
    if (status == ZMPIO_V3_CONFIG_OK) {
        response->type = (uint32_t)ZMPIO_V3_RSP_CONFIG_ACK;
        /* The only hardware effect this command has today -- see the comment on
         * zmpio_v3_set_dsp_config_t in zmpio_abi_v3.h. */
        fpga_dsp_hal_bump_config_seq();
        CPU1_LOG("CPU1: ABI v3 SET_DSP_CONFIG applied corr=%lu coeff=%lu "
                 "scale=%lu mask=0x%03lx\r\n",
                 (unsigned long)cmd->correlation_id,
                 (unsigned long)config.coeff_set_id,
                 (unsigned long)config.fft_scale_shift,
                 (unsigned long)config.feature_mask);
    } else {
        response->type = (uint32_t)ZMPIO_V3_RSP_CONFIG_NACK;
        CPU1_LOG("CPU1: ABI v3 SET_DSP_CONFIG rejected corr=%lu status=%ld\r\n",
                 (unsigned long)cmd->correlation_id, (long)status);
    }
}

static void build_dsp_soft_reset_response(const zmpio_v3_cmd_slot_t *cmd, zmpio_v3_rsp_slot_t *response)
{
    zmpio_v3_dsp_soft_reset_ack_t ack;
    fpga_dsp_hal_counters_t counters;

    memset(response, 0, sizeof(*response));
    response->correlation_id = cmd->correlation_id;
    response->session_id = v3_control->session_id;

    if (cmd->session_id != v3_control->session_id) {
        response->type = (uint32_t)ZMPIO_V3_RSP_CONFIG_NACK;
        response->status = (int32_t)ZMPIO_V3_CONFIG_ERR_STALE_SESSION;
        CPU1_LOG("CPU1: ABI v3 DSP_SOFT_RESET rejected corr=%lu (stale session)\r\n",
                 (unsigned long)cmd->correlation_id);
        return;
    }

    fpga_dsp_hal_get_counters(&counters);
    ack.feature_count_before = counters.feature_count;
    ack.drop_count_before = counters.ctrl_drop_count;

    fpga_dsp_hal_soft_reset_pulse();

    response->type = (uint32_t)ZMPIO_V3_RSP_DSP_SOFT_RESET_ACK;
    response->status = 0;
    response->payload_len = sizeof(ack);
    memcpy(response->payload, &ack, sizeof(ack));
    CPU1_LOG("CPU1: ABI v3 DSP_SOFT_RESET applied corr=%lu feature_count_before=%lu "
             "drop_count_before=%lu\r\n",
             (unsigned long)cmd->correlation_id,
             (unsigned long)ack.feature_count_before,
             (unsigned long)ack.drop_count_before);
}

static void build_fifo_full_inject_response(const zmpio_v3_cmd_slot_t *cmd, zmpio_v3_rsp_slot_t *response)
{
    zmpio_v3_fifo_full_inject_cmd_t request;
    zmpio_v3_fifo_full_inject_ack_t ack;
    fpga_dsp_hal_counters_t counters;

    memset(response, 0, sizeof(*response));
    response->correlation_id = cmd->correlation_id;
    response->session_id = v3_control->session_id;

    if (cmd->session_id != v3_control->session_id) {
        response->type = (uint32_t)ZMPIO_V3_RSP_CONFIG_NACK;
        response->status = (int32_t)ZMPIO_V3_CONFIG_ERR_STALE_SESSION;
        CPU1_LOG("CPU1: ABI v3 FIFO_FULL_INJECT rejected corr=%lu (stale session)\r\n",
                 (unsigned long)cmd->correlation_id);
        return;
    }

    memset(&request, 0, sizeof(request));
    memcpy(&request, cmd->payload,
           (cmd->payload_len < sizeof(request)) ? cmd->payload_len : sizeof(request));

    fpga_dsp_hal_get_counters(&counters);
    ack.feature_count_before = counters.feature_count;
    ack.drop_count_before = counters.ctrl_drop_count;

    fpga_dsp_hal_fault_inject_stall_arm(request.hold_ms);
    ack.hold_ms_applied = fpga_dsp_hal_fault_inject_stall_hold_ms_applied();

    response->type = (uint32_t)ZMPIO_V3_RSP_FIFO_FULL_INJECT_ACK;
    response->status = 0;
    response->payload_len = sizeof(ack);
    memcpy(response->payload, &ack, sizeof(ack));
    CPU1_LOG("CPU1: ABI v3 FIFO_FULL_INJECT armed corr=%lu feature_count_before=%lu "
             "drop_count_before=%lu hold_ms_applied=%lu\r\n",
             (unsigned long)cmd->correlation_id,
             (unsigned long)ack.feature_count_before,
             (unsigned long)ack.drop_count_before,
             (unsigned long)ack.hold_ms_applied);
}

static void dispatch_command(const zmpio_v3_cmd_slot_t *cmd)
{
    zmpio_v3_rsp_slot_t response;

    if (last_correlation_valid && (cmd->correlation_id == last_correlation_id)) {
        /* Duplicate of the last processed command -- resend the cached
         * outcome verbatim instead of re-validating/re-applying it. */
        (void)push_response(&last_response);
        return;
    }

    switch ((zmpio_v3_cmd_type_t)cmd->type) {
    case ZMPIO_V3_CMD_HELLO:
        build_hello_ack(cmd, &response);
        break;
    case ZMPIO_V3_CMD_SET_DSP_CONFIG:
        build_config_response(cmd, &response);
        break;
    case ZMPIO_V3_CMD_DSP_SOFT_RESET:
        build_dsp_soft_reset_response(cmd, &response);
        break;
    case ZMPIO_V3_CMD_FIFO_FULL_INJECT:
        build_fifo_full_inject_response(cmd, &response);
        break;
    case ZMPIO_V3_CMD_NONE:
    default:
        memset(&response, 0, sizeof(response));
        response.type = (uint32_t)ZMPIO_V3_RSP_CONFIG_NACK;
        response.correlation_id = cmd->correlation_id;
        response.session_id = v3_control->session_id;
        response.status = (int32_t)ZMPIO_V3_CONFIG_ERR_UNKNOWN_COMMAND;
        break;
    }

    sign_response(&response);
    last_correlation_id = cmd->correlation_id;
    last_correlation_valid = true;
    last_response = response;
    (void)push_response(&response);
}

bool ipc_v3_poll(void)
{
    zmpio_v3_cmd_slot_t cmd;

    if (!pop_command(&cmd)) {
        return false;
    }

    if (!cmd_crc_is_valid(&cmd)) {
        v3_control->cpu1_crc_drop_count++;
        memory_barrier();
        CPU1_LOG("CPU1: ABI v3 command CRC mismatch, dropped (total=%lu)\r\n",
                 (unsigned long)v3_control->cpu1_crc_drop_count);
        return true;
    }

    dispatch_command(&cmd);
    return true;
}
