#include "ipc_shared_mem.h"

#include <stdint.h>

#include "cpu1_log.h"
#include "xil_mmu.h"
#include "zmpio_ipc_layout.h"

/*
 * Why the shared window is mapped Normal Non-cacheable on CPU1
 * -----------------------------------------------------------
 * CPU0 already maps SHARED_MEM_BASE..+SHARED_MEM_SIZE as NORM_NONCACHE
 * (cpu0_ipc.c) and performs no cache maintenance at all.  CPU1 maps the same
 * window the same way rather than mapping it cacheable write-back with
 * explicit Xil_DCacheFlushRange()/Xil_DCacheInvalidateRange() calls around
 * every ring/control access, because cache maintenance on Cortex-A9 works on
 * whole 32-byte lines while both control structures deliberately pack fields
 * owned by DIFFERENT cores into one line:
 *
 *   zmpio_v3_control_t (52 bytes, zmpio_abi_v3.h)
 *     line 0 (offset 0..31)  : magic/abi_version/layout_hash/cpu1_link_state/
 *                              session_id/cmd_tail/rsp_head  (CPU1-owned)
 *                              + cmd_head at offset 20       (CPU0-owned)
 *     line 1 (offset 32..63) : rsp_tail at 32, cpu0_crc_drop_count at 48
 *                              (CPU0-owned) + ring sizes and
 *                              cpu1_crc_drop_count            (CPU1-owned)
 *
 *   ipc_control_t (12 bytes, zmpio_ipc_layout.h): head and tail share one line
 *   and have opposite owners on both the TX and the RX ring.
 *
 * Once CPU1 writes any field it owns, the whole line becomes dirty in CPU1's
 * L1 holding CPU1's stale copy of the neighbouring CPU0-owned fields.  The
 * next Xil_DCacheFlushRange() cleans the entire line back to DDR and thereby
 * REVERTS whatever CPU0 wrote to cmd_head / rsp_tail in the meantime -- CPU0
 * writes go straight to DDR, so there is nothing to arbitrate the conflict.
 * A lost cmd_head increment silently drops a command; the ABI v3 HELLO
 * exchange is a bounded, correlation-id-matched transaction, so a single lost
 * increment costs a full timeout and can desynchronise the ring indices.  The
 * ABI v2 path survived the same hazard only because it is a strict
 * request/response ping-pong: CPU0 never writes an index while CPU1 holds the
 * line dirty.
 *
 * Padding every field to its own cache line would also work, but it changes
 * the wire ABI (and therefore ZMPIO_ABI_V3_LAYOUT_HASH) for no benefit: the
 * shared window carries a few hundred bytes per second, so the cost of
 * non-cacheable access is irrelevant, and matching CPU0's mapping removes the
 * whole class of bug from both ABIs at once.  This is also the arrangement
 * Xilinx documents for Zynq AMP shared-memory IPC.
 *
 * NORM_NONCACHE is Normal (not Device) memory, so unaligned and multi-word
 * accesses -- i.e. the memcpy() calls in ipc_v3.c and zmpio_ipc_logic.c --
 * remain legal.  Ordering between the accesses is still the callers'
 * responsibility and is provided by their dmb barriers.
 */

#define IPC_SHARED_MEM_MMU_SECTION_SIZE 0x00100000U

void ipc_shared_mem_init(void)
{
    uint32_t offset;

    /* Xil_SetTlbAttributes() rewrites one 1 MiB MMU section per call and
     * flushes the D-cache + invalidates the TLB itself, so any line this core
     * may already hold for the window is dropped before the first ring
     * access. */
    for (offset = 0U; offset < SHARED_MEM_SIZE;
         offset += IPC_SHARED_MEM_MMU_SECTION_SIZE) {
        Xil_SetTlbAttributes((INTPTR)(SHARED_MEM_BASE + offset), NORM_NONCACHE);
    }
    __asm__ volatile("dsb" ::: "memory");

    CPU1_LOG("CPU1: shared DDR 0x%08lx..0x%08lx mapped non-cacheable\r\n",
             (unsigned long)SHARED_MEM_BASE,
             (unsigned long)(SHARED_MEM_BASE + SHARED_MEM_SIZE - 1U));
}
