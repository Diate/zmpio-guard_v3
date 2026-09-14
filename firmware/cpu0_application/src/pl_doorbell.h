#ifndef PL_DOORBELL_H
#define PL_DOORBELL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * CPU0-side driver for zmpio_doorbell.v (Step 5, docs/ROADMAP.md STEP 5,
 * docs/sdd_sad/SDD_DOORBELL.md) -- the wakeup end of the one-way CPU1->CPU0
 * doorbell. cpu0_irq_handler.c is the only normal caller of the
 * status/ack/irq_enable functions here (from its ISR and its
 * cpu0_irq_handler_service() drain loop); main.c's doorbell stress test
 * additionally calls pl_doorbell_test_ring() to exercise the PL+GIC+ISR path
 * in isolation from CPU1/ABI v3 -- see that function's comment.
 *
 * Base address and IRQ id come from firmware/platform_dual/hw/sdt/pl.dtsi:
 *   zmpio_doorbell_0: zmpio_doorbell@40002000 { interrupts = <0 31 4>; ... }
 * i.e. reg base 0x4000_2000, GIC SPI = 31 + 32 = 63 (UG585 Table B-1,
 * `<type IRQ_M flags>` devicetree encoding, type 0 = SPI) -- matching the
 * xlconcat_0/In2 position add_zmpio_doorbell.tcl assigns (axi_iic_0 on
 * In0 = SPI 61, zmpio_dsp_ctrl_0 on In1 = SPI 62). See
 * docs/sdd_sad/SDD_DOORBELL.md for the full IRQ concat layout.
 */
#define PL_DOORBELL_BASEADDR  0x40002000U

/* GIC SPI id for zmpio_doorbell_0/irq_out -- see the comment above. */
#define PL_DOORBELL_IRQ_ID  63U

bool     pl_doorbell_pending(void);
uint32_t pl_doorbell_count(void);
void     pl_doorbell_ack(void);
void     pl_doorbell_irq_enable(void);
void     pl_doorbell_irq_disable(void);

/* Lab-test-only: writes DBELL_SET directly from CPU0 itself, standing in for
 * CPU1's pl_doorbell_ring() (firmware/app_freertos/src/pl_doorbell.c). Valid
 * because zmpio_doorbell_0's AXI4-Lite slave is reachable from both cores
 * over the same PS7 GP AXI path (see docs/sdd_sad/SDD_DOORBELL.md) -- the
 * hardware never distinguishes which core issued a write. Used by
 * run_doorbell_fault_test() (main.c) to drive the 10.000-event and
 * doorbell-storm gates without needing CPU1/ABI v3 traffic to generate
 * volume, keeping those gates a test of the PL+GIC+ISR mechanism alone.
 * Never called from production code -- CPU1 remains the only real ringer. */
void pl_doorbell_test_ring(void);

#endif
