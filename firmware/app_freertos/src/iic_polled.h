#ifndef IIC_POLLED_H
#define IIC_POLLED_H

#include <stdbool.h>
#include <stdint.h>

#include "xil_types.h"

/*
 * Bounded replacement for the BSP's polled AXI IIC transport
 * (bsp/libsrc/iic/src/xiic_l.c: XIic_Send()/XIic_Recv()).
 *
 * Why this exists
 * ---------------
 * The BSP transport contains hardware-polling loops with NO timeout and, in
 * one case, no error exit at all:
 *
 *   xiic_l.c:175 / :437   while ((StatusReg & XIIC_SR_BUS_BUSY_MASK) == 0)
 *   xiic_l.c:482          while ((StatusReg & XIIC_SR_BUS_BUSY_MASK) != 0)
 *   xiic_l.c:576          while (1) { if (IntrStatus & TX_EMPTY) break; }
 *   plus the BNB spins at the end of SendData()/RecvData()
 *
 * The :576 loop is on the XIIC_REPEATED_START path, which is exactly the
 * path mpu6050_read_register()/mpu6050_read_sample() take, and it breaks
 * only on TX_EMPTY -- it has no ARB_LOST/TX_ERROR/BNB escape like its
 * sibling at :527. A slave that wedges the bus (MPU6050 stranded mid-byte,
 * typically after the master is reset in the middle of a transaction)
 * therefore spins sensor_task forever inside the driver: mpu6050_read_sample()
 * never returns, so consecutive_errors never increments, the bus-recovery and
 * power-cycle paths in sensor_task never run, and vTaskDelayUntil() is never
 * reached. That is a genuine, unrecoverable hang, not slow progress.
 *
 * The register sequence below is transcribed from those same BSP functions so
 * the on-the-wire behaviour is unchanged; the only differences are that every
 * wait carries a deadline (global timer, platform_time.h) and every wait has
 * an error exit. A timed-out or errored transfer returns short, which the
 * caller reports as a failed sample -- i.e. it feeds the recovery path that
 * already exists instead of deadlocking before it.
 *
 * Option values are the BSP's XIIC_STOP / XIIC_REPEATED_START.
 */

typedef enum {
    IIC_POLLED_OK = 0,
    IIC_POLLED_ERR_BUS_BUSY,      /* bus never went free before the transfer */
    IIC_POLLED_ERR_TIMEOUT,       /* a wait inside the transfer hit its deadline */
    IIC_POLLED_ERR_ARB_LOST,      /* another master, or SDA held low */
    IIC_POLLED_ERR_TX_ERROR,      /* slave did not ACK */
    IIC_POLLED_ERR_INVALID_ARG
} iic_polled_status_t;

/*
 * Both return the number of bytes actually transferred (0 on a failure to
 * start), matching XIic_Send()/XIic_Recv() so callers need no restructuring.
 * `timeout_us` bounds the whole transfer, not each byte.
 */
unsigned iic_polled_send(UINTPTR base_address, u8 address, const u8 *buffer,
                         unsigned byte_count, u8 option, uint32_t timeout_us);
unsigned iic_polled_recv(UINTPTR base_address, u8 address, u8 *buffer,
                         unsigned byte_count, u8 option, uint32_t timeout_us);

/* Outcome of the most recent iic_polled_send()/iic_polled_recv() call, for
 * logging and for distinguishing "slave NAK" from "bus wedged". */
iic_polled_status_t iic_polled_last_status(void);
const char *iic_polled_status_string(iic_polled_status_t status);

#endif
