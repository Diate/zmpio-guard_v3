#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "app_config.h"
#include "cpu1_log.h"
#include "diskio_sd.h"
#include "feature_stream.h"
#include "fpga_dsp_hal.h"
#include "iic_polled.h"
#include "ipc_shared_mem.h"
#include "logger.h"
#include "mpu6050.h"
#include "mpu6050_power.h"
#include "platform_time.h"
#include "rule_anomaly.h"
#include "sd_spi.h"
#include "semphr.h"
#include "storage_task.h"
#include "task.h"
#include "uart1_log.h"
#include "zlog_feature_v2.h"
#include "xiic.h"
#include "xparameters.h"
#include "xspips.h"
#include "xstatus.h"
#include "ipc_v3.h"
#include "zmpio_ipc_logic.h"

static XIic iic_instance;
static XSpiPs spi_instance;
static mpu6050_t mpu6050;
static sd_spi_t sd_card;
static mpu6050_sample_t latest_sensor_sample;
static uint64_t latest_sensor_timestamp_us;
static bool latest_sensor_available;
static SemaphoreHandle_t fpga_result_semaphore;
static uint32_t fpga_sample_sequence;

static void update_sensor_snapshot(const mpu6050_sample_t *sample,
                                   uint64_t timestamp_us)
{
    taskENTER_CRITICAL();
    latest_sensor_sample = *sample;
    latest_sensor_timestamp_us = timestamp_us;
    latest_sensor_available = true;
    taskEXIT_CRITICAL();
}

static bool read_sensor_snapshot(ipc_sensor_data_t *snapshot)
{
    bool available;

    taskENTER_CRITICAL();
    available = latest_sensor_available;
    if (available) {
        snapshot->sample = latest_sensor_sample;
        snapshot->sample_timestamp_us = latest_sensor_timestamp_us;
    }
    taskEXIT_CRITICAL();
    return available;
}

static int initialize_peripherals(void)
{
    XSpiPs_Config *spi_config;
    int status;

    /* Force the sensor through a real power-on reset before the first I2C
     * transaction. A JTAG reload/reboot only resets the AXI IIC controller
     * in the PL, not the MPU6050 chip itself, so a bus lockup left over from
     * the previous run would otherwise survive the reload. */
    mpu6050_power_init();

    status = XIic_Initialize(&iic_instance, XPAR_AXI_IIC_0_BASEADDR);
    if (status != XST_SUCCESS) {
        CPU1_LOG("CPU1: AXI IIC initialization failed\r\n");
        return XST_FAILURE;
    }
    mpu6050_bind(&mpu6050, &iic_instance, APP_MPU6050_I2C_ADDRESS);

    spi_config = XSpiPs_LookupConfig(XPAR_XSPIPS_0_BASEADDR);
    if (spi_config == NULL) {
        CPU1_LOG("CPU1: PS SPI0 configuration not found\r\n");
        return XST_FAILURE;
    }
    status = XSpiPs_CfgInitialize(&spi_instance, spi_config,
                                  spi_config->BaseAddress);
    if (status != XST_SUCCESS) {
        CPU1_LOG("CPU1: PS SPI0 initialization failed\r\n");
        return XST_FAILURE;
    }
    status = XSpiPs_SetOptions(&spi_instance,
                               XSPIPS_MASTER_OPTION |
                               XSPIPS_FORCE_SSELECT_OPTION);
    if (status != XST_SUCCESS) {
        CPU1_LOG("CPU1: PS SPI0 option setup failed\r\n");
        return XST_FAILURE;
    }
    if (XSpiPs_SetSlaveSelect(&spi_instance,
                              APP_SD_SPI_SLAVE_SELECT) != XST_SUCCESS) {
        CPU1_LOG("CPU1: PS SPI0 chip-select setup failed\r\n");
        return XST_FAILURE;
    }

    sd_spi_bind(&sd_card, &spi_instance);
    sd_disk_bind(&sd_card);
    return XST_SUCCESS;
}

static void sensor_task(void *parameters)
{
    TickType_t last_wake = xTaskGetTickCount();
#if APP_CPU1_UART_LOG_ENABLED
    TickType_t last_uart_log = last_wake;
#endif
    TickType_t sample_period = configTICK_RATE_HZ / APP_SENSOR_RATE_HZ;
    uint32_t consecutive_errors = 0U;

    (void)parameters;
    if (sample_period == 0U) {
        sample_period = 1U;
    }

    for (;;) {
        mpu6050_sample_t sample;

        if (mpu6050.initialized == 0U) {
            if (mpu6050_configure(&mpu6050) != XST_SUCCESS) {
                CPU1_LOG("CPU1: MPU6050 init failed stage=%d who=0x%02x "
                           "pwr1=0x%02x pwr2=0x%02x cfg=0x%02x div=%d "
                           "gyro=0x%02x accel=0x%02x\r\n",
                           (int)mpu6050.last_diagnostic, mpu6050.who_am_i,
                           mpu6050.pwr_mgmt_1, mpu6050.pwr_mgmt_2,
                           mpu6050.config,
                           (int)mpu6050.sample_rate_divider,
                           mpu6050.gyro_config, mpu6050.accel_config);
                vTaskDelay(pdMS_TO_TICKS(APP_SENSOR_RETRY_DELAY_MS));
                last_wake = xTaskGetTickCount();
                continue;
            }
            CPU1_LOG("CPU1: MPU6050 online at 0x%02x\r\n",
                       APP_MPU6050_I2C_ADDRESS);
            CPU1_LOG("CPU1: MPU cfg who=0x%02x pwr1=0x%02x pwr2=0x%02x "
                       "cfg=0x%02x div=%d gyro=0x%02x accel=0x%02x\r\n",
                       mpu6050.who_am_i, mpu6050.pwr_mgmt_1,
                       mpu6050.pwr_mgmt_2, mpu6050.config,
                       (int)mpu6050.sample_rate_divider,
                       mpu6050.gyro_config, mpu6050.accel_config);
            consecutive_errors = 0U;
            last_wake = xTaskGetTickCount();
        }

        if (mpu6050_read_sample(&mpu6050, &sample) == XST_SUCCESS) {
            uint64_t timestamp_us = platform_time_us();

            update_sensor_snapshot(&sample, timestamp_us);
            (void)logger_submit(LOG_SOURCE_MPU6050, &sample, sizeof(sample),
                                0U, timestamp_us, 0U);
            /* Push the same raw sample into the PL DSP core alongside the
             * unchanged raw ZLOG write above. flags is reserved. */
            fpga_dsp_hal_push_sample(sample.accel_x, sample.accel_y,
                                     sample.accel_z, sample.temperature,
                                     sample.gyro_x, sample.gyro_y,
                                     sample.gyro_z, 0U,
                                     fpga_sample_sequence++);
#if APP_CPU1_UART_LOG_ENABLED
            if ((TickType_t)(xTaskGetTickCount() - last_uart_log) >=
                pdMS_TO_TICKS(APP_SENSOR_UART_LOG_INTERVAL_MS)) {
                CPU1_LOG("CPU1: MPU raw ax=%d ay=%d az=%d temp=%d "
                           "gx=%d gy=%d gz=%d\r\n",
                           (int)sample.accel_x, (int)sample.accel_y,
                           (int)sample.accel_z, (int)sample.temperature,
                           (int)sample.gyro_x, (int)sample.gyro_y,
                           (int)sample.gyro_z);
                last_uart_log = xTaskGetTickCount();
            }
#endif
            consecutive_errors = 0U;
        } else {
            ++consecutive_errors;
            if (consecutive_errors >= APP_SENSOR_MAX_ERRORS) {
                mpu6050.initialized = 0U;
                /* iic_polled distinguishes "slave did not ACK" from
                 * "bus wedged" from "transfer timed out" -- the
                 * difference between a sensor fault and a bus fault. */
                CPU1_LOG("CPU1: MPU6050 read failed (i2c=%s), "
                           "power-cycling and reinitializing\r\n",
                           iic_polled_status_string(
                               iic_polled_last_status()));
                /* Same recovery a physical unplug/replug currently provides
                 * by hand: repeated read failures usually mean the I2C bus
                 * is wedged, and XIic_Reset() alone only resets the master
                 * side, not a slave stuck holding SDA low. */
                mpu6050_power_cycle();
            }
        }

        vTaskDelayUntil(&last_wake, sample_period);
    }
}

/* Blocks on the zmpio_dsp_ctrl IRQ semaphore instead of polling STATUS, then
 * drains the feature FIFO to empty. Every popped frame gets a FEATURE_V2 ZLOG
 * record plus one paired ANOMALY_RULE_V1 record: rule_anomaly_evaluate() runs
 * here, in task context, never in fpga_dsp_hal_isr() (see rule_anomaly.h).
 * DSP health (fpga_dsp_hal_update_health()) is polled once per drain and
 * logged as DSP_HEALTH on every OK/DEGRADED transition, plus a heartbeat every
 * APP_DSP_HEALTH_DEGRADED_LOG_INTERVAL_MS while degraded, so a stuck-degraded
 * PL cannot flood the SD write path.
 *
 * The same frames have a second consumer: feature_stream_publish_feature()
 * puts each frame and its rule verdict on the ABI v2 TX ring for Linux and
 * rings the PL doorbell, so Linux gets the data live instead of only from the
 * microSD card CPU1 owns. It never blocks this task -- see the loss policy in
 * feature_stream.h. ANOMALY_ML_V1 has no producer anywhere in V3 (ADR-017
 * takes ML out of scope): the record type stays RESERVED in log_record.h, and
 * the anomaly verdict Linux receives is the rule one. */
static void fpga_result_task(void *parameters)
{
    TickType_t last_health_log = xTaskGetTickCount();
    bool health_logged_once = false;

    (void)parameters;

    /* Must happen after vTaskStartScheduler(), not from fpga_dsp_hal_init()
     * in main() -- see fpga_dsp_hal_irq_start()'s doc comment. This task's
     * first line is the earliest point guaranteed to run after the
     * scheduler (and its tick-timer GIC setup) is fully up. */
    fpga_dsp_hal_irq_start();

    for (;;) {
        fpga_feature_frame_t frame;
        fpga_dsp_hal_counters_t counters;
        fpga_dsp_hal_health_t health;
        bool fault_injection_recovered = false;

        /* Bounded wait, not portMAX_DELAY: CPU0 owns the GIC distributor in
         * this AMP split, and any distributor re-init on its side silently
         * clears the enable bit of this IRQ (see the doc comment on
         * fpga_dsp_hal_irq_start()). A timeout here is the only symptom that
         * failure has: the FIFO fills, the PL keeps producing, and nothing
         * ever wakes this task again. So on every timeout, re-arm if the
         * enable bit is gone, and drain anyway if the FIFO has work -- a
         * polled fallback that does not depend on a healthy GIC. */
        if (xSemaphoreTake(fpga_result_semaphore,
                           pdMS_TO_TICKS(APP_DSP_IRQ_WATCHDOG_POLL_MS))
                != pdTRUE) {
            bool irq_rearmed = fpga_dsp_hal_irq_watchdog();

            if (irq_rearmed) {
                CPU1_LOG("CPU1: DSP IRQ re-armed in GIC after it was cleared\r\n");
            }
            if (!irq_rearmed && !fpga_dsp_hal_feature_ready() &&
                !fpga_dsp_hal_fault_inject_stall_active()) {
                /* Genuinely idle: nothing to drain, IRQ still armed. */
                continue;
            }
        }

        /* FIFO-full fault injection (ABI v3 FIFO_FULL_INJECT, ipc_v3.c):
         * deliberately skip draining while armed, so that the feature
         * production of the DSP core fills the 64-entry zmpio_dsp_ctrl FIFO
         * and its DROP_COUNT increments. The IRQ stays masked throughout,
         * which is fine: what fills the FIFO is PL hardware production, not
         * IRQ handling. Once the hold expires, fall straight into the normal
         * drain loop below, so the backlog is seen to recover cleanly. */
        if (fpga_dsp_hal_fault_inject_stall_active()) {
            CPU1_LOG("CPU1: ABI v3 FIFO_FULL_INJECT holding drain hold_ms=%lu\r\n",
                       (unsigned long)fpga_dsp_hal_fault_inject_stall_hold_ms_applied());
            while (fpga_dsp_hal_fault_inject_stall_active()) {
                vTaskDelay(pdMS_TO_TICKS(APP_FIFO_FULL_INJECT_POLL_MS));
            }
            CPU1_LOG("CPU1: ABI v3 FIFO_FULL_INJECT hold expired, draining backlog\r\n");
            fault_injection_recovered = true;
        }

        while (fpga_dsp_hal_pop_feature(&frame)) {
            uint64_t timestamp_us = platform_time_us();
            zlog_anomaly_rule_v1_t rule_result;

            CPU1_LOG("CPU1: DSP feature #%lu rms_q24_8=%lu dom_freq_q16_16=%lu\r\n",
                       (unsigned long)frame.frame_sequence,
                       (unsigned long)frame.rms_q24_8,
                       (unsigned long)frame.dominant_frequency_q16_16);
            (void)logger_submit(LOG_SOURCE_FEATURE_V2, &frame, sizeof(frame),
                                0U, timestamp_us, 0U);

            rule_anomaly_evaluate(&frame, &rule_result);
            (void)logger_submit(LOG_SOURCE_ANOMALY_RULE_V1, &rule_result,
                                sizeof(rule_result), rule_result.verdict,
                                timestamp_us, 0U);
            /* microSD first, Linux second. The order matters: the SD record
             * is the archival copy, so it must not depend on the stream
             * succeeding. */
            feature_stream_publish_feature(&frame, &rule_result, timestamp_us);
            if (rule_result.verdict != 0U) {
                CPU1_LOG("CPU1: rule anomaly frame=%lu rms_q24_8=%lu "
                           "threshold_q24_8=%lu\r\n",
                           (unsigned long)rule_result.frame_sequence,
                           (unsigned long)rule_result.value_q24_8,
                           (unsigned long)rule_result.threshold_q24_8);
            }
        }

        /* FIFO is empty now -- safe to unmask. See fpga_dsp_hal_isr()'s
         * matching fpga_dsp_hal_irq_disable() call. */
        fpga_dsp_hal_irq_enable();

        fpga_dsp_hal_get_counters(&counters);
        if (counters.result_overflow || (counters.bridge_drop_count != 0U)) {
            CPU1_LOG("CPU1: DSP counters feature=%lu ctrl_drop=%lu "
                       "bridge_drop=%lu overflow=%d\r\n",
                       (unsigned long)counters.feature_count,
                       (unsigned long)counters.ctrl_drop_count,
                       (unsigned long)counters.bridge_drop_count,
                       (int)counters.result_overflow);
        }
        if (fault_injection_recovered) {
            /* Evidence for the FIFO-full gate: compare against the
             * feature_count_before/drop_count_before fields of the
             * FIFO_FULL_INJECT_ACK to confirm that ctrl_drop really increased
             * while I2C, SD and the scheduler kept running. */
            CPU1_LOG("CPU1: ABI v3 FIFO_FULL_INJECT counters after drain "
                       "feature_count=%lu ctrl_drop_count=%lu\r\n",
                       (unsigned long)counters.feature_count,
                       (unsigned long)counters.ctrl_drop_count);
        }

        fpga_dsp_hal_update_health(&counters, &health);
        /* Cached here, in the one task allowed to read the PL counters, so
         * ipc_rx_task can answer MSG_TYPE_STREAM_STATUS without a second
         * reader clearing the sticky overflow bit (feature_stream.h). */
        feature_stream_note_counters(&counters, health.state);
        {
            bool heartbeat_due = (health.state == 1U) &&
                ((TickType_t)(xTaskGetTickCount() - last_health_log) >=
                 pdMS_TO_TICKS(APP_DSP_HEALTH_DEGRADED_LOG_INTERVAL_MS));

            if (health.state_changed || heartbeat_due || !health_logged_once) {
                zlog_dsp_health_t health_record;
                uint64_t health_timestamp_us = platform_time_us();

                health_record.health_version = (uint8_t)ZLOG_DSP_HEALTH_VERSION;
                health_record.state = health.state;
                health_record.fault = health.last_fault;
                health_record.reserved0 = 0U;
                health_record.consecutive_fault_count =
                    health.consecutive_fault_count;
                health_record.feature_count = counters.feature_count;
                health_record.ctrl_drop_count = counters.ctrl_drop_count;
                health_record.bridge_drop_count = counters.bridge_drop_count;

                (void)logger_submit(LOG_SOURCE_DSP_HEALTH, &health_record,
                                    sizeof(health_record), health.state,
                                    health_timestamp_us, 0U);
                /* Same moment, same bytes, to Linux: this is what explains
                 * a frame_sequence jump in-band. */
                feature_stream_publish_health(&health_record,
                                              health_timestamp_us);
                if (health.state_changed) {
                    CPU1_LOG("CPU1: DSP health -> %s fault=%d\r\n",
                               (health.state == 1U) ? "DEGRADED" : "OK",
                               (int)health.last_fault);
                }
                last_health_log = xTaskGetTickCount();
                health_logged_once = true;
            }
        }
    }
}

static void send_acknowledgement(const ipc_message_t *request, int32_t status,
                                 uint32_t detail)
{
    ipc_message_t reply;
    ipc_ack_payload_t acknowledgement;

    memset(&reply, 0, sizeof(reply));
    acknowledgement.request_type = request->header.type;
    acknowledgement.status = status;
    acknowledgement.detail = detail;
    reply.header.magic = IPC_MAGIC;
    reply.header.type = status == 0 ? MSG_TYPE_ACK : MSG_TYPE_ERROR;
    reply.header.timestamp = request->header.timestamp;
    reply.header.length = sizeof(acknowledgement);
    memcpy(reply.payload, &acknowledgement, sizeof(acknowledgement));
    (void)ipc_send(&reply);
}

static void send_logger_status(const ipc_message_t *request)
{
    ipc_message_t reply;
    logger_status_t status;

    logger_get_status(&status);
    memset(&reply, 0, sizeof(reply));
    reply.header.magic = IPC_MAGIC;
    reply.header.type = MSG_TYPE_LOG_STATUS;
    reply.header.timestamp = request->header.timestamp;
    reply.header.length = sizeof(status);
    memcpy(reply.payload, &status, sizeof(status));
    (void)ipc_send(&reply);
}

static void send_logger_diagnostic(const ipc_message_t *request)
{
    ipc_message_t reply;
    logger_diagnostic_t diagnostic;

    logger_get_diagnostic(&diagnostic);
    memset(&reply, 0, sizeof(reply));
    reply.header.magic = IPC_MAGIC;
    reply.header.type = MSG_TYPE_LOG_DIAGNOSTIC;
    reply.header.timestamp = request->header.timestamp;
    reply.header.length = sizeof(diagnostic);
    memcpy(reply.payload, &diagnostic, sizeof(diagnostic));
    (void)ipc_send(&reply);
}

/*
 * The CPU1 view of the feature stream, so the Linux side can check "records
 * received + dropped_since_last == feature_count" against the producer rather
 * than against itself. Kept as its own message type instead of extra fields on
 * LOG_STATUS, because that struct is deployed ABI v2 and an older v2 client
 * must keep working byte for byte.
 */
static void send_stream_status(const ipc_message_t *request)
{
    ipc_message_t reply;
    ipc_stream_status_t status;

    feature_stream_get_status(&status);
    memset(&reply, 0, sizeof(reply));
    reply.header.magic = IPC_MAGIC;
    reply.header.type = MSG_TYPE_STREAM_STATUS;
    reply.header.timestamp = request->header.timestamp;
    reply.header.length = sizeof(status);
    memcpy(reply.payload, &status, sizeof(status));
    (void)ipc_send(&reply);
}

/* Linux sends MSG_TYPE_SENSOR_DATA with an empty payload to poll the newest
 * sample.  This is request/response rather than a 100 Hz push stream, so a
 * stopped Linux process cannot fill the CPU1-to-Linux IPC ring. */
static void send_sensor_sample(const ipc_message_t *request)
{
    ipc_message_t reply;
    ipc_sensor_data_t snapshot;

    if (!read_sensor_snapshot(&snapshot)) {
        send_acknowledgement(request, -4, 0U);
        return;
    }

    memset(&reply, 0, sizeof(reply));
    reply.header.magic = IPC_MAGIC;
    reply.header.type = MSG_TYPE_SENSOR_DATA;
    reply.header.timestamp = request->header.timestamp;
    reply.header.length = sizeof(snapshot);
    memcpy(reply.payload, &snapshot, sizeof(snapshot));
    (void)ipc_send(&reply);
}

static void ipc_rx_task(void *parameters)
{
    (void)parameters;

    /* Run after the scheduler/port has programmed the CPU1 private timer and
     * before the lower-priority sensor and storage tasks can consume ticks. */
    platform_freertos_tick_fix();

    for (;;) {
        ipc_message_t message;

        /* Bounded ABI v3 poll alongside the existing ABI v2 receive: drains
         * at most one v3 command per loop iteration, so neither channel can
         * starve the other. */
        (void)ipc_v3_poll();

        if (ipc_recv_from_linux(&message) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10U));
            continue;
        }

        if ((message.header.magic != IPC_MAGIC) ||
            (message.header.length > IPC_PAYLOAD_SIZE)) {
            send_acknowledgement(&message, -1, 0U);
            continue;
        }

        switch ((msg_type_t)message.header.type) {
        case MSG_TYPE_RAW_DATA: /* Backward-compatible Linux data message. */
        case MSG_TYPE_LOG_DATA:
            if (logger_submit(LOG_SOURCE_LINUX, message.payload,
                              (uint16_t)message.header.length,
                              (uint16_t)message.header.type,
                              platform_time_us(), 0U)) {
                logger_status_t status;
                logger_get_status(&status);
                send_acknowledgement(&message, 0,
                                     status.queue_depth);
            } else {
                send_acknowledgement(&message, -2, 0U);
            }
            break;
        case MSG_TYPE_LOG_START:
            send_acknowledgement(&message,
                                 logger_send_command(LOGGER_COMMAND_START,
                                                     0U) ? 0 : -2,
                                 0U);
            break;
        case MSG_TYPE_LOG_STOP:
            send_acknowledgement(&message,
                                 logger_send_command(LOGGER_COMMAND_STOP,
                                                     0U) ? 0 : -2,
                                 0U);
            break;
        case MSG_TYPE_LOG_FLUSH:
            send_acknowledgement(&message,
                                 logger_send_command(LOGGER_COMMAND_FLUSH,
                                                     0U) ? 0 : -2,
                                 0U);
            break;
        case MSG_TYPE_LOG_STATUS:
            send_logger_status(&message);
            break;
        case MSG_TYPE_LOG_DIAGNOSTIC:
            if (message.header.length != 0U) {
                send_acknowledgement(&message, -1, message.header.length);
            } else {
                send_logger_diagnostic(&message);
            }
            break;
        case MSG_TYPE_SENSOR_DATA:
            if (message.header.length != 0U) {
                send_acknowledgement(&message, -1, message.header.length);
            } else {
                send_sensor_sample(&message);
            }
            break;
        case MSG_TYPE_STREAM_STATUS:
            if (message.header.length != 0U) {
                send_acknowledgement(&message, -1, message.header.length);
            } else {
                send_stream_status(&message);
            }
            break;
        case MSG_TYPE_HEARTBEAT:
            send_acknowledgement(&message, 0, IPC_PROTOCOL_VERSION);
            break;
        default:
            send_acknowledgement(&message, -3, message.header.type);
            break;
        }
    }
}

int main(void)
{
    BaseType_t result;

#if APP_CPU1_UART_LOG_ENABLED
    (void)uart1_log_init();
#endif
    CPU1_LOG("CPU1: starting MPU6050/IPC SD logger\r\n");
    platform_time_init();
    /* Must precede every shared-DDR access below (ipc_init/ipc_rx_init/
     * ipc_v3_init and the rings they own): this is what makes the ABI v2
     * and ABI v3 windows non-cacheable on this core, matching CPU0. See
     * ipc_shared_mem.c for why manual cache maintenance cannot work. */
    ipc_shared_mem_init();
    if (initialize_peripherals() != XST_SUCCESS) {
        return -1;
    }
    if (logger_init() != 0) {
        CPU1_LOG("CPU1: logger queue allocation failed\r\n");
        return -1;
    }

    if (ipc_init(IPC_BUFFER_SIZE) != 0) {
        return -1;
    }
    ipc_rx_init(IPC_BUFFER_SIZE);
    /* Zeroes the stream counters before any task can publish. Must follow
     * ipc_init() -- the TX ring this publishes on -- and precede
     * vTaskStartScheduler(). */
    feature_stream_init();

    /* ABI v3: a region disjoint from the ABI v2 rings above, additive and
     * lab-only for now (see ipc_v3.h). Initialised here, at the same point in
     * boot as ipc_init()/ipc_rx_init(), so CPU0 can start polling for
     * ZMPIO_V3_LINK_CPU1_READY as soon as it releases CPU1 from WFE. */
    if (ipc_v3_init() != 0) {
        CPU1_LOG("CPU1: ABI v3 init failed\r\n");
        return -1;
    }

    /* PL DSP shell. The semaphore must exist before fpga_dsp_hal_init()
     * brings the shell up. Init itself only programs zmpio_dsp_ctrl and does
     * not depend on the scheduler being started yet, matching the rest of
     * initialize_peripherals() above. */
    fpga_result_semaphore = xSemaphoreCreateBinary();
    if (fpga_result_semaphore == NULL) {
        CPU1_LOG("CPU1: FPGA DSP result semaphore allocation failed\r\n");
        return -1;
    }
    fpga_dsp_hal_set_result_semaphore(fpga_result_semaphore);
    if (fpga_dsp_hal_init() != 0) {
        CPU1_LOG("CPU1: FPGA DSP HAL init failed\r\n");
        return -1;
    }

    result = xTaskCreate(storage_task, "SD_Write",
                         APP_STORAGE_TASK_STACK_WORDS, NULL,
                         APP_STORAGE_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        CPU1_LOG("CPU1: storage task creation failed\r\n");
        return -1;
    }
    result = xTaskCreate(ipc_rx_task, "IPC_RX", APP_IPC_TASK_STACK_WORDS,
                         NULL, APP_IPC_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        CPU1_LOG("CPU1: IPC task creation failed\r\n");
        return -1;
    }
    result = xTaskCreate(sensor_task, "Sensor",
                         APP_SENSOR_TASK_STACK_WORDS, NULL,
                         APP_SENSOR_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        CPU1_LOG("CPU1: sensor task creation failed\r\n");
        return -1;
    }
    result = xTaskCreate(fpga_result_task, "FPGA_Result",
                         APP_FPGA_RESULT_TASK_STACK_WORDS, NULL,
                         APP_FPGA_RESULT_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        CPU1_LOG("CPU1: FPGA result task creation failed\r\n");
        return -1;
    }

    vTaskStartScheduler();
    CPU1_LOG("CPU1: scheduler stopped unexpectedly\r\n");
    return -1;
}
