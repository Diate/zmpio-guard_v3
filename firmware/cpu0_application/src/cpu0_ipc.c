#include "cpu0_ipc.h"

#include <string.h>

#include "sleep.h"
#include "xil_mmu.h"
#include "xil_printf.h"

#define CPU0_IPC_MMU_SECTION_SIZE 0x00100000U
#define CPU0_IPC_POLL_DELAY_US    1000U

static volatile ipc_control_t *const tx_control =
    (volatile ipc_control_t *)IPC_TX_CTRL_BASE;
static volatile ipc_message_t *const tx_messages =
    (volatile ipc_message_t *)IPC_TX_BUFFER_BASE;
static volatile ipc_control_t *const rx_control =
    (volatile ipc_control_t *)IPC_RX_CTRL_BASE;
static volatile ipc_message_t *const rx_messages =
    (volatile ipc_message_t *)IPC_RX_BUFFER_BASE;

static void memory_barrier(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

static int control_is_valid(const volatile ipc_control_t *control)
{
    return (control->size == IPC_BUFFER_SIZE) &&
           (control->head < control->size) &&
           (control->tail < control->size);
}

void cpu0_ipc_init(void)
{
    uint32_t offset;

    /* Xil_SetTlbAttributes applies to one 1 MiB MMU section per call. */
    for (offset = 0U; offset < SHARED_MEM_SIZE;
         offset += CPU0_IPC_MMU_SECTION_SIZE) {
        Xil_SetTlbAttributes((INTPTR)(SHARED_MEM_BASE + offset),
                             NORM_NONCACHE);
    }
    memory_barrier();
}

int cpu0_ipc_wait_for_cpu1_ready(uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms) {
        memory_barrier();
        if (control_is_valid(tx_control) && control_is_valid(rx_control)) {
            return CPU0_IPC_OK;
        }
        usleep(CPU0_IPC_POLL_DELAY_US);
        ++elapsed_ms;
    }
    return CPU0_IPC_TIMEOUT;
}

int cpu0_ipc_send(const ipc_message_t *message)
{
    uint32_t current_head;
    uint32_t next_head;

    if (message == NULL) {
        return CPU0_IPC_NOT_READY;
    }

    memory_barrier();
    if (!control_is_valid(rx_control)) {
        return CPU0_IPC_NOT_READY;
    }

    current_head = rx_control->head;
    next_head = (current_head + 1U) % IPC_BUFFER_SIZE;
    if (next_head == rx_control->tail) {
        return CPU0_IPC_FULL;
    }

    memcpy((void *)&rx_messages[current_head], message, sizeof(*message));
    memory_barrier();
    rx_control->head = next_head;
    memory_barrier();
    return CPU0_IPC_OK;
}

int cpu0_ipc_receive(ipc_message_t *message)
{
    uint32_t current_tail;

    if (message == NULL) {
        return CPU0_IPC_NOT_READY;
    }

    memory_barrier();
    if (!control_is_valid(tx_control)) {
        return CPU0_IPC_NOT_READY;
    }
    if (tx_control->head == tx_control->tail) {
        return CPU0_IPC_EMPTY;
    }

    current_tail = tx_control->tail;
    memcpy(message, (const void *)&tx_messages[current_tail], sizeof(*message));
    memory_barrier();
    tx_control->tail = (current_tail + 1U) % IPC_BUFFER_SIZE;
    memory_barrier();
    return CPU0_IPC_OK;
}

int cpu0_ipc_wait_for_reply(uint32_t request_id, ipc_message_t *reply,
                            uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms) {
        int status = cpu0_ipc_receive(reply);

        if (status == CPU0_IPC_OK) {
            if ((reply->header.magic != IPC_MAGIC) ||
                (reply->header.length > IPC_PAYLOAD_SIZE)) {
                xil_printf("CPU0: discarded malformed CPU1 message\r\n");
            } else if (reply->header.timestamp != request_id) {
                xil_printf("CPU0: discarded unmatched CPU1 reply (id=%lu)\r\n",
                           (unsigned long)reply->header.timestamp);
            } else {
                return CPU0_IPC_OK;
            }
            /* There is no asynchronous push stream in this test protocol.
             * Still count discards toward the timeout so a stream of
             * unmatched traffic cannot hang this call forever. */
            ++elapsed_ms;
            continue;
        }
        if (status == CPU0_IPC_NOT_READY) {
            return status;
        }
        usleep(CPU0_IPC_POLL_DELAY_US);
        ++elapsed_ms;
    }
    return CPU0_IPC_TIMEOUT;
}
