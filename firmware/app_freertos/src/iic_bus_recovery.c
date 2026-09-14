#include "iic_bus_recovery.h"

#include "sleep.h"
#include "xiic_l.h"

/* Bound every spin that the stock driver leaves unbounded (or bounded at
 * ~1 s) so a wedged bus can never hang this recovery attempt itself. */
#define RECOVERY_POLL_TIMEOUT_US   2000U
#define RECOVERY_POLL_STEP_US      10U
#define RECOVERY_ATTEMPT_GAP_US    1000U

static int poll_until(UINTPTR base_address, u32 offset, u32 mask,
                      int want_set)
{
    unsigned waited_us = 0U;

    for (;;) {
        u32 value = XIic_ReadReg(base_address, offset);
        int hit = (value & mask) != 0U;

        if (hit == want_set) {
            return 1;
        }
        if (waited_us >= RECOVERY_POLL_TIMEOUT_US) {
            return 0;
        }
        usleep(RECOVERY_POLL_STEP_US);
        waited_us += RECOVERY_POLL_STEP_US;
    }
}

static void force_one_start_attempt(UINTPTR base_address, u8 address)
{
    /* Mirrors the sequence XIic_Send() uses internally to issue a fresh
     * START + address byte (SendData() in xiic_l.c), minus its leading
     * XIic_WaitBusFree() gate -- that gate is exactly what leaves a wedged
     * bus untouched, since it refuses to drive anything while Bus Busy
     * reads stuck. */
    XIic_Send7BitAddress(base_address, address, XIIC_WRITE_OPERATION);
    XIic_ClearIisr(base_address, XIIC_INTR_TX_EMPTY_MASK |
                   XIIC_INTR_TX_ERROR_MASK | XIIC_INTR_ARB_LOST_MASK);
    XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                 XIIC_CR_MSMS_MASK | XIIC_CR_DIR_IS_TX_MASK |
                 XIIC_CR_ENABLE_DEVICE_MASK);

    /* Bounded window for the core to drive the address byte (real SCL
     * activity) before tearing the attempt down -- on a genuinely wedged
     * bus this may never report success, so do not spin forever. */
    (void)poll_until(base_address, XIIC_SR_REG_OFFSET,
                     XIIC_SR_BUS_BUSY_MASK, 1);
    (void)poll_until(base_address, XIIC_SR_REG_OFFSET,
                     XIIC_SR_TX_FIFO_EMPTY_MASK, 1);

    /* Best-effort STOP (drop MSMS) regardless of outcome. */
    XIic_WriteReg(base_address, XIIC_CR_REG_OFFSET,
                 XIIC_CR_DIR_IS_TX_MASK | XIIC_CR_ENABLE_DEVICE_MASK);
    (void)poll_until(base_address, XIIC_SR_REG_OFFSET,
                     XIIC_SR_BUS_BUSY_MASK, 0);
}

int iic_bus_force_recover(UINTPTR base_address, u8 address, unsigned attempts)
{
    unsigned i;

    XIic_WriteReg(base_address, XIIC_RESETR_OFFSET, XIIC_RESET_MASK);
    usleep(RECOVERY_ATTEMPT_GAP_US);

    for (i = 0U; i < attempts; ++i) {
        force_one_start_attempt(base_address, address);
        XIic_WriteReg(base_address, XIIC_RESETR_OFFSET, XIIC_RESET_MASK);
        usleep(RECOVERY_ATTEMPT_GAP_US);

        if ((XIic_ReadReg(base_address, XIIC_SR_REG_OFFSET) &
             XIIC_SR_BUS_BUSY_MASK) == 0U) {
            return 1;
        }
    }

    return (XIic_ReadReg(base_address, XIIC_SR_REG_OFFSET) &
            XIIC_SR_BUS_BUSY_MASK) == 0U;
}
