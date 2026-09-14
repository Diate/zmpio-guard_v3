#include "cpu0_irq_handler.h"

#include "pl_doorbell.h"
#include "xil_printf.h"
#include "xinterrupt_wrap.h"
#include "xstatus.h"

/* GIC distributor base address -- the physical SoC distributor is a single,
 * shared MMIO block both CPUs' local GIC CPU interfaces sit behind (UG585
 * Appendix B); same constant fpga_dsp_hal.c uses on the CPU1 side for the
 * same physical register. XSetupInterruptSystem()'s "IntrParent" argument
 * wants this distributor base address for a plain SPI with no Xilinx driver
 * Config struct of its own (mirrors fpga_dsp_hal_irq_program()'s call). */
#define CPU0_IRQ_GIC_DIST_BASEADDR  0xF8F01000U
#define CPU0_IRQ_DOORBELL_LOCAL_ID  (PL_DOORBELL_IRQ_ID - 32U)

/* Bounded retry for the ACK+recheck race window (see cpu0_irq_handler.h) --
 * not a timeout, just a cap on how many times a genuinely back-to-back SET
 * can plausibly land inside this tiny window before something else is
 * wrong. Kept small and diagnostic-only: hitting the cap just means the
 * source is left masked for one more main-loop iteration, never a hang. */
#define CPU0_IRQ_DOORBELL_MAX_ACK_ATTEMPTS  8U

static volatile int      doorbell_service_pending = 0;
static volatile uint32_t doorbell_isr_count = 0U;
static volatile int      doorbell_spurious_detected = 0;

static void cpu0_irq_handler_doorbell_isr(void *callback_ref)
{
    (void)callback_ref;

    if (!pl_doorbell_pending()) {
        /* GIC delivered this ISR without irq_out actually asserted (or
         * DBELL_IRQ_ENABLE was already 0) -- should be unreachable given
         * zmpio_doorbell.v's irq_out = IRQ_ENABLE && PENDING. Verifying the
         * PL register directly, rather than trusting that assumption, is
         * what catches a GIC enable-bit mismatch on this SPI (see the
         * level-IRQ discipline in docs/SDD/SDD_10_DOORBELL.md). Mask anyway
         * (see below) so a genuinely stuck source cannot storm forever. */
        doorbell_spurious_detected = 1;
    }

    /* Mask the source in the ISR itself, before anything else -- the same
     * mask-before-anything-else rule documented for zmpio_dsp_ctrl
     * (docs/SDD/SDD_06_PL_DSP_SHELL.md). Cheap here (bare-metal CPU0, no
     * scheduler/task deferral needed for a single MMIO write). */
    pl_doorbell_irq_disable();
    doorbell_isr_count++;
    doorbell_service_pending = 1;
}

void cpu0_irq_handler_init(void)
{
    int status;

    pl_doorbell_irq_disable();

    status = XSetupInterruptSystem(NULL, (void *)cpu0_irq_handler_doorbell_isr,
                                   CPU0_IRQ_DOORBELL_LOCAL_ID,
                                   CPU0_IRQ_GIC_DIST_BASEADDR,
                                   XINTERRUPT_DEFAULT_PRIORITY);
    if (status != XST_SUCCESS) {
        xil_printf("CPU0: doorbell XSetupInterruptSystem failed (status=%d)\r\n", status);
        return;
    }

    pl_doorbell_irq_enable();
    xil_printf("CPU0: doorbell IRQ armed (GIC SPI %lu)\r\n",
               (unsigned long)PL_DOORBELL_IRQ_ID);
}

void cpu0_irq_handler_service(void)
{
    uint32_t attempts;

    if (!doorbell_service_pending) {
        return;
    }
    doorbell_service_pending = 0;

    attempts = 0U;
    do {
        pl_doorbell_ack();
        ++attempts;
    } while (pl_doorbell_pending() && (attempts < CPU0_IRQ_DOORBELL_MAX_ACK_ATTEMPTS));

    if (attempts >= CPU0_IRQ_DOORBELL_MAX_ACK_ATTEMPTS) {
        xil_printf("CPU0: doorbell still PENDING after %lu ACK attempts -- "
                   "leaving masked, will retry next main-loop iteration\r\n",
                   (unsigned long)CPU0_IRQ_DOORBELL_MAX_ACK_ATTEMPTS);
        doorbell_service_pending = 1; /* try again next loop iteration */
        return;
    }

    pl_doorbell_irq_enable();
}

uint32_t cpu0_irq_handler_get_isr_count(void)
{
    return doorbell_isr_count;
}

bool cpu0_irq_handler_get_spurious_detected(void)
{
    return doorbell_spurious_detected != 0;
}
