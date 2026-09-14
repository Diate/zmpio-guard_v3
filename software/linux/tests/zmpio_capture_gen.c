/*
 * Step 7.2 T7.2-5 verification helper: drives zmpiod's real ZLOG capture
 * writer (zmpio_capture.c) with synthetic feature/rule/health records and
 * leaves the resulting STREAMnnnnn.ZBIN files in a directory.
 *
 * The point is NOT to check the writer against a second decoder written for
 * the occasion -- that would only prove the two agree with each other.  It
 * is to produce files that software/linux/parse_zlog.py, the same parser
 * used on every microSD capture in docs/architecture/evidence/, accepts with
 * zero errors.  tests/capture_roundtrip.py runs this and does exactly that.
 *
 *   ./zmpio_capture_gen <output-dir> [record-pairs] [max-bytes]
 *
 * A small max-bytes forces rotation, so the rotation path is covered too.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zmpio_capture.h"
#include "zmpio_protocol.h"

int main(int argc, char **argv)
{
    zmpio_capture_t capture;
    ipc_feature_stream_v1_t feature;
    ipc_dsp_health_stream_v1_t health;
    unsigned long pairs = 100UL;
    unsigned long long max_bytes = 4096ULL;
    unsigned long i;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <output-dir> [record-pairs] [max-bytes]\n",
                argv[0]);
        return 2;
    }
    if (argc >= 3) {
        pairs = strtoul(argv[2], NULL, 0);
    }
    if (argc >= 4) {
        max_bytes = strtoull(argv[3], NULL, 0);
    }

    if (zmpio_capture_open(&capture, argv[1], (uint64_t)max_bytes) != 0) {
        fprintf(stderr, "zmpio_capture_gen: cannot open capture in %s\n",
                argv[1]);
        return 1;
    }

    memset(&feature, 0, sizeof(feature));
    memset(&health, 0, sizeof(health));

    for (i = 0UL; i < pairs; ++i) {
        feature.stream_sequence = (uint32_t)(i + 1UL);
        feature.timestamp_us = 1000000ULL + (uint64_t)i * 640000ULL;
        feature.frame.frame_sequence = (uint32_t)i;
        feature.frame.window_end_sample_sequence = (uint32_t)(128UL + i * 64UL);
        feature.frame.rms_q24_8 = (uint32_t)(1200UL + i);
        feature.frame.peak_q24_8 = (uint32_t)(4096UL + i);
        feature.frame.variance_q32_0 = (uint32_t)(7UL * i);
        feature.frame.kurtosis_q16_16 = 3U << 16;
        feature.frame.dominant_frequency_q16_16 = 5U << 16;
        feature.frame.dominant_power_q32_0 = 9999U;
        feature.frame.band_energy_q32_0[0] = (uint32_t)i;
        feature.frame.band_energy_q32_0[3] = (uint32_t)(3UL * i);
        feature.rule.rule_version = 2U;
        feature.rule.metric_id = 1U;
        feature.rule.verdict = (uint8_t)((i % 10UL) == 0UL);
        feature.rule.frame_sequence = (uint32_t)i;
        feature.rule.value_q24_8 = feature.frame.rms_q24_8;
        feature.rule.threshold_q24_8 = 2351U;

        if (zmpio_capture_write(&capture, LOG_SOURCE_FEATURE_V2,
                                feature.timestamp_us, 0U, &feature.frame,
                                (uint16_t)sizeof(feature.frame)) != 0) {
            fprintf(stderr, "zmpio_capture_gen: feature write failed at %lu\n",
                    i);
            return 1;
        }
        if (zmpio_capture_write(&capture, LOG_SOURCE_ANOMALY_RULE_V1,
                                feature.timestamp_us,
                                (uint16_t)feature.rule.verdict, &feature.rule,
                                (uint16_t)sizeof(feature.rule)) != 0) {
            fprintf(stderr, "zmpio_capture_gen: rule write failed at %lu\n", i);
            return 1;
        }
    }

    health.stream_sequence = (uint32_t)(pairs + 1UL);
    health.timestamp_us = 1000000ULL + (uint64_t)pairs * 640000ULL;
    health.health_version = 1U;
    health.state = 1U;
    health.fault = 1U;
    health.consecutive_fault_count = 3U;
    health.feature_count = (uint32_t)pairs;
    health.ctrl_drop_count = 2U;
    health.bridge_drop_count = 0U;
    /* Same 20 bytes zmpiod writes: the tail of the stream message, which is
     * asserted to be exactly zlog_dsp_health_t (zlog_feature_v2.h). */
    if (zmpio_capture_write(&capture, LOG_SOURCE_DSP_HEALTH,
                            health.timestamp_us, health.state,
                            &health.health_version, 20U) != 0) {
        fprintf(stderr, "zmpio_capture_gen: health write failed\n");
        return 1;
    }

    printf("records=%llu rotations=%llu errors=%llu\n",
           (unsigned long long)capture.records_written,
           (unsigned long long)capture.rotations,
           (unsigned long long)capture.write_errors);
    zmpio_capture_close(&capture);
    return 0;
}
