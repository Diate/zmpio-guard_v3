#include "pl_doorbell.h"

#include "xil_io.h"
#include "zmpio_doorbell_regs.h"

bool pl_doorbell_pending(void)
{
    return (Xil_In32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_STATUS) &
            ZMPIO_DOORBELL_BIT_PENDING) != 0U;
}

uint32_t pl_doorbell_count(void)
{
    return Xil_In32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_COUNT);
}

void pl_doorbell_ack(void)
{
    Xil_Out32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_ACK,
               ZMPIO_DOORBELL_BIT_ACK);
}

void pl_doorbell_irq_enable(void)
{
    Xil_Out32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_IRQ_ENABLE,
               ZMPIO_DOORBELL_BIT_IRQ_ENABLE);
}

void pl_doorbell_irq_disable(void)
{
    Xil_Out32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_IRQ_ENABLE, 0U);
}

void pl_doorbell_test_ring(void)
{
    Xil_Out32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_SET,
               ZMPIO_DOORBELL_BIT_SET);
}
