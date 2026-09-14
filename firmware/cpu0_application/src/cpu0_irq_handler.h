#ifndef CPU0_IRQ_HANDLER_H
#define CPU0_IRQ_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * CPU0-side GIC wiring for zmpio_doorbell.v's irq_out (Step 5, docs/ROADMAP.md
 * STEP 5, docs/sdd_sad/SDD_DOORBELL.md). Mirrors fpga_dsp_hal.c's IRQ
 * discipline on CPU1 (mask-in-ISR, drain, ACK+recheck, re-enable) adapted to
 * a bare-metal main loop instead of a FreeRTOS task/semaphore: the ISR sets a
 * flag, cpu0_irq_handler_service() (called from main()'s existing for(;;)
 * loop, alongside net_poll()) does the actual drain/ACK/re-enable work.
 */

/*
 * Must be called AFTER net_init() has already run its own
 * XSetupInterruptSystem() call (net_lwip.c) -- that is the call that forces
 * CPU0's own first DoDistributorInit(), and this driver deliberately never
 * wants to be the one to trigger it. This doorbell SPI is owned solely by
 * CPU0, so keeping every GIC-touching call strictly after CPU0's own first
 * distributor init avoids ever depending on DoDistributorInit()'s
 * destructive full-reset path running a second time.
 */
void cpu0_irq_handler_init(void);

/* Call once per main-loop iteration (same cadence as net_poll()). No-op
 * unless the ISR has set its pending flag; when it has, ACKs zmpio_doorbell
 * (retrying a bounded number of times if a new DBELL_SET races in between
 * ACK and the STATUS recheck -- docs/sdd_sad/SDD_DOORBELL.md's
 * ACK-after-drain-and-recheck discipline) and only then re-enables
 * DBELL_IRQ_ENABLE. */
void cpu0_irq_handler_service(void);

/* Diagnostics for the doorbell test scenarios in main.c
 * (run_doorbell_fault_test()): counts real ISR entries (distinct wakeups),
 * separate from zmpio_doorbell's own DBELL_COUNT (total DBELL_SET writes) --
 * comparing the two is how the stress tests confirm 0 lost wakeups without
 * losing an IRQ storm to coalescing (a level IRQ can legitimately produce
 * fewer ISR entries than DBELL_SET writes if several SETs land while the
 * ISR is still masked -- that is correct behavior, not a lost wakeup, since
 * DBELL_COUNT is what actually proves no SET was silently dropped by the PL
 * register itself). */
uint32_t cpu0_irq_handler_get_isr_count(void);

/* True if the ISR has fired at least once with DBELL_IRQ_ENABLE already 0
 * (i.e. the GIC re-entered an ISR whose source should have stayed masked) --
 * used by the spurious-IRQ test scenario. Always false in normal operation. */
bool cpu0_irq_handler_get_spurious_detected(void);

#endif
