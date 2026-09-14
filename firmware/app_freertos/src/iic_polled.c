#include "iic_polled.h"

#include <stddef.h>

#include "platform_time.h"
#include "xiic_l.h"

/*
 * Register sequence transcribed from bsp/libsrc/iic/src/xiic_l.c
 * (XIic_Send/SendData/XIic_Recv/RecvData, iic_v3_10). Every write to the
 * device is identical to the BSP version; only the waits differ. See
 * iic_polled.h for why the BSP functions cannot be used directly.
 */

typedef struct {
    uint32_t start;
    uint32_t limit;
} iic_deadline_t;

static iic_polled_status_t last_status = IIC_POLLED_OK;

static void deadline_start(iic_deadline_t *deadline, uint32_t timeout_us)
{
    deadline->start = platform_time_counter();
    deadline->limit = platform_time_us_to_counts(timeout_us);
}

/* Unsigned subtraction makes this correct across the counter's 32-bit wrap. */
static bool deadline_expired(const iic_deadline_t *deadline)
{
    return (uint32_t)(platform_time_counter() - deadline->start) >=
           deadline->limit;
}

/*
 * Waits until one of `wanted` is latched in the ISR, and fails as soon as one
 * of `fatal` is latched instead. Both conditions are checked every pass, so
 * neither a NAK nor a wedged bus can outlive the deadline.
 */
static iic_polled_status_t wait_isr(UINTPTR base_address,
                                    const iic_deadline_t *deadline,
                                    uint32_t wanted, uint32_t fatal)
{
    for (;;) {
        uint32_t status = XIic_ReadIisr(base_address);

        if ((status & wanted) != 0U) {
            return IIC_POLLED_OK;
        }
        if ((status & fatal) != 0U) {
            if ((status & XIIC_INTR_ARB_LOST_MASK) != 0U) {
                return IIC_POLLED_ERR_ARB_LOST;
            }
            return IIC_POLLED_ERR_TX_ERROR;
        }
        if (deadline_expired(deadline)) {
            return IIC_POLLED_ERR_TIMEOUT;
        }
    }
}

static iic_polled_status_t wait_bus_busy(UINTPTR base_address,
                                         const iic_deadline_t *deadline,
                                         bool want_busy)
{
    for (;;) {
        uint32_t status = XIic_ReadReg(base_address, XIIC_SR_REG_OFFSET);
        bool busy = (status & XIIC_SR_BUS_BUSY_MASK) != 0U;

        if (busy == want_busy) {
            return IIC_POLLED_OK;
        }
        if (deadline_expired(deadline)) {
            return IIC_POLLED_ERR_TIMEOUT;
        }
    }
}

/* Replaces XIic_WaitBusFree(), whose 10000 x usleep(100) budget is a fixed
 * one second of spinning that the caller cannot shorten. */
static iic_polled_status_t wait_bus_free(UINTPTR base_address,
                                         const iic_deadline_t *deadline)
{
    if (wait_bus_busy(base_address, deadline, false) != IIC_POLLED_OK) {
        return IIC_POLLED_ERR_BUS_BUSY;
    }
    return IIC_POLLED_OK;
}

/* Mirrors SendData() in xiic_l.c. Returns the number of bytes NOT sent. */
static unsigned send_data(UINTPTR base_address, const u8 *buffer,
                          unsigned byte_count, u8 option,
                          const iic_deadline_t *deadline,
                          iic_polled_status_t *status_out)
{
    *status_out = IIC_POLLED_OK;

    while (byte_count > 0U) {
        iic_polled_status_t status =
            wait_isr(base_address, deadline, XIIC_INTR_TX_EMPTY_MASK,
                     XIIC_INTR_TX_ERROR_MASK | XIIC_INTR_ARB_LOST_MASK |
                     XIIC_INTR_BNB_MASK);

        if (status != IIC_POLLED_OK) {
            *status_out = status;
            return byte_count;
        }

        if (byte_count > 1U) {
            XIic_WriteReg(base_address, XIIC_DTR_REG_OFFSET, *buffer);
            ++buffer;
        } else {
            if (option == XIIC_STOP) {
                /* Clearing MSMS before the last byte makes the controller
                 * emit the STOP immediately after it. */
                XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                              XIIC_CR_ENABLE_DEVICE_MASK |
                              XIIC_CR_DIR_IS_TX_MASK);
            }

            XIic_WriteReg(base_address, XIIC_DTR_REG_OFFSET, *buffer);
            ++buffer;

            if (option == XIIC_REPEATED_START) {
                XIic_ClearIisr(base_address, XIIC_INTR_TX_EMPTY_MASK);
                /* RSTA may only be set once the FIFO is completely empty.
                 * This is the wait that is unbounded and escape-less at
                 * xiic_l.c:576 -- the one that hangs sensor_task. */
                status = wait_isr(base_address, deadline,
                                  XIIC_INTR_TX_EMPTY_MASK,
                                  XIIC_INTR_TX_ERROR_MASK |
                                  XIIC_INTR_ARB_LOST_MASK);
                if (status != IIC_POLLED_OK) {
                    *status_out = status;
                    return byte_count;
                }
                XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                              XIIC_CR_REPEATED_START_MASK |
                              XIIC_CR_ENABLE_DEVICE_MASK |
                              XIIC_CR_DIR_IS_TX_MASK |
                              XIIC_CR_MSMS_MASK);
            }
        }

        XIic_ClearIisr(base_address, XIIC_INTR_TX_EMPTY_MASK);
        --byte_count;
    }

    if (option == XIIC_STOP) {
        iic_polled_status_t status =
            wait_isr(base_address, deadline, XIIC_INTR_BNB_MASK, 0U);

        if (status != IIC_POLLED_OK) {
            *status_out = status;
        }
    }

    return byte_count;
}

/* Mirrors RecvData() in xiic_l.c. Returns the number of bytes NOT received. */
static unsigned recv_data(UINTPTR base_address, u8 *buffer,
                          unsigned byte_count, u8 option,
                          const iic_deadline_t *deadline,
                          iic_polled_status_t *status_out)
{
    *status_out = IIC_POLLED_OK;

    while (byte_count > 0U) {
        uint32_t control_reg;
        uint32_t fatal_mask;
        iic_polled_status_t status;

        /* On the final byte the master itself drives NO-ACK, which latches
         * TX_ERROR as a matter of course -- so it must not be fatal there. */
        if (byte_count == 1U) {
            fatal_mask = XIIC_INTR_ARB_LOST_MASK | XIIC_INTR_BNB_MASK;
        } else {
            fatal_mask = XIIC_INTR_ARB_LOST_MASK | XIIC_INTR_TX_ERROR_MASK |
                         XIIC_INTR_BNB_MASK;
        }

        status = wait_isr(base_address, deadline, XIIC_INTR_RX_FULL_MASK,
                          fatal_mask);
        if (status != IIC_POLLED_OK) {
            *status_out = status;
            return byte_count;
        }

        control_reg = XIic_ReadReg(base_address, XIIC_CR_REG_OFFSET);

        if ((byte_count == 1U) && (option == XIIC_STOP)) {
            /* Last byte is already clocked in and was not acknowledged: drop
             * MSMS so the controller can put a STOP on the bus. */
            XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                          XIIC_CR_ENABLE_DEVICE_MASK);
        }

        if (byte_count == 2U) {
            /* NO-ACK must be armed before the last byte leaves the FIFO. */
            XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                          control_reg | XIIC_CR_NO_ACK_MASK);
        }

        *buffer = (u8)XIic_ReadReg(base_address, XIIC_DRR_REG_OFFSET);
        ++buffer;

        if ((byte_count == 1U) && (option == XIIC_REPEATED_START)) {
            XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                          XIIC_CR_ENABLE_DEVICE_MASK | XIIC_CR_MSMS_MASK |
                          XIIC_CR_REPEATED_START_MASK);
        }

        XIic_ClearIisr(base_address, XIIC_INTR_RX_FULL_MASK |
                       XIIC_INTR_TX_ERROR_MASK | XIIC_INTR_ARB_LOST_MASK);
        --byte_count;
    }

    if (option == XIIC_STOP) {
        iic_polled_status_t status =
            wait_isr(base_address, deadline, XIIC_INTR_BNB_MASK, 0U);

        if (status != IIC_POLLED_OK) {
            *status_out = status;
        }
    }

    return byte_count;
}

unsigned iic_polled_send(UINTPTR base_address, u8 address, const u8 *buffer,
                         unsigned byte_count, u8 option, uint32_t timeout_us)
{
    iic_deadline_t deadline;
    iic_polled_status_t status;
    uint32_t control_reg;
    unsigned remaining;

    if ((buffer == NULL) || (byte_count == 0U)) {
        last_status = IIC_POLLED_ERR_INVALID_ARG;
        return 0U;
    }

    deadline_start(&deadline, timeout_us);

    control_reg = XIic_ReadReg(base_address, XIIC_CR_REG_OFFSET);
    if ((control_reg & XIIC_CR_REPEATED_START_MASK) == 0U) {
        status = wait_bus_free(base_address, &deadline);
        if (status != IIC_POLLED_OK) {
            last_status = status;
            return 0U;
        }

        XIic_Send7BitAddress(base_address, address, XIIC_WRITE_OPERATION);
        XIic_ClearIisr(base_address, XIIC_INTR_TX_EMPTY_MASK |
                       XIIC_INTR_TX_ERROR_MASK | XIIC_INTR_ARB_LOST_MASK);
        XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                      XIIC_CR_MSMS_MASK | XIIC_CR_DIR_IS_TX_MASK |
                      XIIC_CR_ENABLE_DEVICE_MASK);

        /* BNB is only meaningful to clear while the bus is actually busy. */
        status = wait_bus_busy(base_address, &deadline, true);
        if (status != IIC_POLLED_OK) {
            last_status = status;
            return 0U;
        }
        XIic_ClearIisr(base_address, XIIC_INTR_BNB_MASK);
    } else {
        /* Repeated start: this core already owns the bus, so waiting for it
         * to go free here would deadlock by construction. */
        XIic_Send7BitAddress(base_address, address, XIIC_WRITE_OPERATION);
    }

    remaining = send_data(base_address, buffer, byte_count, option, &deadline,
                          &status);

    control_reg = XIic_ReadReg(base_address, XIIC_CR_REG_OFFSET);
    if ((control_reg & XIIC_CR_REPEATED_START_MASK) == 0U) {
        if ((control_reg & XIIC_CR_MSMS_MASK) != 0U) {
            XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                          control_reg & ~XIIC_CR_MSMS_MASK);
        }
        if ((XIic_ReadReg(base_address, XIIC_SR_REG_OFFSET) &
             XIIC_SR_ADDR_AS_SLAVE_MASK) != 0U) {
            XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET, 0U);
        } else {
            iic_polled_status_t free_status =
                wait_bus_busy(base_address, &deadline, false);

            if ((free_status != IIC_POLLED_OK) && (status == IIC_POLLED_OK)) {
                status = free_status;
            }
        }
    }

    last_status = status;
    return byte_count - remaining;
}

unsigned iic_polled_recv(UINTPTR base_address, u8 address, u8 *buffer,
                         unsigned byte_count, u8 option, uint32_t timeout_us)
{
    iic_deadline_t deadline;
    iic_polled_status_t status;
    uint32_t control_reg;
    unsigned remaining;

    if ((buffer == NULL) || (byte_count == 0U)) {
        last_status = IIC_POLLED_ERR_INVALID_ARG;
        return 0U;
    }

    deadline_start(&deadline, timeout_us);

    XIic_ClearIisr(base_address, XIIC_INTR_RX_FULL_MASK |
                   XIIC_INTR_TX_ERROR_MASK | XIIC_INTR_ARB_LOST_MASK);
    /* Receive FIFO occupancy depth of one byte (zero based). */
    XIic_WriteReg(base_address, XIIC_RFD_REG_OFFSET, 0U);

    control_reg = XIic_ReadReg(base_address, XIIC_CR_REG_OFFSET);
    if ((control_reg & XIIC_CR_REPEATED_START_MASK) == 0U) {
        status = wait_bus_free(base_address, &deadline);
        if (status != IIC_POLLED_OK) {
            last_status = status;
            return 0U;
        }

        XIic_Send7BitAddress(base_address, address, XIIC_READ_OPERATION);

        /* MSMS is set after the address is in the FIFO. A single-byte read
         * must arm NO-ACK before the address goes out. */
        control_reg = XIIC_CR_MSMS_MASK | XIIC_CR_ENABLE_DEVICE_MASK;
        if (byte_count == 1U) {
            control_reg |= XIIC_CR_NO_ACK_MASK;
        }
        XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET, control_reg);

        status = wait_bus_busy(base_address, &deadline, true);
        if (status != IIC_POLLED_OK) {
            last_status = status;
            return 0U;
        }
        XIic_ClearIisr(base_address, XIIC_INTR_BNB_MASK);
    } else {
        control_reg &= ~XIIC_CR_DIR_IS_TX_MASK;
        if (byte_count == 1U) {
            control_reg |= XIIC_CR_NO_ACK_MASK;
        }
        XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET, control_reg);
        XIic_Send7BitAddress(base_address, address, XIIC_READ_OPERATION);
    }

    remaining = recv_data(base_address, buffer, byte_count, option, &deadline,
                          &status);

    control_reg = XIic_ReadReg(base_address, XIIC_CR_REG_OFFSET);
    if ((control_reg & XIIC_CR_REPEATED_START_MASK) == 0U) {
        XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET, 0U);
    }

    if (option == XIIC_STOP) {
        iic_polled_status_t free_status = wait_bus_free(base_address, &deadline);

        if ((free_status != IIC_POLLED_OK) && (status == IIC_POLLED_OK)) {
            status = free_status;
        }
    }

    last_status = status;
    return byte_count - remaining;
}

iic_polled_status_t iic_polled_last_status(void)
{
    return last_status;
}

const char *iic_polled_status_string(iic_polled_status_t status)
{
    static const char *const messages[] = {
        "ok",
        "bus-busy",
        "timeout",
        "arb-lost",
        "tx-error",
        "invalid-argument"
    };

    if ((unsigned)status >= (sizeof(messages) / sizeof(messages[0]))) {
        return "unknown";
    }
    return messages[status];
}
