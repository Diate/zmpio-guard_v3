#ifndef ZMPIO_UIO_H
#define ZMPIO_UIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Internal UIO plumbing shared by the ABI v2 ring layer (zmpio_v2.c), the
 * ABI v3 command/response layer (zmpio_v3.c) and the doorbell wait path
 * (libzmpio.c) -- one copy of "find by name under /sys/class/uio, mmap
 * resource0, wait/ack/re-enable" instead of three, and the same discipline
 * tools/uio_spike.c already validates on real hardware in Step 6.2
 * (docs/PLAN_BUOC_6.md SS4 6.2) before this library exists.
 */

typedef struct {
    int      fd;
    int      uio_index;
    volatile uint8_t *base;
    size_t   size;
} zmpio_uio_region_t;

/* Looks up /sys/class/uio/uioN/name for a match against `uio_name`, opens
 * /dev/uioN and mmaps its resource0 (maps/map0) read-write. Returns 0 on
 * success; *out is only valid then. */
int zmpio_uio_open(const char *uio_name, zmpio_uio_region_t *out);

void zmpio_uio_close(zmpio_uio_region_t *region);

/*
 * Blocks until the region's IRQ fires (uio_pdrv_genirq blocking-read
 * protocol -- see tools/uio_spike.c's header comment for the exact
 * semantics this mirrors). Returns 0 on a real wakeup, -1 with errno set on
 * error, and 1 if interrupted by a signal (EINTR) so the caller can check
 * a shutdown flag without this function eating that signal silently.
 *
 * Does NOT re-enable the IRQ -- the caller must do so via
 * zmpio_uio_irq_reenable() only after it has finished draining/ACKing
 * whatever the IRQ signals, same ACK-before-re-enable ordering
 * tools/uio_spike.c documents and docs/PLAN_BUOC_6.md SS4 6.2 gates on.
 */
int zmpio_uio_irq_wait(const zmpio_uio_region_t *region);

/* Re-enables the region's UIO IRQ (write of a nonzero uint32_t). Call only
 * after the caller-side ACK is fully settled -- never before. */
int zmpio_uio_irq_reenable(const zmpio_uio_region_t *region);

#endif
