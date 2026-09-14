#ifndef CPU0_IPC_H
#define CPU0_IPC_H

#include <stdint.h>

#include "zmpio_ipc_layout.h"

enum {
    CPU0_IPC_OK = 0,
    CPU0_IPC_EMPTY = -1,
    CPU0_IPC_NOT_READY = -2,
    CPU0_IPC_FULL = -3,
    CPU0_IPC_TIMEOUT = -4
};

/* Mark the complete 4 MiB IPC reservation non-cacheable on CPU0. */
void cpu0_ipc_init(void);

/* CPU1 owns initialization of both ring controls; CPU0 only waits for it. */
int cpu0_ipc_wait_for_cpu1_ready(uint32_t timeout_ms);

int cpu0_ipc_send(const ipc_message_t *message);
int cpu0_ipc_receive(ipc_message_t *message);
int cpu0_ipc_wait_for_reply(uint32_t request_id, ipc_message_t *reply,
                            uint32_t timeout_ms);

#endif
