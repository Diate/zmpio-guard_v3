#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * Compile-time configuration for the CPU1 (FreeRTOS) application: sensor,
 * microSD logger, PL DSP shell, IPC and task sizing. CPU1 is the sole owner of
 * every peripheral named here; CPU0 and Linux must not bind any of them.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "xspips.h"

#define APP_MPU6050_I2C_ADDRESS       0x68U
/* Sensor range/filter configuration: +/-8 g, +/-500 deg/s, DLPF 21 Hz. */
#define APP_MPU6050_ACCEL_LSB_PER_G   4096.0F
#define APP_MPU6050_GYRO_LSB_PER_DPS  65.5F
#define APP_SENSOR_RATE_HZ            100U
#define APP_SENSOR_RETRY_DELAY_MS     1000U
#define APP_SENSOR_MAX_ERRORS         5U

/*
 * Best-effort recovery from an MPU6050 bus lockup: retry the address byte this
 * many times while the AXI IIC core still reports Bus Busy, instead of giving
 * up the way XIic_WaitBusFree() does. See iic_bus_recovery.h.
 */
#define APP_MPU6050_BUS_RECOVERY_ATTEMPTS 5U

/*
 * Hard deadline for one complete AXI IIC transaction (iic_polled.c). A 15-byte
 * burst read on this 100 kHz bus takes ~1.4 ms including both address phases,
 * so anything slower is a fault rather than slow hardware. The transaction
 * must fail the sample instead of blocking, so that consecutive_errors can
 * reach APP_SENSOR_MAX_ERRORS and trigger recovery.
 */
#define APP_MPU6050_I2C_TIMEOUT_US    20000U

/*
 * Read the 14-byte MPU6050 output window in one repeated-start burst instead
 * of 14 single-register transactions, which removes 13 of the 14 START/STOP
 * pairs per sample. Set to 0U for the one-register-per-transaction fallback.
 */
#define APP_MPU6050_BURST_READ_ENABLED 1U

/*
 * GAP-002, disabled. The MPU6050 has no reset pin, so guaranteed recovery from
 * an I2C bus lockup requires cutting its VCC with a high-side P-MOSFET load
 * switch driven from PS EMIO GPIO bit 1. XGpioPs numbers EMIO pins from flat
 * index 54 (bank2), so EMIO bit 1 is index 55, not 1. Active-low, same
 * polarity as ETH_nRST: 0 = sensor powered, 1 = sensor unpowered. A pull-up on
 * the gate keeps the sensor off from board power-on until this firmware runs.
 *
 * Keep at 0U until the Vivado block design routes EMIO bit 1 to a package pin
 * and the P-MOSFET switch is wired; until then driving the pin is a no-op.
 */
#define APP_MPU6050_PWR_GPIO_ENABLED  0U
#define APP_MPU6050_PWR_EMIO_PIN      55U
#define APP_MPU6050_PWR_OFF_MS        200U
#define APP_MPU6050_PWR_STABLE_MS     100U

/*
 * CPU1 logs on PS UART1 (EMIO, K19 TX / M19 RX, JP2 header), a dedicated UART
 * with no other consumer -- unlike PS UART0, which CPU0 and Linux share as
 * their boot console. Safe to leave enabled; set to 0U only to strip the log
 * strings out of the build.
 */
#define APP_CPU1_UART_LOG_ENABLED      1U
#define APP_SENSOR_UART_LOG_INTERVAL_MS 1000U

#define APP_LOG_QUEUE_LENGTH          64U
#define APP_LOG_COMMAND_QUEUE_LENGTH  8U
#define APP_LOG_SYNC_INTERVAL_MS      1000U
#define APP_LOG_RETRY_DELAY_MS        1000U

#define APP_SD_SPI_SLAVE_SELECT       0U
#define APP_SD_SPI_INIT_PRESCALER     XSPIPS_CLK_PRESCALE_256
#define APP_SD_SPI_DATA_PRESCALER     XSPIPS_CLK_PRESCALE_32
#define APP_SD_COMMAND_TIMEOUT_MS     1000U
#define APP_SD_DATA_TIMEOUT_MS        1000U
/*
 * Busy-poll iterations sd_wait_ready() spins before it blocks on the FreeRTOS
 * tick instead. One poll is a single SPI byte (~1.6 us at the 5 MHz data
 * prescaler), so 64 covers ~100 us: ample for the "already ready" case and far
 * short of a card programming cycle. An unbounded spin here starves the sensor
 * task.
 */
#define APP_SD_WAIT_READY_FAST_POLLS  64U
/*
 * Runs once after every successful FatFs mount, before a LOGxxxxx.BIN file is
 * opened: writes a deterministic payload to a regular FAT file, syncs and
 * closes it, reopens it, then verifies byte count and CRC32. Keep enabled
 * while bringing up a new adapter or card; set to 0 for a deployed logger that
 * should not pay the extra write per mount.
 */
#define APP_SD_SELF_TEST_ENABLED       1U
#define APP_SD_SELF_TEST_BYTES         512U
#define APP_SD_SELF_TEST_FILENAME      "0:/SDTEST.BIN"

/*
 * Task stack sizes, in words. This firmware builds at -O0 (UserConfig.cmake),
 * so no stack slot is reused across a call chain; each size is set from the
 * deepest frame the task actually holds, with roughly 2x margin.
 *
 * Sensor: logger_submit() log_queue_item_t local (header plus a 240-byte
 * LOG_MAX_PAYLOAD_SIZE payload, log_record.h), fpga_dsp_hal_push_sample(), the
 * XIic driver, and on repeated read failures mpu6050_power_cycle() plus the
 * bit-banged recovery routine in iic_bus_recovery.c.
 */
#define APP_SENSOR_TASK_STACK_WORDS   3072U
/*
 * IPC RX: ipc_v3_poll() plus the per-command response builders in
 * dispatch_command(), each holding its own command, ack and counter structs
 * resident on top of the poll frame.
 */
#define APP_IPC_TASK_STACK_WORDS      3072U
/*
 * Storage: the resident storage_context_t in storage_task() (embeds a full
 * FATFS and FIL), plus the two live 512-byte arrays of the self-test path
 * (APP_SD_SELF_TEST_BYTES) and the f_open/f_write call depth inside FatFs.
 */
#define APP_STORAGE_TASK_STACK_WORDS  2048U
/*
 * FPGA result: drains the zmpio_dsp_ctrl feature FIFO on IRQ rather than
 * polling (see fpga_dsp_hal.h). Each iteration holds frame, counters, health
 * and rule_result resident -- plus a second health_record on the DSP_HEALTH
 * branch -- and makes three logger_submit() calls (FEATURE_V2,
 * ANOMALY_RULE_V1, DSP_HEALTH), each building its own log_queue_item_t.
 */
#define APP_FPGA_RESULT_TASK_STACK_WORDS 1536U

/*
 * Rule anomaly: a single-metric "RMS > mean + 2*std" test, the same formula
 * and factor as rule_baseline_report() in dsp_host_sim. The threshold is a
 * frozen constant rather than a running estimate, which keeps CPU1 float-free.
 *
 * Derived from one labeled board capture (undisturbed baseline, then moderate
 * and hard hand-shake, all on the same session and mount). Clean NORMAL
 * n=1292: mean=1221, std=565, max=8448; mean + 2*std = 2351. Measured on that
 * same dataset: moderate fault 64.2% detected, severe fault 92.2%, NORMAL
 * false-positive 1.8%. The NORMAL tail is heavier than Gaussian, which this
 * single-metric rule does not capture. ANOMALY_RULE_V1 is logged per frame
 * with no debounce.
 *
 * GAP-005: the threshold comes from one hand-shake session, not from a
 * multi-mode mechanical fault dataset (bearing wear, imbalance, looseness).
 * Re-derive once such a dataset exists, and bump APP_RULE_ANOMALY_VERSION
 * whenever the numeric threshold changes, so that older ZLOG captures stay
 * attributable to the rule version that produced them.
 */
#define APP_RULE_ANOMALY_VERSION           2U
#define APP_RULE_ANOMALY_RMS_THRESHOLD_Q24_8 2351U

/*
 * fpga_dsp_hal_update_health() flags DEGRADED after this many consecutive
 * polls (one per drained FIFO, i.e. per feature_ready IRQ) see a new fault --
 * FIFO-full stall, dropped sample, or result overflow -- and flags OK again
 * after this many consecutive fault-free polls. While DEGRADED,
 * fpga_result_task logs a DSP_HEALTH heartbeat at this interval instead of on
 * every poll, so a stuck-degraded PL cannot flood the SD write path.
 */
#define APP_DSP_HEALTH_FAULT_STREAK_TO_DEGRADED  3U
#define APP_DSP_HEALTH_OK_STREAK_TO_RECOVER      10U
#define APP_DSP_HEALTH_DEGRADED_LOG_INTERVAL_MS  5000U

/*
 * Fault injection: ABI v3 FIFO_FULL_INJECT asks fpga_result_task to pause
 * draining the zmpio_dsp_ctrl feature FIFO so that the production of the DSP
 * core fills it and DROP_COUNT increments. Software-only -- no PL register is
 * touched (fpga_dsp_hal.c). MAX_HOLD_MS bounds a misbehaving or oversized CPU0
 * request; POLL_MS is how often fpga_result_task wakes to check whether the
 * hold has expired.
 */
#define APP_FIFO_FULL_INJECT_MAX_HOLD_MS  120000U
#define APP_FIFO_FULL_INJECT_POLL_MS      50U

/*
 * Arming the zmpio_dsp_ctrl IRQ (GIC SPI 62) under this AMP split. CPU1 builds
 * with USE_AMP=1, so its XScuGic_CfgInitialize() does not initialise the GIC
 * distributor -- by design, the distributor belongs to CPU0. Consequently CPU1
 * may set the SPI 62 enable bit only AFTER CPU0 has run DoDistributorInit(),
 * whose final step writes ICDDCR=1: that same function writes
 * ICDICER=0xFFFFFFFF for every SPI bank and resets priorities to 0xA0, erasing
 * whatever CPU1 programmed earlier.
 *
 * GIC_WAIT_MS: how long fpga_result_task busy-waits for ICDDCR before its
 * first arm. vTaskDelay() is unusable here, because without a distributor the
 * tick IRQ does not arrive either. CPU0 raises ICDDCR roughly 6 s after CPU1
 * boots, so this is about 2x margin while staying well inside the ~16 s wrap
 * of the 32-bit ARM global timer at CPU_3x2x = 266.64 MHz
 * (platform_time_counter() is that counter, and a deadline is only correct
 * while the budget is shorter than one wrap).
 *
 * WATCHDOG_POLL_MS: how often fpga_result_task re-checks the enable bit and
 * re-arms if the distributor is re-initialised mid-run. This is the half that
 * covers CPU0 restart and Linux boot; no waiting budget can cover those.
 */
#define APP_DSP_IRQ_GIC_WAIT_MS       12000U
#define APP_DSP_IRQ_WATCHDOG_POLL_MS  1000U

/*
 * Feature stream: fpga_result_task publishes each feature frame to Linux over
 * the ABI v2 TX ring right after it has queued the microSD records.
 *
 * SEND_TIMEOUT_MS bounds how long feature_stream.c may wait for tx_mutex.
 * ipc_rx_task holds that mutex only for the memcpy of one 256-byte slot, so
 * 5 ms is orders of magnitude more than the contended case needs while still
 * being half of one 10 ms sensor tick -- it can never delay the FIFO drain
 * enough to matter against the ~640 ms feature period. A timeout is treated
 * exactly like a full ring: drop the frame, count it, keep draining. This path
 * must never wait indefinitely.
 *
 * DROP_LOG_INTERVAL throttles the UART1 line during a sustained drop streak: a
 * consumer that stops entirely holds the 256-slot ring full for ~164 s, and
 * one line per lost frame would bury every other log. The first drop of a
 * streak is always logged.
 */
#define APP_FEATURE_STREAM_SEND_TIMEOUT_MS  5U
#define APP_FEATURE_STREAM_DROP_LOG_INTERVAL 32U

/*
 * ABI v3 SET_DSP_CONFIG validation (ipc_v3.c). No runtime PL config-write path
 * exists yet -- CONFIG_SEQ is a scratch register, not a real coefficient load
 * -- so these bounds only decide ACK versus NACK; a validated command still
 * just bumps CONFIG_SEQ via fpga_dsp_hal_bump_config_seq() rather than
 * changing FIR/FFT behaviour. coeff_set_id has exactly one valid value because
 * only coefficients_v1.json is loaded into the PL.
 */
#define APP_ZMPIO_V3_COEFF_SET_ID_VALID   0U
#define APP_ZMPIO_V3_FFT_SCALE_SHIFT_MAX  7U
#define APP_ZMPIO_V3_FEATURE_MASK_VALID   0x3FFU /* bits 0..9, one per FeatureFrameV2 field */

#define APP_IPC_TASK_PRIORITY         (tskIDLE_PRIORITY + 3U)
#define APP_SENSOR_TASK_PRIORITY      (tskIDLE_PRIORITY + 2U)
/*
 * Above the sensor/storage pair, which time-slice at the same priority, but
 * below IPC_RX: feature frames arrive at most once every 640 ms (window 128,
 * hop 64 at 100 Hz), far slower than the 10 ms sensor tick, so there is no
 * throughput reason to run it higher, and staying below IPC_RX keeps Linux
 * command turnaround unaffected by FIFO drains.
 */
#define APP_FPGA_RESULT_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
/*
 * One sensor sample can consume an entire 10 ms tick, in which case a
 * lower-priority writer would never run. Keep writer and sensor at the same
 * priority so FreeRTOS time slicing drains the queue, with IPC RX above both.
 */
#define APP_STORAGE_TASK_PRIORITY     (tskIDLE_PRIORITY + 2U)

#endif
