#ifndef ZMPIO_IPC_LOGIC_H
#define ZMPIO_IPC_LOGIC_H

#include "zmpio_ipc.h"

/* ipc_send() return codes. ipc_send() itself collapses every failure to
 * IPC_SEND_ERR (-1), so its existing call sites keep their meaning;
 * ipc_send_timeout() distinguishes the cases because the stream path has to
 * count them apart. */
#define IPC_SEND_OK              0
#define IPC_SEND_ERR            (-1)
#define IPC_SEND_ERR_MUTEX      (-2)
#define IPC_SEND_ERR_RING_FULL  (-3)

int ipc_init(uint32_t buffer_size);
void ipc_rx_init(uint32_t buffer_size);
int ipc_send(const ipc_message_t *message);

/*
 * Bounded-wait variant of ipc_send()
 * for the CPU1 -> Linux telemetry stream.  ipc_send() takes tx_mutex with
 * portMAX_DELAY, which is safe for ipc_rx_task (its whole job is that ring)
 * but not for fpga_result_task, which has a real deadline: while it waits,
 * zmpio_dsp_ctrl's 64-entry feature FIFO keeps filling and eventually drops
 * frames in hardware.  Losing one telemetry frame is cheap and countable;
 * stalling the FIFO drain is not.  wait_ms == 0 polls the mutex once and
 * gives up immediately.
 *
 * Returns IPC_SEND_OK, IPC_SEND_ERR_MUTEX (another task held tx_mutex past
 * the budget), IPC_SEND_ERR_RING_FULL (Linux is not consuming) or
 * IPC_SEND_ERR (bad argument / ring control block invalid).
 */
int ipc_send_timeout(const ipc_message_t *message, uint32_t wait_ms);

int ipc_recv_from_linux(ipc_message_t *message);

#endif
