#ifndef ZMPIO_ABI_V3_H
#define ZMPIO_ABI_V3_H

#include <stdint.h>

#include "zmpio_ipc_layout.h"

/*
 * ABI v3 -- a bounded, polled command/response channel between CPU0 (produces
 * commands, consumes responses) and CPU1 (consumes commands, produces
 * responses).
 *
 * It occupies a region of shared DDR disjoint from the ABI v2 rings
 * (zmpio_ipc_layout.h), so v2 traffic -- sensor poll, logger control,
 * FEATURE_V2 / ANOMALY_RULE_V1 / DSP_HEALTH logging -- is unaffected.
 *
 * Ownership rule, one writer per field: every field below names its single
 * writer. A reader must Xil_DCacheInvalidateRange() + dmb before reading a
 * field it does not own; a writer must dmb + Xil_DCacheFlushRange() after
 * writing one it does. See ipc_v3.c and cpu0_ipc_v3.c for the exact sequencing.
 */

#define ZMPIO_ABI_V3_BASE   (SHARED_MEM_BASE + 0x00040000U) /* 0x19040000 */
#define ZMPIO_ABI_V3_SIZE   0x00040000U                     /* 256 KiB reserved for v3+ growth */

#define ZMPIO_ABI_V3_CONTROL_BASE  (ZMPIO_ABI_V3_BASE + 0x0000U)
#define ZMPIO_ABI_V3_CMD_RING_BASE (ZMPIO_ABI_V3_BASE + 0x1000U)
#define ZMPIO_ABI_V3_RSP_RING_BASE (ZMPIO_ABI_V3_BASE + 0x2000U)

#define ZMPIO_ABI_V3_MAGIC      0x5A4D5033U /* "ZMP3" */
#define ZMPIO_ABI_V3_VERSION    3U

#define ZMPIO_ABI_V3_CMD_SLOTS         16U
#define ZMPIO_ABI_V3_RSP_SLOTS         16U
#define ZMPIO_ABI_V3_CMD_PAYLOAD_SIZE  64U
#define ZMPIO_ABI_V3_RSP_PAYLOAD_SIZE  64U

/* Bit i of a SET_DSP_CONFIG feature_mask selects one of the 10 FeatureFrameV2
 * fields; only bits 0..9 are meaningful. */
#define ZMPIO_ABI_V3_FEATURE_COUNT 10U

typedef enum {
    ZMPIO_V3_CMD_NONE = 0,
    ZMPIO_V3_CMD_HELLO = 1,
    ZMPIO_V3_CMD_SET_DSP_CONFIG = 2,
    /* Fault injection: pulse zmpio_dsp_ctrl's CONTROL.SOFT_RESET via
     * fpga_dsp_hal_soft_reset_pulse(). No payload. session_id must match. */
    ZMPIO_V3_CMD_DSP_SOFT_RESET = 3,
    /* Fault injection: pause draining the feature FIFO for
     * zmpio_v3_fifo_full_inject_cmd_t.hold_ms (clamped to
     * APP_FIFO_FULL_INJECT_MAX_HOLD_MS) so the DSP core fills the 64-entry FIFO
     * and increments DROP_COUNT. Software only, no PL register is written.
     * session_id must match. */
    ZMPIO_V3_CMD_FIFO_FULL_INJECT = 4
} zmpio_v3_cmd_type_t;

typedef enum {
    ZMPIO_V3_RSP_NONE = 0,
    ZMPIO_V3_RSP_HELLO_ACK = 1,
    ZMPIO_V3_RSP_CONFIG_ACK = 2,
    ZMPIO_V3_RSP_CONFIG_NACK = 3,
    ZMPIO_V3_RSP_DSP_SOFT_RESET_ACK = 4,
    ZMPIO_V3_RSP_FIFO_FULL_INJECT_ACK = 5
} zmpio_v3_rsp_type_t;

/* CPU1-owned; CPU0 only ever reads this field (control->cpu1_link_state). */
typedef enum {
    ZMPIO_V3_LINK_CPU1_NOT_READY = 0,
    ZMPIO_V3_LINK_CPU1_READY = 1,
    ZMPIO_V3_LINK_ONLINE = 2
} zmpio_v3_link_state_t;

/* Protocol-level outcome of a SET_DSP_CONFIG, carried in
 * zmpio_v3_rsp_slot_t.status. Transport-level outcomes (timeout, CRC drop, ring
 * full) never reach this far -- see cpu0_ipc_v3.h's CPU0_IPC_V3_* enum. */
typedef enum {
    ZMPIO_V3_CONFIG_OK = 0,
    ZMPIO_V3_CONFIG_ERR_BAD_COEFF_SET = -1,
    ZMPIO_V3_CONFIG_ERR_BAD_FFT_SCALE = -2,
    ZMPIO_V3_CONFIG_ERR_BAD_FEATURE_MASK = -3,
    ZMPIO_V3_CONFIG_ERR_STALE_SESSION = -4,
    ZMPIO_V3_CONFIG_ERR_UNKNOWN_COMMAND = -5
} zmpio_v3_config_status_t;

/*
 * There is no PL register interface that pushes these values into
 * zmpio_dsp_ctrl: CONFIG_SEQ is a scratch register, bumped by
 * fpga_dsp_hal_bump_config_seq(). SET_DSP_CONFIG therefore validates the
 * request and, on success, bumps CONFIG_SEQ -- it does NOT change FIR or FFT
 * behaviour in the PL.
 */
typedef struct __attribute__((packed)) {
    uint32_t coeff_set_id;    /* index into a fixed table of known-valid FIR/FFT coefficient sets */
    uint32_t fft_scale_shift; /* must be within [0, APP_ZMPIO_V3_FFT_SCALE_SHIFT_MAX] */
    uint32_t feature_mask;    /* bits 0..(ZMPIO_ABI_V3_FEATURE_COUNT-1) only, must be nonzero */
} zmpio_v3_set_dsp_config_t;

_Static_assert(sizeof(zmpio_v3_set_dsp_config_t) <= ZMPIO_ABI_V3_CMD_PAYLOAD_SIZE,
               "zmpio_v3_set_dsp_config_t does not fit in a command payload");

/* DSP_SOFT_RESET_ACK response payload: zmpio_dsp_ctrl counters as they stood
 * immediately before the pulse, so CPU0 can log fault-injection evidence
 * without reading zmpio_dsp_ctrl's PL registers itself -- CPU1 stays the
 * sole owner of that MMIO, same ownership discipline as axi_iic_0/spi0. */
typedef struct __attribute__((packed)) {
    uint32_t feature_count_before;
    uint32_t drop_count_before;
} zmpio_v3_dsp_soft_reset_ack_t;

_Static_assert(sizeof(zmpio_v3_dsp_soft_reset_ack_t) <= ZMPIO_ABI_V3_RSP_PAYLOAD_SIZE,
               "zmpio_v3_dsp_soft_reset_ack_t does not fit in a response payload");

/* FIFO_FULL_INJECT command payload: requested drain-pause duration. CPU1
 * clamps this to APP_FIFO_FULL_INJECT_MAX_HOLD_MS before arming, and echoes
 * the clamped value back in zmpio_v3_fifo_full_inject_ack_t.hold_ms_applied
 * -- CPU0 must not assume the requested value was honored verbatim. */
typedef struct __attribute__((packed)) {
    uint32_t hold_ms;
} zmpio_v3_fifo_full_inject_cmd_t;

_Static_assert(sizeof(zmpio_v3_fifo_full_inject_cmd_t) <= ZMPIO_ABI_V3_CMD_PAYLOAD_SIZE,
               "zmpio_v3_fifo_full_inject_cmd_t does not fit in a command payload");

/* FIFO_FULL_INJECT_ACK response payload: zmpio_dsp_ctrl counters as they
 * stood immediately before the drain pause was armed, plus the actual
 * (clamped) hold duration -- same evidence-without-CPU0-touching-PL-MMIO
 * discipline as zmpio_v3_dsp_soft_reset_ack_t. */
typedef struct __attribute__((packed)) {
    uint32_t feature_count_before;
    uint32_t drop_count_before;
    uint32_t hold_ms_applied;
} zmpio_v3_fifo_full_inject_ack_t;

_Static_assert(sizeof(zmpio_v3_fifo_full_inject_ack_t) <= ZMPIO_ABI_V3_RSP_PAYLOAD_SIZE,
               "zmpio_v3_fifo_full_inject_ack_t does not fit in a response payload");

/* One command slot. CPU0 is the sole writer (owns cmd_head); CPU1 is the
 * sole reader/consumer (owns cmd_tail) -- same single-producer/single-
 * consumer discipline as ipc_message_t in zmpio_protocol.h. `crc32` covers
 * every byte of this struct with `crc32` itself treated as zero; see
 * zmpio_crc32.h. */
typedef struct __attribute__((packed)) {
    uint32_t type;           /* zmpio_v3_cmd_type_t */
    uint32_t correlation_id; /* CPU0-generated, nonzero, monotonic; a timeout retry must use a NEW id */
    uint32_t session_id;     /* CPU1 session this command targets; ignored for HELLO, must match for SET_DSP_CONFIG */
    uint32_t payload_len;
    uint8_t  payload[ZMPIO_ABI_V3_CMD_PAYLOAD_SIZE];
    uint32_t crc32;
} zmpio_v3_cmd_slot_t;

/* One response slot. CPU1 is the sole writer (owns rsp_head); CPU0 is the
 * sole reader/consumer (owns rsp_tail). */
typedef struct __attribute__((packed)) {
    uint32_t type;           /* zmpio_v3_rsp_type_t */
    uint32_t correlation_id; /* echoes the command this answers */
    uint32_t session_id;     /* CPU1's session_id at the time of this response */
    int32_t  status;         /* 0 = OK, else zmpio_v3_config_status_t (or another negative reason) */
    uint32_t payload_len;
    uint8_t  payload[ZMPIO_ABI_V3_RSP_PAYLOAD_SIZE];
    uint32_t crc32;
} zmpio_v3_rsp_slot_t;

/*
 * Control block. Every field has exactly one writer, listed alongside it --
 * this is what lets CPU0 and CPU1 poll/update it concurrently without a
 * shared lock (same reasoning as ipc_control_t in zmpio_ipc_layout.h, split
 * across more fields because v3 is bidirectional command/response instead of
 * v2's two independent one-way streams).
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* CPU1-owned; ZMPIO_ABI_V3_MAGIC once initialized */
    uint32_t abi_version;         /* CPU1-owned; ZMPIO_ABI_V3_VERSION */
    uint32_t layout_hash;         /* CPU1-owned; this CPU1 build's ZMPIO_ABI_V3_LAYOUT_HASH */
    uint32_t cpu1_link_state;     /* CPU1-owned; zmpio_v3_link_state_t */
    uint32_t session_id;          /* CPU1-owned; bumped once at ipc_v3_init(), see ipc_v3.c */
    uint32_t cmd_head;            /* CPU0-owned producer index, mod cmd_ring_size */
    uint32_t cmd_tail;            /* CPU1-owned consumer index, mod cmd_ring_size */
    uint32_t rsp_head;            /* CPU1-owned producer index, mod rsp_ring_size */
    uint32_t rsp_tail;            /* CPU0-owned consumer index, mod rsp_ring_size */
    uint32_t cmd_ring_size;       /* CPU1-owned; ZMPIO_ABI_V3_CMD_SLOTS, written once at init */
    uint32_t rsp_ring_size;       /* CPU1-owned; ZMPIO_ABI_V3_RSP_SLOTS, written once at init */
    uint32_t cpu1_crc_drop_count; /* CPU1-owned; commands dropped for bad CRC (ring never stalls on these) */
    uint32_t cpu0_crc_drop_count; /* CPU0-owned; responses dropped for bad CRC */
} zmpio_v3_control_t;

_Static_assert(sizeof(zmpio_v3_cmd_slot_t) == 84U,
               "zmpio_v3_cmd_slot_t ABI changed -- re-run tools/gen_layout_hash.py");
_Static_assert(sizeof(zmpio_v3_rsp_slot_t) == 88U,
               "zmpio_v3_rsp_slot_t ABI changed -- re-run tools/gen_layout_hash.py");
_Static_assert(sizeof(zmpio_v3_control_t) == 52U,
               "zmpio_v3_control_t ABI changed -- re-run tools/gen_layout_hash.py");

_Static_assert(ZMPIO_ABI_V3_CMD_RING_BASE + (ZMPIO_ABI_V3_CMD_SLOTS * sizeof(zmpio_v3_cmd_slot_t)) <=
                   ZMPIO_ABI_V3_RSP_RING_BASE,
               "ABI v3 command ring overlaps response ring");
_Static_assert(ZMPIO_ABI_V3_RSP_RING_BASE + (ZMPIO_ABI_V3_RSP_SLOTS * sizeof(zmpio_v3_rsp_slot_t)) <=
                   ZMPIO_ABI_V3_BASE + ZMPIO_ABI_V3_SIZE,
               "ABI v3 response ring exceeds its reserved region");
_Static_assert(ZMPIO_ABI_V3_BASE >=
                   IPC_RX_BUFFER_BASE + (IPC_BUFFER_SIZE * sizeof(ipc_message_t)),
               "ABI v3 region overlaps the ABI v2 RX ring");
_Static_assert(ZMPIO_ABI_V3_BASE + ZMPIO_ABI_V3_SIZE <= SHARED_MEM_BASE + SHARED_MEM_SIZE,
               "ABI v3 region exceeds shared memory");

/*
 * Generated by tools/gen_layout_hash.py from that script's SPEC constant, which
 * must be kept in sync BY HAND with every struct above whenever an ABI-relevant
 * field, size, or enum value changes.
 *
 * Both cores normally compile this same generated header, so the hash matches
 * trivially when both are rebuilt from one checkout. What it guards against is
 * rebuilding only ONE side -- for example CPU1 reflashed with a new
 * SET_DSP_CONFIG payload shape while CPU0's boot ELF is stale.
 */
#include "zmpio_abi_v3_layout_hash.h"

/*
 * Test-only override: define ZMPIO_ABI_V3_TEST_FORCE_HASH_MISMATCH=1 via
 * USER_COMPILE_DEFINITIONS (UserConfig.cmake) on exactly ONE side, never both,
 * to simulate the stale-binary case above without two checkouts. Leave it
 * undefined for every normal and production build.
 */
#if defined(ZMPIO_ABI_V3_TEST_FORCE_HASH_MISMATCH) && (ZMPIO_ABI_V3_TEST_FORCE_HASH_MISMATCH != 0)
#define ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH (ZMPIO_ABI_V3_LAYOUT_HASH ^ 0xFFFFFFFFU)
#else
#define ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH ZMPIO_ABI_V3_LAYOUT_HASH
#endif

#endif
