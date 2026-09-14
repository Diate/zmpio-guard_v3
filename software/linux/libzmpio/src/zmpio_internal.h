#ifndef ZMPIO_INTERNAL_H
#define ZMPIO_INTERNAL_H

#include "libzmpio.h"
#include "zmpio_ipc_layout.h"
#include "zmpio_uio.h"

/*
 * Shared handle definition -- private to the library, never exposed past
 * libzmpio.h's opaque zmpio_handle_t. zmpio_v2.c and zmpio_v3.c both index
 * into shm.base using offset-from-SHARED_MEM_BASE arithmetic -- valid
 * because zmpio-shm's UIO reg (deploy/petalinux_overlay/
 * system-user-openamp-template.dtsi) covers the identical physical range
 * (SHARED_MEM_BASE, SHARED_MEM_SIZE), so ABI v3's region
 * (ZMPIO_ABI_V3_BASE = SHARED_MEM_BASE + 0x40000) falls inside the same
 * single mapping as the ABI v2 rings -- one mmap serves both layers.
 */
/*
 * Step 7 R1: how many out-of-order/late ABI v2 replies the drain loop will
 * hold rather than discard.  This is not a throughput buffer -- zmpiod
 * issues at most one request at a time and the ring is drained on every
 * doorbell -- it exists so a reply that lands just after its caller gave up
 * survives until the retry asks for it.  8 slots is 2 KiB and more depth
 * than a strictly serial requester can ever need; anything beyond that is a
 * bug worth counting (orphan_overflow) rather than absorbing.
 */
#define ZMPIO_V2_ORPHAN_MAX 8U

struct zmpio_handle {
    zmpio_uio_region_t shm;      /* zmpio-shm: ABI v2 rings + ABI v3 region */
    zmpio_uio_region_t doorbell; /* zmpio-doorbell: wakeup only, no payload */

    uint32_t next_v2_request_id;
    uint32_t next_v3_correlation_id;
    zmpio_v3_cmd_slot_t last_v3_command;
    int      last_v3_command_valid;

    /* Step 7 feature stream (SDD_15).  All of this is touched only from the
     * one thread that owns the handle -- there is no locking here and none
     * is needed while the single-owner rule above holds. */
    zmpio_stream_cb_t stream_cb;
    void             *stream_user;
    zmpio_v2_stream_stats_t stream_stats;
    int               stream_seq_valid; /* false until the first message, so
                                         * the first sequence is never
                                         * mistaken for a gap */
    ipc_message_t     orphan[ZMPIO_V2_ORPHAN_MAX];
    unsigned          orphan_head;
    unsigned          orphan_count;
};

#define ZMPIO_SHM_PTR(h, phys_addr) \
    ((h)->shm.base + ((phys_addr) - SHARED_MEM_BASE))

/* zmpio_v3.c: seeds next_v3_correlation_id at zmpio_open() -- see the doc
 * comment on this function's definition for why it is NOT always 1. */
uint32_t zmpio_v3_seed_correlation_id(void);

#endif
