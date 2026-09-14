#include "zmpio_ipc_logic.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "cpu1_log.h"
#include "semphr.h"

extern uint32_t end;

static volatile ipc_control_t *const tx_control =
    (volatile ipc_control_t *)IPC_TX_CTRL_BASE;
static ipc_message_t *const tx_messages =
    (ipc_message_t *)IPC_TX_BUFFER_BASE;
static volatile ipc_control_t *const rx_control =
    (volatile ipc_control_t *)IPC_RX_CTRL_BASE;
static ipc_message_t *const rx_messages =
    (ipc_message_t *)IPC_RX_BUFFER_BASE;

static SemaphoreHandle_t tx_mutex;

static void memory_barrier(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

static uint32_t validated_buffer_size(uint32_t requested)
{
    if (requested != IPC_BUFFER_SIZE) {
        CPU1_LOG("CPU1: IPC ring size %lu invalid, using %u\r\n",
                   (unsigned long)requested, IPC_BUFFER_SIZE);
        return IPC_BUFFER_SIZE;
    }
    return requested;
}

static void initialize_control(volatile ipc_control_t *control,
                               uint32_t buffer_size)
{
    control->head = 0U;
    control->tail = 0U;
    control->size = validated_buffer_size(buffer_size);
    memory_barrier();
}

static int control_is_valid(const volatile ipc_control_t *control)
{
    return (control->size == IPC_BUFFER_SIZE) &&
           (control->head < control->size) &&
           (control->tail < control->size);
}

int ipc_init(uint32_t buffer_size)
{
    if ((UINTPTR)&end > (UINTPTR)SHARED_MEM_BASE) {
        CPU1_LOG("CPU1: firmware end 0x%08lx overlaps IPC memory\r\n",
                   (unsigned long)(UINTPTR)&end);
        return -1;
    }

    initialize_control(tx_control, buffer_size);
    tx_mutex = xSemaphoreCreateMutex();
    if (tx_mutex == NULL) {
        CPU1_LOG("CPU1: IPC TX mutex allocation failed\r\n");
        return -1;
    }
    return 0;
}

void ipc_rx_init(uint32_t buffer_size)
{
    initialize_control(rx_control, buffer_size);
}

/*
 * One implementation for both ipc_send() and ipc_send_timeout(); they differ
 * only in how long they are willing to wait for tx_mutex.  Everything after
 * the take is identical, so the ring discipline (validate control block ->
 * reserve slot -> copy -> barrier -> publish head) cannot drift between the
 * command/response path and the telemetry stream path.
 */
static int ipc_send_locked(const ipc_message_t *message, TickType_t wait_ticks)
{
    uint32_t current_head;
    uint32_t next_head;
    int result;

    if ((message == NULL) || (tx_mutex == NULL)) {
        return IPC_SEND_ERR;
    }
    if (xSemaphoreTake(tx_mutex, wait_ticks) != pdTRUE) {
        return IPC_SEND_ERR_MUTEX;
    }

    memory_barrier();
    if (!control_is_valid(tx_control)) {
        result = IPC_SEND_ERR;
        goto release;
    }

    current_head = tx_control->head;
    next_head = (current_head + 1U) % tx_control->size;
    if (next_head == tx_control->tail) {
        result = IPC_SEND_ERR_RING_FULL;
        goto release;
    }

    memcpy(&tx_messages[current_head], message, sizeof(*message));
    memory_barrier();

    tx_control->head = next_head;
    memory_barrier();
    result = IPC_SEND_OK;

release:
    (void)xSemaphoreGive(tx_mutex);
    return result;
}

int ipc_send(const ipc_message_t *message)
{
    /* Collapses every failure back to -1: unchanged contract for the
     * command/response call sites in main.c and ipc_v3.c. */
    return (ipc_send_locked(message, portMAX_DELAY) == IPC_SEND_OK)
               ? IPC_SEND_OK
               : IPC_SEND_ERR;
}

int ipc_send_timeout(const ipc_message_t *message, uint32_t wait_ms)
{
    TickType_t wait_ticks = (wait_ms == 0U) ? (TickType_t)0
                                            : pdMS_TO_TICKS(wait_ms);

    /* pdMS_TO_TICKS() truncates toward zero, so any nonzero budget shorter
     * than one tick would silently become "do not wait at all".  Round it up
     * to one tick instead -- the caller asked for a bounded wait, not for
     * none. */
    if ((wait_ms != 0U) && (wait_ticks == (TickType_t)0)) {
        wait_ticks = (TickType_t)1;
    }
    return ipc_send_locked(message, wait_ticks);
}

int ipc_recv_from_linux(ipc_message_t *message)
{
    uint32_t current_tail;

    if (message == NULL) {
        return -1;
    }

    memory_barrier();
    if (!control_is_valid(rx_control) ||
        (rx_control->head == rx_control->tail)) {
        return -1;
    }

    current_tail = rx_control->tail;
    memory_barrier();
    memcpy(message, &rx_messages[current_tail], sizeof(*message));

    rx_control->tail = (current_tail + 1U) % rx_control->size;
    memory_barrier();
    return 0;
}
