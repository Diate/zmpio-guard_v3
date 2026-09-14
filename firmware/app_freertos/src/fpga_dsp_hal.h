#ifndef FPGA_DSP_HAL_H
#define FPGA_DSP_HAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * CPU1-side driver for the PL DSP shell: mmio_axis_bridge.v (sample push) and
 * zmpio_dsp_ctrl.v (control/status plus a 64-entry feature FIFO), wired around
 * DspCore through hardware/rtl/dsp_core/hls/dsp_core_axis_top.
 *
 * Base addresses come from the mmio_axis_bridge_0 / zmpio_dsp_ctrl_0 reg
 * properties in firmware/platform_dual/hw/sdt/pl.dtsi. The IRQ ID has no
 * interrupts property there, because the SDT generator only infers one for IP
 * declaring a formal IP-XACT INTERRUPT bus interface, which these hand-written
 * modules do not: it follows from the xlconcat_0 port position instead.
 * axi_iic_0 on In0 is interrupts = <0 29 4>, i.e. GIC ID 61 (SPI 29 + 32,
 * UG585 Table B-1), so irq_out of zmpio_dsp_ctrl on In1 is GIC ID 62.
 */
#define FPGA_DSP_HAL_MMIO_BRIDGE_BASEADDR      0x40000000U
#define FPGA_DSP_HAL_DSP_CTRL_BASEADDR         0x40001000U
#define FPGA_DSP_HAL_IRQ_ID                    62U

/* Mirrors zmpio::step1::FeatureFrameV2 (dsp_host_sim/fpga/dsp_core/src/
 * feature_frame_v2.h) field for field; kept as a separate plain-C struct here
 * because app_freertos is compiled as C, not C++. */
typedef struct {
    uint32_t frame_sequence;
    uint32_t window_end_sample_sequence;
    uint32_t rms_q24_8;
    uint32_t peak_q24_8;
    uint32_t variance_q32_0;
    uint32_t kurtosis_q16_16;
    uint32_t dominant_frequency_q16_16;
    uint32_t dominant_power_q32_0;
    uint32_t band_energy_q32_0[4];
} fpga_feature_frame_t;

typedef struct {
    uint32_t feature_count;      /* zmpio_dsp_ctrl FEATURE_COUNT */
    uint32_t ctrl_drop_count;    /* zmpio_dsp_ctrl DROP_COUNT (FIFO-full stalls) */
    uint32_t bridge_drop_count;  /* mmio_axis_bridge DROP_COUNT (samples actually lost) */
    bool     result_overflow;    /* zmpio_dsp_ctrl STATUS bit1, sticky until cleared */
} fpga_dsp_hal_counters_t;

/*
 * Brings the PL shell into its normal operating state: pulses SOFT_RESET and
 * leaves RUN and IRQ_ENABLE set in zmpio_dsp_ctrl CONTROL. Does NOT touch the
 * GIC -- interrupt routing is fpga_dsp_hal_irq_start(), which has its own
 * ordering requirement against CPU0.
 */
int fpga_dsp_hal_init(void);

/* Pulses CONTROL.SOFT_RESET (assert, then clear and restore RUN|IRQ_ENABLE),
 * resetting the DSP pipeline (dsp_core_axis_top, mmio_axis_bridge) and the
 * FIFO and counters of this module, independently of axi_iic_0/rst_ps7_0_49M
 * -- see the CONTROL register map in zmpio_dsp_ctrl.v. Called once at startup
 * by fpga_dsp_hal_init(), and on demand by ipc_v3.c for the ABI v3
 * DSP_SOFT_RESET command. Safe to call from any task context after init: it
 * touches only zmpio_dsp_ctrl MMIO registers. */
void fpga_dsp_hal_soft_reset_pulse(void);

/* Fault injection (ABI v3 FIFO_FULL_INJECT, ipc_v3.c): arms a deadline hold_ms
 * from now, clamped to APP_FIFO_FULL_INJECT_MAX_HOLD_MS. Touches no MMIO
 * register -- it only records a tick value that
 * fpga_dsp_hal_fault_inject_stall_active() checks. The caller
 * (fpga_result_task, main.c) is what actually skips its drain loop while this
 * reports true, letting the feature production of the DSP core fill the
 * 64-entry FIFO and increment DROP_COUNT. Safe to call from any task
 * context. */
void fpga_dsp_hal_fault_inject_stall_arm(uint32_t hold_ms);

/* True from the moment fpga_dsp_hal_fault_inject_stall_arm() is called until
 * its (clamped) deadline elapses. */
bool fpga_dsp_hal_fault_inject_stall_active(void);

/* The clamped hold_ms from the most recent fpga_dsp_hal_fault_inject_stall_arm()
 * call -- ipc_v3.c reads this once to fill zmpio_v3_fifo_full_inject_ack_t.hold_ms_applied. */
uint32_t fpga_dsp_hal_fault_inject_stall_hold_ms_applied(void);

/* Connects, targets (to CPU1), and enables the zmpio_dsp_ctrl IRQ. Must be
 * called from a task, AFTER vTaskStartScheduler(), never from main() before
 * it: the tick-timer setup of the port reprograms the shared GIC in a way that
 * silently un-routes any interrupt targeted before the scheduler starts. Full
 * reasoning in the doc comment on this function in fpga_dsp_hal.c. */
void fpga_dsp_hal_irq_start(void);

/* Re-arms the zmpio_dsp_ctrl IRQ if its GIC enable bit was cleared behind our
 * back, and reports whether it had to. CPU0 owns the GIC distributor in this
 * AMP split (CPU1 builds with USE_AMP=1), and any distributor re-init on CPU0
 * clears the enable bit and resets the priority of every SPI, this one
 * included. One MMIO read when nothing is wrong, so fpga_result_task calls it
 * on every semaphore timeout. */
bool fpga_dsp_hal_irq_watchdog(void);

/* True when the zmpio_dsp_ctrl feature FIFO has at least one frame waiting
 * (STATUS bit0), without popping anything. Lets fpga_result_task tell "no IRQ
 * because there is no work" apart from "no IRQ although work is pending",
 * i.e. gives it a polled fallback that does not depend on a healthy GIC. */
bool fpga_dsp_hal_feature_ready(void);

/* Pushes one raw MPU6050 sample through mmio_axis_bridge. Never blocks: if the
 * bridge is still busy with a previous sample (BUSY=1), the new one is dropped
 * and counted in the mmio_axis_bridge DROP_COUNT. */
void fpga_dsp_hal_push_sample(int16_t accel_x, int16_t accel_y, int16_t accel_z,
                              int16_t temperature, int16_t gyro_x, int16_t gyro_y,
                              int16_t gyro_z, uint16_t flags,
                              uint32_t sample_sequence);

/* Pops one feature frame from the zmpio_dsp_ctrl FIFO if one is available.
 * Returns false (out untouched) when the FIFO is empty. */
bool fpga_dsp_hal_pop_feature(fpga_feature_frame_t *out);

/* irq_out is level-triggered and stays asserted for as long as the FIFO is
 * non-empty (zmpio_dsp_ctrl.v: feature_ready = !fifo_empty); it does not
 * self-clear on ISR entry or EOI the way a pulse IRQ would. fpga_dsp_hal_isr()
 * calls this first, before giving the semaphore, so the GIC cannot re-enter it
 * back-to-back while fpga_result_task is still working through the FIFO -- an
 * IRQ storm that would starve every task, including the only one that can
 * clear the condition. fpga_result_task must call fpga_dsp_hal_irq_enable()
 * again only after it has drained the FIFO to empty, right before it goes back
 * to blocking on the semaphore. */
void fpga_dsp_hal_irq_disable(void);
void fpga_dsp_hal_irq_enable(void);

/* Snapshot of the diagnostic counters described above. Clears the sticky
 * result_overflow bit in hardware as a side effect (write-1-to-clear), so a
 * true result means "at least one overflow event happened since the previous
 * call". */
void fpga_dsp_hal_get_counters(fpga_dsp_hal_counters_t *out);

/* DSP_HEALTH: fault and health tracking derived from the counters above.
 * state and last_fault use the same numeric encoding as
 * zlog_dsp_health_state_t / zlog_dsp_fault_t in zlog_feature_v2.h (0/1 and
 * 0..3) so main.c can copy them straight into a zlog_dsp_health_t record --
 * this header stays free of any ZLOG dependency on purpose. */
typedef struct {
    uint8_t  state;                 /* 0 = OK, 1 = DEGRADED */
    uint8_t  last_fault;            /* 0 = none, 1..3 = cause of last fault seen */
    bool     state_changed;         /* true only on the poll that flips state */
    uint32_t consecutive_fault_count;
} fpga_dsp_hal_health_t;

/* Call once per drained FIFO, i.e. once per fpga_dsp_hal_get_counters() call
 * in fpga_result_task, in the same task-context loop rule_anomaly_evaluate()
 * follows. Compares the counters against the previous snapshot, so a drop or
 * overflow counter that has not moved is not counted as a new fault, and
 * applies the APP_DSP_HEALTH_FAULT_STREAK_TO_DEGRADED /
 * APP_DSP_HEALTH_OK_STREAK_TO_RECOVER hysteresis from app_config.h before
 * flipping state, so a single isolated drop cannot flap it. */
void fpga_dsp_hal_update_health(const fpga_dsp_hal_counters_t *counters,
                                fpga_dsp_hal_health_t *out);

/* Call exactly once per STOP -> config -> START cycle, so that CONFIG_SEQ
 * increments by exactly one per cycle. It only mirrors the value into the
 * zmpio_dsp_ctrl CONFIG_SEQ scratch register; no other hardware behaviour is
 * tied to it. */
void fpga_dsp_hal_bump_config_seq(void);

/* Binary semaphore given by the zmpio_dsp_ctrl ISR; fpga_result_task (main.c)
 * blocks on this instead of polling STATUS in the normal path. Takes a
 * FreeRTOS SemaphoreHandle_t as an opaque void* so this header does not have
 * to pull in FreeRTOS.h/semphr.h for its own sake. */
void fpga_dsp_hal_set_result_semaphore(void *semaphore_handle);

#endif
