#ifndef ZMPIO_STATS_H
#define ZMPIO_STATS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Step 7.4 (docs/PLAN_BUOC_7.md SS4 7.4): the two timing numbers the soak
 * gate asks for, measured entirely inside the Linux clock domain.
 *
 * What this CAN measure honestly:
 *   - wake latency: CLOCK_MONOTONIC from poll() returning with the doorbell
 *     readable to the drain of that wakeup finishing.  Both timestamps come
 *     from the same clock in the same process.
 *   - inter-arrival: the interval between consecutive FEATURE_V2 messages,
 *     expected ~640 ms (window 128 / hop 64 at 100 Hz).
 *
 * What it CANNOT measure, and does not pretend to: end-to-end latency from
 * the PL producing a frame to userspace holding it.  CPU1 and Linux share no
 * clock, so subtracting CPU1's platform_time_us() from a host timestamp
 * would produce a number with no defined meaning.  The plan's optional
 * GPIO+scope method is the only real answer there and is explicitly not a
 * gate.
 *
 * Percentiles come from a power-of-two histogram rather than a sorted sample
 * buffer: an 8-hour soak must not accumulate memory, and a bucket edge is an
 * honest answer as long as the edge is reported with the number, which
 * zmpio_stats_format() does.
 */

#define ZMPIO_STATS_BUCKETS 32U

typedef struct {
    uint64_t count;
    uint64_t sum_us;
    uint64_t min_us;
    uint64_t max_us;
    /* bucket 0 holds 0 us; bucket i>0 holds [2^(i-1), 2^i) us, and the last
     * one is open-ended so a stall of any size is recorded somewhere. */
    uint64_t bucket[ZMPIO_STATS_BUCKETS];
} zmpio_stats_hist_t;

void zmpio_stats_reset(zmpio_stats_hist_t *hist);
void zmpio_stats_add(zmpio_stats_hist_t *hist, uint64_t value_us);

/* Upper edge, in microseconds, of the bucket the requested percentile falls
 * in -- i.e. "at least `percent`% of samples were below this".  Returns 0
 * when no samples have been recorded. */
uint64_t zmpio_stats_percentile_us(const zmpio_stats_hist_t *hist,
                                   unsigned percent);

/* "n=<count> min=<us> p50=<us> p99=<us> max=<us> mean=<us>" into `out`. */
void zmpio_stats_format(const zmpio_stats_hist_t *hist, char *out,
                        size_t out_size);

#endif
