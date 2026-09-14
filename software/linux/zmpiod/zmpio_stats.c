#define _GNU_SOURCE

#include "zmpio_stats.h"

#include <stdio.h>
#include <string.h>

void zmpio_stats_reset(zmpio_stats_hist_t *hist)
{
    if (hist == NULL) {
        return;
    }
    memset(hist, 0, sizeof(*hist));
    hist->min_us = UINT64_MAX;
}

void zmpio_stats_add(zmpio_stats_hist_t *hist, uint64_t value_us)
{
    unsigned bucket = 0U;
    uint64_t edge = 1U;

    if (hist == NULL) {
        return;
    }

    ++hist->count;
    hist->sum_us += value_us;
    if (value_us < hist->min_us) {
        hist->min_us = value_us;
    }
    if (value_us > hist->max_us) {
        hist->max_us = value_us;
    }

    /* bucket 0 covers [0, 2) us; the last bucket is open-ended so a stall of
     * any size lands somewhere rather than being dropped. */
    while ((bucket + 1U < ZMPIO_STATS_BUCKETS) && (value_us >= edge)) {
        edge <<= 1;
        ++bucket;
    }
    ++hist->bucket[bucket];
}

uint64_t zmpio_stats_percentile_us(const zmpio_stats_hist_t *hist,
                                   unsigned percent)
{
    uint64_t target;
    uint64_t seen = 0U;
    unsigned i;

    if ((hist == NULL) || (hist->count == 0U)) {
        return 0U;
    }
    if (percent > 100U) {
        percent = 100U;
    }

    /* Round up so p99 of 100 samples is the 99th, not the 98th. */
    target = ((hist->count * (uint64_t)percent) + 99U) / 100U;
    if (target == 0U) {
        target = 1U;
    }

    for (i = 0U; i < ZMPIO_STATS_BUCKETS; ++i) {
        seen += hist->bucket[i];
        if (seen >= target) {
            uint64_t edge = (uint64_t)1U << i;

            /* Upper edge of bucket i, clamped by the real maximum so the
             * reported figure can never exceed a value actually observed. */
            return (edge > hist->max_us) ? hist->max_us : edge;
        }
    }
    return hist->max_us;
}

void zmpio_stats_format(const zmpio_stats_hist_t *hist, char *out,
                        size_t out_size)
{
    if ((out == NULL) || (out_size == 0U)) {
        return;
    }
    if ((hist == NULL) || (hist->count == 0U)) {
        snprintf(out, out_size, "n=0");
        return;
    }
    snprintf(out, out_size,
             "n=%llu min=%llu p50=%llu p99=%llu max=%llu mean=%llu",
             (unsigned long long)hist->count,
             (unsigned long long)hist->min_us,
             (unsigned long long)zmpio_stats_percentile_us(hist, 50U),
             (unsigned long long)zmpio_stats_percentile_us(hist, 99U),
             (unsigned long long)hist->max_us,
             (unsigned long long)(hist->sum_us / hist->count));
}
