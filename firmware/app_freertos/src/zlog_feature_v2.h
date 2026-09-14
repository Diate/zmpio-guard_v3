#ifndef ZLOG_FEATURE_V2_H
#define ZLOG_FEATURE_V2_H

#include <stddef.h>
#include <stdint.h>

#include "fpga_dsp_hal.h"
#include "log_record.h"
#include "zmpio_protocol.h"

/*
 * Feature-logging payload definitions. Each struct here is the
 * `payload` of a log_queue_item_t written verbatim by storage_task.c under
 * the matching log_source_t from log_record.h -- storage_task itself does
 * not need to know these layouts, it only copies payload_len bytes.
 *
 * All three types are versioned independently (RECORD version fields below,
 * distinct from LOG_FORMAT_VERSION in log_record.h which covers the
 * container format only) so a rule/model change can be told apart from a
 * container change when replaying old captures.
 */

/* FEATURE_V2: mirrors fpga_feature_frame_t field-for-field, 48 bytes, no
 * padding (all uint32_t members) -- logged as-is, no separate struct. */
_Static_assert(sizeof(fpga_feature_frame_t) == 48U,
               "FEATURE_V2 payload size changed -- update "
               "software/linux/parse_zlog.py alongside it");

#define ZLOG_ANOMALY_RULE_V1_VERSION 1U

typedef enum {
    ZLOG_RULE_METRIC_RMS = 1
} zlog_rule_metric_t;

/* One ANOMALY_RULE_V1 record per FEATURE_V2 frame (not just on a positive
 * verdict), so a host replay can always correlate FEATURE_V2 -> rule score
 * 1:1 via frame_sequence. */
typedef struct __attribute__((packed)) {
    uint8_t  rule_version;   /* ZLOG_ANOMALY_RULE_V1_VERSION */
    uint8_t  metric_id;      /* zlog_rule_metric_t */
    uint8_t  verdict;        /* 1 = anomaly, 0 = normal */
    uint8_t  reserved0;
    uint32_t frame_sequence; /* == fpga_feature_frame_t.frame_sequence */
    uint32_t value_q24_8;    /* metric value, same Q24.8 scale as the frame */
    uint32_t threshold_q24_8;
} zlog_anomaly_rule_v1_t;

_Static_assert(sizeof(zlog_anomaly_rule_v1_t) == 16U,
               "ANOMALY_RULE_V1 payload size changed -- update host tooling");

/*
 * common/zmpio_protocol.h carries wire copies of the two structs above so
 * that Linux can decode the
 * feature stream without including any firmware header.  These asserts are
 * the contract that keeps the copies honest -- add, drop or reorder a field
 * on either side and the CPU1 build fails here rather than shipping a Linux
 * decoder that silently reads the wrong offsets (REQ-OPS-002).  This file is
 * the right place for them because it is the only one that sees both
 * definitions.
 */
_Static_assert(sizeof(ipc_feature_frame_v2_t) == sizeof(fpga_feature_frame_t),
               "wire/firmware FEATURE_V2 size drift");
_Static_assert(offsetof(ipc_feature_frame_v2_t, frame_sequence) ==
               offsetof(fpga_feature_frame_t, frame_sequence),
               "wire/firmware FEATURE_V2 field drift: frame_sequence");
_Static_assert(offsetof(ipc_feature_frame_v2_t, window_end_sample_sequence) ==
               offsetof(fpga_feature_frame_t, window_end_sample_sequence),
               "wire/firmware FEATURE_V2 field drift: window_end_sample_sequence");
_Static_assert(offsetof(ipc_feature_frame_v2_t, rms_q24_8) ==
               offsetof(fpga_feature_frame_t, rms_q24_8),
               "wire/firmware FEATURE_V2 field drift: rms_q24_8");
_Static_assert(offsetof(ipc_feature_frame_v2_t, peak_q24_8) ==
               offsetof(fpga_feature_frame_t, peak_q24_8),
               "wire/firmware FEATURE_V2 field drift: peak_q24_8");
_Static_assert(offsetof(ipc_feature_frame_v2_t, variance_q32_0) ==
               offsetof(fpga_feature_frame_t, variance_q32_0),
               "wire/firmware FEATURE_V2 field drift: variance_q32_0");
_Static_assert(offsetof(ipc_feature_frame_v2_t, kurtosis_q16_16) ==
               offsetof(fpga_feature_frame_t, kurtosis_q16_16),
               "wire/firmware FEATURE_V2 field drift: kurtosis_q16_16");
_Static_assert(offsetof(ipc_feature_frame_v2_t, dominant_frequency_q16_16) ==
               offsetof(fpga_feature_frame_t, dominant_frequency_q16_16),
               "wire/firmware FEATURE_V2 field drift: dominant_frequency_q16_16");
_Static_assert(offsetof(ipc_feature_frame_v2_t, dominant_power_q32_0) ==
               offsetof(fpga_feature_frame_t, dominant_power_q32_0),
               "wire/firmware FEATURE_V2 field drift: dominant_power_q32_0");
_Static_assert(offsetof(ipc_feature_frame_v2_t, band_energy_q32_0) ==
               offsetof(fpga_feature_frame_t, band_energy_q32_0),
               "wire/firmware FEATURE_V2 field drift: band_energy_q32_0");

_Static_assert(sizeof(ipc_anomaly_rule_v1_t) == sizeof(zlog_anomaly_rule_v1_t),
               "wire/firmware ANOMALY_RULE_V1 size drift");
_Static_assert(offsetof(ipc_anomaly_rule_v1_t, rule_version) ==
               offsetof(zlog_anomaly_rule_v1_t, rule_version),
               "wire/firmware ANOMALY_RULE_V1 field drift: rule_version");
_Static_assert(offsetof(ipc_anomaly_rule_v1_t, metric_id) ==
               offsetof(zlog_anomaly_rule_v1_t, metric_id),
               "wire/firmware ANOMALY_RULE_V1 field drift: metric_id");
_Static_assert(offsetof(ipc_anomaly_rule_v1_t, verdict) ==
               offsetof(zlog_anomaly_rule_v1_t, verdict),
               "wire/firmware ANOMALY_RULE_V1 field drift: verdict");
_Static_assert(offsetof(ipc_anomaly_rule_v1_t, frame_sequence) ==
               offsetof(zlog_anomaly_rule_v1_t, frame_sequence),
               "wire/firmware ANOMALY_RULE_V1 field drift: frame_sequence");
_Static_assert(offsetof(ipc_anomaly_rule_v1_t, value_q24_8) ==
               offsetof(zlog_anomaly_rule_v1_t, value_q24_8),
               "wire/firmware ANOMALY_RULE_V1 field drift: value_q24_8");
_Static_assert(offsetof(ipc_anomaly_rule_v1_t, threshold_q24_8) ==
               offsetof(zlog_anomaly_rule_v1_t, threshold_q24_8),
               "wire/firmware ANOMALY_RULE_V1 field drift: threshold_q24_8");

#define ZLOG_DSP_HEALTH_VERSION 1U

typedef enum {
    ZLOG_DSP_HEALTH_OK = 0,
    ZLOG_DSP_HEALTH_DEGRADED = 1
} zlog_dsp_health_state_t;

typedef enum {
    ZLOG_DSP_FAULT_NONE = 0,
    ZLOG_DSP_FAULT_CTRL_FIFO_DROP = 1,   /* zmpio_dsp_ctrl DROP_COUNT moved */
    ZLOG_DSP_FAULT_BRIDGE_SAMPLE_DROP = 2, /* mmio_axis_bridge DROP_COUNT moved */
    ZLOG_DSP_FAULT_RESULT_OVERFLOW = 3   /* zmpio_dsp_ctrl STATUS bit1 */
} zlog_dsp_fault_t;

/* Emitted on every OK<->DEGRADED transition, plus a periodic heartbeat while
 * DEGRADED (see APP_DSP_HEALTH_DEGRADED_LOG_INTERVAL_MS) so a host replay can
 * see how long a degraded window lasted without needing every ZLOG record. */
typedef struct __attribute__((packed)) {
    uint8_t  health_version; /* ZLOG_DSP_HEALTH_VERSION */
    uint8_t  state;          /* zlog_dsp_health_state_t */
    uint8_t  fault;          /* zlog_dsp_fault_t, cause of last transition */
    uint8_t  reserved0;
    uint32_t consecutive_fault_count;
    uint32_t feature_count;
    uint32_t ctrl_drop_count;
    uint32_t bridge_drop_count;
} zlog_dsp_health_t;

_Static_assert(sizeof(zlog_dsp_health_t) == 20U,
               "DSP_HEALTH payload size changed -- update host tooling");

/* Same contract for the DSP_HEALTH stream message: its trailing 20 bytes
 * are this struct field-for-field, which is what lets zmpiod write the
 * received message straight out as a LOG_SOURCE_DSP_HEALTH ZLOG record that
 * parse_zlog.py decodes with the existing DSP_HEALTH_PAYLOAD layout. */
#define ZLOG_DSP_HEALTH_STREAM_TRAILER \
    offsetof(ipc_dsp_health_stream_v1_t, health_version)

_Static_assert(sizeof(ipc_dsp_health_stream_v1_t) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER + sizeof(zlog_dsp_health_t)),
               "DSP health stream trailer is no longer exactly zlog_dsp_health_t");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, health_version) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, health_version)),
               "DSP health stream field drift: health_version");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, state) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, state)),
               "DSP health stream field drift: state");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, fault) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, fault)),
               "DSP health stream field drift: fault");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, reserved0) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, reserved0)),
               "DSP health stream field drift: reserved0");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, consecutive_fault_count) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, consecutive_fault_count)),
               "DSP health stream field drift: consecutive_fault_count");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, feature_count) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, feature_count)),
               "DSP health stream field drift: feature_count");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, ctrl_drop_count) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, ctrl_drop_count)),
               "DSP health stream field drift: ctrl_drop_count");
_Static_assert(offsetof(ipc_dsp_health_stream_v1_t, bridge_drop_count) ==
               (ZLOG_DSP_HEALTH_STREAM_TRAILER +
                offsetof(zlog_dsp_health_t, bridge_drop_count)),
               "DSP health stream field drift: bridge_drop_count");

#endif
