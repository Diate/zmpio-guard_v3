#include "libzmpio.h"
#include "zmpio_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "zmpio_doorbell_regs.h"

#define ZMPIO_SHM_UIO_NAME      "zmpio-shm"
#define ZMPIO_DOORBELL_UIO_NAME "zmpio-doorbell"
#define ZMPIO_ACK_RECHECK_MAX_RETRIES 16U

/* Double-open safety: the handle does not exist until open succeeds, so
 * there is no global mutable mapping pointer to
 * accidentally reuse. */
zmpio_status_t zmpio_open(zmpio_handle_t **out_handle)
{
    zmpio_handle_t *handle;

    if (out_handle == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    handle = (zmpio_handle_t *)calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ZMPIO_ERR_MMAP;
    }
    handle->shm.fd = -1;
    handle->doorbell.fd = -1;

    if (zmpio_uio_open(ZMPIO_SHM_UIO_NAME, &handle->shm) != 0) {
        int saved_errno = errno;
        free(handle);
        return (saved_errno == ENODEV) ? ZMPIO_ERR_UIO_NOT_FOUND
                                       : ZMPIO_ERR_MMAP;
    }
    if (zmpio_uio_open(ZMPIO_DOORBELL_UIO_NAME, &handle->doorbell) != 0) {
        int saved_errno = errno;
        zmpio_uio_close(&handle->shm);
        free(handle);
        return (saved_errno == ENODEV) ? ZMPIO_ERR_UIO_NOT_FOUND
                                       : ZMPIO_ERR_MMAP;
    }

    handle->next_v2_request_id = 1U;
    handle->next_v3_correlation_id = zmpio_v3_seed_correlation_id();
    handle->last_v3_command_valid = 0;

    *out_handle = handle;
    return ZMPIO_OK;
}

void zmpio_close(zmpio_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }
    zmpio_uio_close(&handle->doorbell);
    zmpio_uio_close(&handle->shm);
    free(handle);
}

/*
 * ACK-before-re-enable, bounded PENDING recheck -- identical ordering to
 * tools/uio_spike.c (Step 6.2's own bring-up validation of this exact
 * sequence) and to CPU0's bare-metal doorbell driver
 * (firmware/cpu0_application/src/pl_doorbell.c). See SDD_10 SS5's doc
 * comment on why re-enabling before ACK would silently drop a SET that
 * lands in between.
 */
zmpio_status_t zmpio_wait_doorbell(zmpio_handle_t *handle)
{
    int wait_result;
    volatile uint8_t *regs;
    uint32_t status;
    unsigned retry;

    if (handle == NULL) {
        return ZMPIO_ERR_INVALID_ARG;
    }

    wait_result = zmpio_uio_irq_wait(&handle->doorbell);
    if (wait_result == 1) {
        return ZMPIO_ERR_INTERRUPTED;
    }
    if (wait_result != 0) {
        return ZMPIO_ERR_TIMEOUT;
    }

    regs = handle->doorbell.base;
    status = *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_STATUS);
    if ((status & ZMPIO_DOORBELL_BIT_PENDING) != 0U) {
        *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_ACK) =
            ZMPIO_DOORBELL_BIT_ACK;

        for (retry = 0U; retry < ZMPIO_ACK_RECHECK_MAX_RETRIES; ++retry) {
            status = *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_STATUS);
            if ((status & ZMPIO_DOORBELL_BIT_PENDING) == 0U) {
                break;
            }
            *(volatile uint32_t *)(regs + ZMPIO_DOORBELL_REG_ACK) =
                ZMPIO_DOORBELL_BIT_ACK;
        }
    }
    /* A wakeup with nothing pending (spurious, or CPU1 already re-armed and
     * re-fired between our IRQ and this STATUS read) needs no ACK -- see
     * SDD_10 SS2 "MUST NOT" list and tools/uio_spike.c's identical branch. */

    if (zmpio_uio_irq_reenable(&handle->doorbell) != 0) {
        return ZMPIO_ERR_MMAP;
    }
    return ZMPIO_OK;
}

uint32_t zmpio_doorbell_count(zmpio_handle_t *handle)
{
    if (handle == NULL) {
        return 0U;
    }
    return *(volatile uint32_t *)(handle->doorbell.base +
                                  ZMPIO_DOORBELL_REG_COUNT);
}

int zmpio_doorbell_fd(zmpio_handle_t *handle)
{
    return (handle != NULL) ? handle->doorbell.fd : -1;
}
