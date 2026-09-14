#ifndef PL_DOORBELL_H
#define PL_DOORBELL_H

/*
 * CPU1-side driver for zmpio_doorbell.v, the one-way CPU1 -> CPU0 wakeup
 * doorbell. CPU1 only ever writes DBELL_SET here; it never enables or listens
 * for the IRQ, which lives entirely in pl_doorbell.c/cpu0_irq_handler.c on
 * CPU0.
 *
 * The base address comes from the zmpio_doorbell_0 reg property in
 * firmware/platform_dual/hw/sdt/pl.dtsi and must be re-checked after any
 * block-design rebuild.
 */
#define PL_DOORBELL_BASEADDR  0x40002000U

/* Writes DBELL_SET, bumping zmpio_doorbell's PENDING/DBELL_COUNT and (if
 * CPU0 has DBELL_IRQ_ENABLE set) asserting its GIC SPI. Called once per
 * successful ipc_v3.c push_response() -- see that call site's comment.
 * Never blocks, touches no ABI v3 shared-DDR state; safe from any task
 * context. */
void pl_doorbell_ring(void);

#endif
