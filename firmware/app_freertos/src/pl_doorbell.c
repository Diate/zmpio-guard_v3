#include "pl_doorbell.h"

#include "xil_io.h"
#include "zmpio_doorbell_regs.h"

void pl_doorbell_ring(void)
{
    Xil_Out32(PL_DOORBELL_BASEADDR + ZMPIO_DOORBELL_REG_SET,
               ZMPIO_DOORBELL_BIT_SET);
}
