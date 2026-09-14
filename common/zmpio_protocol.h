#ifndef ZMPIO_PROTOCOL_H
#define ZMPIO_PROTOCOL_H

#include <stdint.h>

#define IPC_MAGIC              0x5A4D5049U
#define IPC_PROTOCOL_VERSION   2U
#define IPC_MESSAGE_SIZE       256U
#define IPC_PAYLOAD_SIZE       240U

typedef enum {
    MSG_TYPE_RAW_DATA    = 0x01,
    MSG_TYPE_PROCESSED   = 0x02,
    MSG_TYPE_SENSOR_DATA = 0x10,
    MSG_TYPE_LOG_DATA    = 0x20,
    MSG_TYPE_LOG_START   = 0x21,
    MSG_TYPE_LOG_STOP    = 0x22,
    MSG_TYPE_LOG_FLUSH   = 0x23,
    MSG_TYPE_LOG_STATUS  = 0x24,
    /* Optional diagnostic request.  LOG_STATUS remains ABI v2. */
    MSG_TYPE_LOG_DIAGNOSTIC = 0x25,
    /*
     * The CPU1 -> Linux telemetry stream rides the EXISTING ABI v2 TX ring as
     * two new message types rather than a third ring in the ABI v3 region, so
     * ZMPIO_ABI_V3_LAYOUT_HASH stays valid (ADR-018). Both are UNSOLICITED:
     * CPU1 publishes them with no request, which is why exactly one reader on
     * the TX ring is mandatory (REQ-STR-002).
     */
    MSG_TYPE_FEATURE_V2  = 0x30, /* payload: ipc_feature_stream_v1_t */
    MSG_TYPE_DSP_HEALTH  = 0x31, /* payload: ipc_dsp_health_stream_v1_t */
    /*
     * Request/response with a zero-length request, NOT part of the stream. It
     * gives the Linux side CPU1's own view of the stream counters, so
     * "records received + dropped_since_last == feature_count" can be checked
     * against the producer instead of inferred from the consumer alone.
     */
    MSG_TYPE_STREAM_STATUS = 0x32, /* payload: ipc_stream_status_t */
    MSG_TYPE_ACK         = 0x7E,
    MSG_TYPE_ERROR       = 0x7F,
    MSG_TYPE_HEARTBEAT   = 0xFF
} msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t type;
    uint32_t timestamp;
    uint32_t length;
} msg_header_t;

typedef struct __attribute__((packed)) {
    msg_header_t header;
    uint8_t payload[IPC_PAYLOAD_SIZE];
} ipc_message_t;

typedef struct __attribute__((packed)) {
    uint32_t request_type;
    int32_t status;
    uint32_t detail;
} ipc_ack_payload_t;

/*
 * Raw MPU6050 register values, shared by the CPU1 firmware and Linux.
 * Conversion is deliberately performed at the consumer because the selected
 * full-scale ranges are a CPU1 configuration detail.
 */
typedef struct __attribute__((packed)) {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t temperature;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} ipc_mpu6050_sample_t;

/* Response payload for a zero-length MSG_TYPE_SENSOR_DATA request. */
typedef struct __attribute__((packed)) {
    uint64_t sample_timestamp_us;
    ipc_mpu6050_sample_t sample;
} ipc_sensor_data_t;

/* Payload returned for MSG_TYPE_LOG_STATUS. */
typedef struct __attribute__((packed)) {
    uint32_t protocol_version;
    uint32_t mounted;
    uint32_t logging;
    uint32_t queue_depth;
    uint32_t sensor_accepted;
    uint32_t linux_accepted;
    uint32_t sensor_dropped;
    uint32_t linux_dropped;
    uint32_t records_written;
    uint32_t io_errors;
    uint64_t file_bytes;
    char filename[16];
} ipc_logger_status_t;

/*
 * Storage diagnostic returned by MSG_TYPE_LOG_DIAGNOSTIC.  This is a new,
 * optional IPC request rather than an extension of ipc_logger_status_t, so
 * deployed v2 clients can continue to use LOG_STATUS without an ABI change.
 * ``result`` is a FatFs FRESULT value for storage stages; ``spi_diagnostic``
 * is the CPU1 SD-SPI diagnostic code (zero means no low-level init error).
 */
typedef enum {
    IPC_LOG_DIAG_STAGE_NONE = 0,
    IPC_LOG_DIAG_STAGE_FATFS_MOUNT,
    IPC_LOG_DIAG_STAGE_SELFTEST_OPEN_WRITE,
    IPC_LOG_DIAG_STAGE_SELFTEST_WRITE,
    IPC_LOG_DIAG_STAGE_SELFTEST_SYNC,
    IPC_LOG_DIAG_STAGE_SELFTEST_CLOSE_WRITE,
    IPC_LOG_DIAG_STAGE_SELFTEST_OPEN_READ,
    IPC_LOG_DIAG_STAGE_SELFTEST_READ,
    IPC_LOG_DIAG_STAGE_SELFTEST_LENGTH,
    IPC_LOG_DIAG_STAGE_SELFTEST_COMPARE,
    IPC_LOG_DIAG_STAGE_SELFTEST_CLOSE_READ,
    IPC_LOG_DIAG_STAGE_LOG_NAME,
    IPC_LOG_DIAG_STAGE_LOG_OPEN,
    IPC_LOG_DIAG_STAGE_LOG_HEADER_WRITE,
    IPC_LOG_DIAG_STAGE_LOG_HEADER_SYNC,
    IPC_LOG_DIAG_STAGE_LOG_RECORD_WRITE,
    IPC_LOG_DIAG_STAGE_LOG_PERIODIC_SYNC,
    IPC_LOG_DIAG_STAGE_LOG_FLUSH_SYNC
} ipc_logger_diagnostic_stage_t;

/*
 * Low-level SD-SPI failure information packed into the existing 32-bit
 * logger-diagnostic detail field. This keeps the optional diagnostic ABI the
 * same size while exposing the values needed to distinguish a missing card
 * response from a Zynq PS SPI mode fault.
 *
 *   [31:28] reason
 *   [27:20] last byte received from the card
 *   [19:2]  SPI configuration register (implemented 18 bits)
 *   [1]     SPI enable-register bit
 *   [0]     mode-fault status bit
 */
typedef enum {
    IPC_SD_SPI_DETAIL_NONE = 0,
    IPC_SD_SPI_DETAIL_MODE_FAULT,
    IPC_SD_SPI_DETAIL_RX_TIMEOUT,
    IPC_SD_SPI_DETAIL_RESPONSE_TIMEOUT,
    IPC_SD_SPI_DETAIL_BAD_RESPONSE,
    IPC_SD_SPI_DETAIL_CONTROLLER_SETUP
} ipc_sd_spi_detail_reason_t;

#define IPC_SD_SPI_DETAIL_REASON_SHIFT 28U
#define IPC_SD_SPI_DETAIL_LAST_RX_SHIFT 20U
#define IPC_SD_SPI_DETAIL_CONFIG_SHIFT  2U
#define IPC_SD_SPI_DETAIL_REASON_MASK   0x0FU
#define IPC_SD_SPI_DETAIL_LAST_RX_MASK  0xFFU
#define IPC_SD_SPI_DETAIL_CONFIG_MASK   0x3FFFFU

#define IPC_SD_SPI_DETAIL_PACK(reason, last_rx, config, enabled, mode_fault) \
    ((((uint32_t)(reason) & IPC_SD_SPI_DETAIL_REASON_MASK) <<             \
      IPC_SD_SPI_DETAIL_REASON_SHIFT) |                                  \
     (((uint32_t)(last_rx) & IPC_SD_SPI_DETAIL_LAST_RX_MASK) <<           \
      IPC_SD_SPI_DETAIL_LAST_RX_SHIFT) |                                 \
     (((uint32_t)(config) & IPC_SD_SPI_DETAIL_CONFIG_MASK) <<             \
      IPC_SD_SPI_DETAIL_CONFIG_SHIFT) |                                  \
     (((uint32_t)(enabled) & 0x01U) << 1U) |                              \
     ((uint32_t)(mode_fault) & 0x01U))

#define IPC_SD_SPI_DETAIL_REASON(detail)                                  \
    (((uint32_t)(detail) >> IPC_SD_SPI_DETAIL_REASON_SHIFT) &             \
     IPC_SD_SPI_DETAIL_REASON_MASK)
#define IPC_SD_SPI_DETAIL_LAST_RX(detail)                                 \
    (((uint32_t)(detail) >> IPC_SD_SPI_DETAIL_LAST_RX_SHIFT) &            \
     IPC_SD_SPI_DETAIL_LAST_RX_MASK)
#define IPC_SD_SPI_DETAIL_CONFIG(detail)                                  \
    (((uint32_t)(detail) >> IPC_SD_SPI_DETAIL_CONFIG_SHIFT) &             \
     IPC_SD_SPI_DETAIL_CONFIG_MASK)
#define IPC_SD_SPI_DETAIL_ENABLED(detail)                                 \
    (((uint32_t)(detail) >> 1U) & 0x01U)
#define IPC_SD_SPI_DETAIL_MODE_FAULT(detail) ((uint32_t)(detail) & 0x01U)

typedef struct __attribute__((packed)) {
    /* Most recent failure in the active storage-failure streak. */
    uint32_t stage;
    int32_t result;
    uint32_t spi_diagnostic;
    uint32_t detail;
    /* First failure since the last fully successful storage_open(). */
    uint32_t first_stage;
    int32_t first_result;
    uint32_t first_spi_diagnostic;
    uint32_t first_detail;
} ipc_logger_diagnostic_t;

/*
 * ---------------------------------------------------------------------
 * Feature stream wire types
 * ---------------------------------------------------------------------
 * The two structs below are the WIRE copies of CPU1-internal types:
 *   ipc_feature_frame_v2_t <-> fpga_feature_frame_t (fpga_dsp_hal.h)
 *   ipc_anomaly_rule_v1_t  <-> zlog_anomaly_rule_v1_t (zlog_feature_v2.h)
 *
 * They are duplicated deliberately: common/ must stay free of firmware
 * headers, and Linux must decode the stream without one.
 * firmware/app_freertos/src/zlog_feature_v2.h carries per-field offsetof() and
 * sizeof() _Static_asserts tying each pair together, so adding or reordering a
 * field on either side fails the CPU1 build (REQ-OPS-002).
 *
 * These payloads are byte-for-byte identical to the FEATURE_V2 and
 * ANOMALY_RULE_V1 records storage_task writes to microSD, so comparing the two
 * at the same frame_sequence is a real tearing and loss check.
 */

typedef struct __attribute__((packed)) {
    uint32_t frame_sequence;
    uint32_t window_end_sample_sequence;
    uint32_t rms_q24_8;
    uint32_t peak_q24_8;
    uint32_t variance_q32_0;
    uint32_t kurtosis_q16_16;
    uint32_t dominant_frequency_q16_16;
    uint32_t dominant_power_q32_0;
    uint32_t band_energy_q32_0[4];
} ipc_feature_frame_v2_t;

typedef struct __attribute__((packed)) {
    uint8_t  rule_version;
    uint8_t  metric_id;
    uint8_t  verdict;        /* 1 = anomaly, 0 = normal */
    uint8_t  reserved0;
    uint32_t frame_sequence; /* == ipc_feature_frame_v2_t.frame_sequence */
    uint32_t value_q24_8;
    uint32_t threshold_q24_8;
} ipc_anomaly_rule_v1_t;

#define IPC_STREAM_ABI_VERSION 1U

/*
 * stream_sequence is ONE counter shared by both stream message types, so a gap
 * in it means a stream message was lost between CPU1's ring and this reader,
 * whichever kind it was. It counts only messages CPU1 actually published:
 * frames dropped because the ring was full, or because the TX mutex timed out,
 * never get a sequence number and are reported instead by dropped_since_last on
 * the NEXT message that does get through, so the loss is self-describing inside
 * the stream (REQ-STR-001).
 *
 * It does NOT reset when the Linux consumer restarts. Only a CPU1 reboot
 * restarts it, which the ABI v3 session_id already marks.
 *
 * timestamp_us is CPU1's platform_time_us() at publish time, the same clock the
 * microSD records use, so a capture can be matched against LOGnnnnn.BIN on both
 * timestamp and payload.
 */
typedef struct __attribute__((packed)) {
    uint32_t stream_sequence;
    uint32_t dropped_since_last;
    uint64_t timestamp_us;
    ipc_feature_frame_v2_t frame;
    ipc_anomaly_rule_v1_t  rule;
} ipc_feature_stream_v1_t;

/* Published only on an OK<->DEGRADED transition and on the degraded heartbeat
 * -- the same moments CPU1 writes a DSP_HEALTH ZLOG record -- so any
 * frame_sequence jump the receiver sees is explained in-band. */
typedef struct __attribute__((packed)) {
    uint32_t stream_sequence;
    uint32_t dropped_since_last;
    uint64_t timestamp_us;
    uint8_t  health_version;
    uint8_t  state;   /* 0 = OK, 1 = DEGRADED */
    uint8_t  fault;   /* zlog_dsp_fault_t encoding */
    uint8_t  reserved0;
    uint32_t consecutive_fault_count;
    uint32_t feature_count;
    uint32_t ctrl_drop_count;
    uint32_t bridge_drop_count;
} ipc_dsp_health_stream_v1_t;

/* Response payload for a zero-length MSG_TYPE_STREAM_STATUS request. */
typedef struct __attribute__((packed)) {
    uint32_t stream_abi_version;  /* IPC_STREAM_ABI_VERSION */
    uint32_t feature_count;       /* PL FEATURE_COUNT -- producer ground truth */
    uint32_t frames_published;    /* FEATURE_V2 messages that reached the ring */
    uint32_t frames_dropped;      /* ring full or TX mutex timeout */
    uint32_t health_published;
    uint32_t health_dropped;
    uint32_t last_frame_sequence;  /* last frame the publisher SAW, whether or
                                    * not it managed to send it */
    uint32_t last_stream_sequence; /* last sequence actually published */
    uint32_t ctrl_drop_count;
    uint32_t bridge_drop_count;
    uint32_t dsp_health_state;
    uint32_t doorbell_rings;      /* rings issued by the stream path only */
} ipc_stream_status_t;

_Static_assert(sizeof(ipc_message_t) == IPC_MESSAGE_SIZE,
               "ipc_message_t must be exactly 256 bytes");
_Static_assert(sizeof(ipc_mpu6050_sample_t) == 14U,
               "MPU6050 IPC sample ABI changed");
_Static_assert(sizeof(ipc_sensor_data_t) == 22U,
               "sensor response ABI changed");
_Static_assert(sizeof(ipc_logger_status_t) <= IPC_PAYLOAD_SIZE,
               "logger status does not fit in an IPC message");
_Static_assert(sizeof(ipc_logger_diagnostic_t) <= IPC_PAYLOAD_SIZE,
               "logger diagnostic does not fit in an IPC message");
_Static_assert(sizeof(ipc_feature_frame_v2_t) == 48U,
               "FEATURE_V2 wire frame must stay byte-identical to the "
               "microSD FEATURE_V2 payload -- update parse_zlog.py and "
               "firmware/app_freertos/src/zlog_feature_v2.h alongside it");
_Static_assert(sizeof(ipc_anomaly_rule_v1_t) == 16U,
               "ANOMALY_RULE_V1 wire payload size changed");
_Static_assert(sizeof(ipc_feature_stream_v1_t) == 80U,
               "feature stream ABI changed -- bump IPC_STREAM_ABI_VERSION");
_Static_assert(sizeof(ipc_dsp_health_stream_v1_t) == 36U,
               "DSP health stream ABI changed -- bump IPC_STREAM_ABI_VERSION");
_Static_assert(sizeof(ipc_feature_stream_v1_t) <= IPC_PAYLOAD_SIZE,
               "feature stream record does not fit in an IPC message");
_Static_assert(sizeof(ipc_dsp_health_stream_v1_t) <= IPC_PAYLOAD_SIZE,
               "DSP health record does not fit in an IPC message");
_Static_assert(sizeof(ipc_stream_status_t) <= IPC_PAYLOAD_SIZE,
               "stream status does not fit in an IPC message");

#endif
