#ifndef IPC_V3_H
#define IPC_V3_H

#include <stdbool.h>
#include <stdint.h>

#include "zmpio_abi_v3.h"

/*
 * CPU1 side of ABI v3. See zmpio_abi_v3.h for
 * the wire format and the single-writer-per-field ownership rule this file
 * follows -- the cache-maintenance/dmb sequencing mirrors zmpio_ipc_logic.c.
 */

/* Zeroes the ABI v3 control block and both rings, picks a session_id, and
 * publishes ZMPIO_V3_LINK_CPU1_READY. Call from main() alongside ipc_init()/
 * ipc_rx_init(), before vTaskStartScheduler() -- CPU0 starts polling for
 * readiness as soon as it releases CPU1 from WFE (see cpu0_ipc_v3.h). */
int ipc_v3_init(void);

/*
 * Drains at most one command per call -- same bounded-per-iteration shape as
 * ipc_recv_from_linux(), so ipc_rx_task can poll both ABI v2 and ABI v3 in
 * one loop without either starving the other. Returns true if a command slot
 * was consumed (whether it was ACKed, NACKed, or dropped for a bad CRC --
 * "handled" here just means the ring made forward progress, matching the
 * rule that a CRC error must never stall the ring).
 */
bool ipc_v3_poll(void);

#endif
