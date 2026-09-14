#ifndef CPU0_IPC_V3_H
#define CPU0_IPC_V3_H

#include <stdbool.h>
#include <stdint.h>

#include "zmpio_abi_v3.h"

/*
 * CPU0 side of ABI v3. CPU0's whole SHARED_MEM_BASE..+SHARED_MEM_SIZE region
 * is already mapped NORM_NONCACHE by cpu0_ipc_init() (cpu0_ipc.c), which
 * covers the ABI v3 region too -- unlike ipc_v3.c on CPU1, no explicit
 * Xil_DCache calls are needed here, only the same dmb barriers cpu0_ipc.c
 * already uses.
 */

enum {
    CPU0_IPC_V3_OK = 0,
    CPU0_IPC_V3_TIMEOUT = -1,
    CPU0_IPC_V3_NOT_READY = -2,
    CPU0_IPC_V3_RING_FULL = -3,
    CPU0_IPC_V3_LAYOUT_MISMATCH = -4
};

typedef struct {
    uint32_t session_id;         /* CPU1's session_id, learned from HELLO_ACK */
    uint32_t remote_layout_hash; /* CPU1's ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH, echoed in HELLO_ACK */
    bool     online;             /* HELLO_ACK received AND layout hashes matched */
} cpu0_ipc_v3_link_t;

/* Poll control->cpu1_link_state until it reaches at least CPU1_READY -- the
 * v3 counterpart of cpu0_ipc_wait_for_cpu1_ready(). */
int cpu0_ipc_v3_wait_for_cpu1_ready(uint32_t timeout_ms);

/*
 * Sends HELLO, retrying with a NEW correlation_id on every timeout (a retry
 * is always a new command, never a resend of the same one) up to
 * max_attempts times. On a reply, compares the peer's layout_hash against
 * this CPU0 build's ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH; out_link->online is
 * only ever set true when they match.
 */
int cpu0_ipc_v3_hello(uint32_t max_attempts, uint32_t timeout_ms,
                     cpu0_ipc_v3_link_t *out_link);

/*
 * Sends SET_DSP_CONFIG stamped with link->session_id, retrying with a new
 * correlation_id on timeout. *out_status is only meaningful (a
 * zmpio_v3_config_status_t) when this returns CPU0_IPC_V3_OK.
 */
int cpu0_ipc_v3_set_dsp_config(const cpu0_ipc_v3_link_t *link,
                               const zmpio_v3_set_dsp_config_t *config,
                               uint32_t max_attempts, uint32_t timeout_ms,
                               int32_t *out_status);

/*
 * Fault-injection gate: forces a PL reset while the DSP pipeline is running.
 * Sends ZMPIO_V3_CMD_DSP_SOFT_RESET stamped with link->session_id, retrying
 * with a new correlation_id on timeout. On CPU0_IPC_V3_OK, *out_ack holds
 * the zmpio_dsp_ctrl counters as they stood immediately before CPU1 pulsed
 * SOFT_RESET.
 */
int cpu0_ipc_v3_dsp_soft_reset(const cpu0_ipc_v3_link_t *link,
                               uint32_t max_attempts, uint32_t timeout_ms,
                               zmpio_v3_dsp_soft_reset_ack_t *out_ack);

/*
 * Fault-injection gate: forces the PL feature FIFO to fill up. Sends
 * ZMPIO_V3_CMD_FIFO_FULL_INJECT stamped with link->session_id and the
 * requested hold_ms, retrying with a new correlation_id on timeout. On
 * CPU0_IPC_V3_OK, *out_ack holds the zmpio_dsp_ctrl counters as they stood
 * immediately before CPU1 armed the drain pause, plus the actual (clamped)
 * hold duration.
 */
int cpu0_ipc_v3_fifo_full_inject(const cpu0_ipc_v3_link_t *link, uint32_t hold_ms,
                                 uint32_t max_attempts, uint32_t timeout_ms,
                                 zmpio_v3_fifo_full_inject_ack_t *out_ack);

/*
 * Lab-test helpers, wired to the CPU0 JTAG menu (main.c). Never called
 * during normal operation.
 */

/* Pushes a HELLO command with a deliberately wrong CRC straight onto the
 * ring, bypassing the normal signed-send path, to prove CPU1 drops it
 * (cpu1_crc_drop_count increments) and the ring keeps working afterward. */
int cpu0_ipc_v3_debug_send_corrupt_command(void);

/* Resends the exact correlation_id used by the most recent
 * cpu0_ipc_v3_hello()/cpu0_ipc_v3_set_dsp_config() call, to exercise CPU1's
 * duplicate-suppression path (the effect -- e.g. CONFIG_SEQ -- must not
 * apply twice). */
int cpu0_ipc_v3_debug_resend_last(cpu0_ipc_v3_link_t *out_link, int32_t *out_status);

/* Fires `count` HELLOs back-to-back with no wait between sends, to force the
 * command/response rings around at least one wrap
 * (ZMPIO_ABI_V3_CMD_SLOTS/RSP_SLOTS = 16). Returns the number that received a
 * valid, correctly-ordered reply. */
uint32_t cpu0_ipc_v3_debug_wrap_stress(uint32_t count, uint32_t timeout_ms);

void cpu0_ipc_v3_get_counters(uint32_t *out_cpu1_crc_drop_count,
                              uint32_t *out_cpu0_crc_drop_count);

#endif
