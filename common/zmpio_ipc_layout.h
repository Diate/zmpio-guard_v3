#ifndef ZMPIO_IPC_LAYOUT_H
#define ZMPIO_IPC_LAYOUT_H

#include <stdint.h>

#include "zmpio_protocol.h"

/* DDR reservations shared by the CPU0 JTAG test, CPU1 firmware and Linux. */
#define CPU1_RESERVED_BASE  0x18000000U
#define CPU1_RESERVED_SIZE  0x01000000U

#define SHARED_MEM_BASE     0x19000000U
#define SHARED_MEM_SIZE     0x00400000U

/* CPU1 producer, CPU0/Linux consumer. */
#define IPC_TX_CTRL_BASE    (SHARED_MEM_BASE + 0x00000U)
#define IPC_TX_BUFFER_BASE  (SHARED_MEM_BASE + 0x01000U)

/* CPU0/Linux producer, CPU1 consumer. */
#define IPC_RX_CTRL_BASE    (SHARED_MEM_BASE + 0x20000U)
#define IPC_RX_BUFFER_BASE  (SHARED_MEM_BASE + 0x21000U)

#define IPC_BUFFER_SIZE     256U

typedef struct __attribute__((packed)) {
    uint32_t head;
    uint32_t tail;
    uint32_t size;
} ipc_control_t;

_Static_assert(IPC_TX_BUFFER_BASE +
                   IPC_BUFFER_SIZE * sizeof(ipc_message_t) <=
               IPC_RX_CTRL_BASE,
               "TX ring overlaps RX ring");
_Static_assert(IPC_RX_BUFFER_BASE +
                   IPC_BUFFER_SIZE * sizeof(ipc_message_t) <=
               SHARED_MEM_BASE + SHARED_MEM_SIZE,
               "RX ring exceeds shared memory");

#endif
