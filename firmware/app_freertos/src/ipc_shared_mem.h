#ifndef IPC_SHARED_MEM_H
#define IPC_SHARED_MEM_H

/*
 * Maps the CPU0/CPU1 shared DDR window (SHARED_MEM_BASE .. +SHARED_MEM_SIZE,
 * zmpio_ipc_layout.h) as Normal Non-cacheable on CPU1, mirroring what
 * cpu0_ipc_init() already does on CPU0.
 *
 * MUST be called before ipc_init()/ipc_rx_init()/ipc_v3_init() and before any
 * access to the ABI v2 rings or the ABI v3 control block.  See the file
 * comment in ipc_shared_mem.c for why the previous "cacheable + manual
 * Xil_DCache*Range()" arrangement was unsound.
 */
void ipc_shared_mem_init(void);

#endif
