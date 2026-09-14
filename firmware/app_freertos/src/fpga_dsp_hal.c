#include "fpga_dsp_hal.h"

#include "app_config.h"
#include "cpu1_log.h"
#include "platform_time.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "xil_exception.h"
#include "xil_io.h"
#include "xinterrupt_wrap.h"

/* GIC distributor base (zynq-7000.dtsi: interrupt-controller@f8f01000) and
 * the SPI-local encoding XSetupInterruptSystem()/XEnableIntrId() expect:
 * IntrId = (Int_Id - 32), type bits left at 0 (SPI) and trigger bits left
 * at 0 (level). See fpga_dsp_hal_irq_start()'s doc comment for why this
 * must go through XSetupInterruptSystem() and not XScuGic_* directly. */
#define ZMPIO_DSP_GIC_DIST_BASEADDR 0xF8F01000U
#define ZMPIO_DSP_IRQ_LOCAL_ID      (FPGA_DSP_HAL_IRQ_ID - 32U)

/* Must equal the FreeRTOS tick priority, and must never be
 * XINTERRUPT_DEFAULT_PRIORITY (0xA0): irq_out is level-triggered, held high
 * for as long as the feature FIFO is non-empty (zmpio_dsp_ctrl.v:
 * feature_ready = !fifo_empty), and on the ARM GIC a lower number outranks.
 * Any higher-priority value keeps winning arbitration over the tick while the
 * FIFO is still draining, which starves vTaskDelay() system-wide. At equal
 * priority the lower GIC ID of the tick (29 vs 62) wins the tie. */
#define ZMPIO_DSP_IRQ_PRIORITY \
    (portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT)

/* GIC distributor registers this driver has to inspect directly (UG585
 * Appendix B "mpcore" / ARM PL390 TRM), because the Xilinx driver offers no
 * "is this interrupt still enabled?" query. ICDDCR bits[1:0] gate forwarding
 * of EVERY interrupt to EVERY CPU interface -- while they are 0 not even the
 * FreeRTOS tick is delivered. ICDISER<n> bit (id % 32) is the enable bit for
 * interrupt id; for SPI 62 that is ICDISER1 bit 30, at 0xF8F01104. */
#define ZMPIO_GIC_ICDDCR         (ZMPIO_DSP_GIC_DIST_BASEADDR + 0x000U)
#define ZMPIO_GIC_ICDDCR_EN_MASK 0x00000003U
#define ZMPIO_GIC_ICDISER        (ZMPIO_DSP_GIC_DIST_BASEADDR + 0x100U + \
                                  4U * (FPGA_DSP_HAL_IRQ_ID / 32U))
#define ZMPIO_GIC_ICDISER_MASK   (1U << (FPGA_DSP_HAL_IRQ_ID % 32U))

/* ICDIPTR (GICD_ITARGETSR), UG585 Appendix B / ARM PL390 TRM ch.4.3.11: one
 * BYTE per interrupt ID (4 IDs per 32-bit word), each bit selecting a CPU
 * interface -- bit0=CPU0, bit1=CPU1 on this dual-Cortex-A9 GIC. For SPI 62
 * that byte is at 0xF8F01000 + 0x800 + 4*(62/4) = 0xF8F0183C, byte index
 * 62%4=2 (bits[23:16]).
 *
 * This driver must WRITE the byte, never OR into it: force CPU1-exclusive
 * (0x02) on every (re)arm. The Linux GIC core (gic_dist_init(), which runs on
 * CPU0 in this AMP split and is the same call that wipes ICDISER above)
 * overwrites every SPI target byte to the booting CPU, while the Enable() step
 * of XSetupInterruptSystem() only ORs the calling target bit back in. A naive
 * re-arm would therefore leave SPI 62 targeting both CPUs, where no Linux
 * driver claims it -- every delivery becomes a spurious trap on CPU0, and a
 * kernel auto-masking that line would clear the per-SPI enable bit and kill
 * CPU1 delivery with it. Same sole-owner discipline as spi0/axi_iic_0 being
 * disabled in the Linux device tree, enforced in hardware instead. */
#define ZMPIO_GIC_ICDIPTR            (ZMPIO_DSP_GIC_DIST_BASEADDR + 0x800U + \
                                      4U * (FPGA_DSP_HAL_IRQ_ID / 4U))
#define ZMPIO_GIC_ICDIPTR_BYTE_SHIFT ((FPGA_DSP_HAL_IRQ_ID % 4U) * 8U)
#define ZMPIO_GIC_ICDIPTR_BYTE_MASK  (0xFFU << ZMPIO_GIC_ICDIPTR_BYTE_SHIFT)
#define ZMPIO_GIC_TARGET_CPU1        0x02U

/* Register offsets -- must match hardware/rtl/src/mmio_axis_bridge.v exactly. */
#define BRIDGE_REG_SAMPLE_W0  0x00U
#define BRIDGE_REG_SAMPLE_W1  0x04U
#define BRIDGE_REG_SAMPLE_W2  0x08U
#define BRIDGE_REG_SAMPLE_W3  0x0CU
#define BRIDGE_REG_SAMPLE_W4  0x10U /* commit strobe */
#define BRIDGE_REG_DROP_COUNT 0x18U

/* Register offsets -- must match hardware/rtl/src/zmpio_dsp_ctrl.v exactly. */
#define CTRL_REG_CONTROL        0x00U
#define CTRL_REG_CONFIG_SEQ     0x04U
#define CTRL_REG_STATUS         0x08U
#define CTRL_REG_FEATURE_COUNT  0x0CU
#define CTRL_REG_DROP_COUNT     0x10U
#define CTRL_REG_FEATURE_POP0   0x18U
#define CTRL_FEATURE_POP_WORDS  12U

#define CTRL_CONTROL_RUN_BIT        (1U << 0)
#define CTRL_CONTROL_SOFT_RESET_BIT (1U << 1)
#define CTRL_CONTROL_IRQ_ENABLE_BIT (1U << 2)

#define CTRL_STATUS_FEATURE_READY_BIT   (1U << 0)
#define CTRL_STATUS_RESULT_OVERFLOW_BIT (1U << 1)

static const uint32_t mmio_bridge_base = FPGA_DSP_HAL_MMIO_BRIDGE_BASEADDR;
static const uint32_t dsp_ctrl_base = FPGA_DSP_HAL_DSP_CTRL_BASEADDR;
static SemaphoreHandle_t result_semaphore = NULL;

/* FIFO-full fault-injection state (fpga_dsp_hal_fault_inject_stall_* below).
 * Single writer (ipc_v3.c, on ipc_rx_task), single reader (fpga_result_task,
 * main.c), word-sized: plain volatile is enough, as for every other
 * cross-task field in this codebase. */
static volatile TickType_t fault_inject_stall_until_tick = 0U;
static volatile uint32_t fault_inject_stall_hold_ms_applied = 0U;

static void fpga_dsp_hal_isr(void *callback_ref)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    (void)callback_ref;

    /* Mask at the source before doing anything else -- see the doc comment
     * on fpga_dsp_hal_irq_enable() in fpga_dsp_hal.h. Without this the level
     * IRQ stays asserted (FIFO still non-empty) and the GIC re-enters this
     * ISR again immediately after EOI, forever, since only fpga_result_task
     * can actually drain the FIFO and it never gets a chance to run. */
    fpga_dsp_hal_irq_disable();

    if (result_semaphore != NULL) {
        (void)xSemaphoreGiveFromISR(result_semaphore, &higher_priority_task_woken);
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* Pulse SOFT_RESET, then leave RUN+IRQ_ENABLE set for normal operation. See
 * the CONTROL register map in zmpio_dsp_ctrl.v for what this resets on the PL
 * side: the DSP pipeline plus FIFO and counters, deliberately independent of
 * axi_iic_0/rst_ps7_0_49M. Used at init and by the ABI v3 DSP_SOFT_RESET
 * command (ipc_v3.c). */
void fpga_dsp_hal_soft_reset_pulse(void)
{
    Xil_Out32(dsp_ctrl_base + CTRL_REG_CONTROL, CTRL_CONTROL_SOFT_RESET_BIT);
    Xil_Out32(dsp_ctrl_base + CTRL_REG_CONTROL,
             CTRL_CONTROL_RUN_BIT | CTRL_CONTROL_IRQ_ENABLE_BIT);
}

void fpga_dsp_hal_fault_inject_stall_arm(uint32_t hold_ms)
{
    uint32_t clamped_ms = (hold_ms > APP_FIFO_FULL_INJECT_MAX_HOLD_MS) ?
                               APP_FIFO_FULL_INJECT_MAX_HOLD_MS : hold_ms;

    fault_inject_stall_hold_ms_applied = clamped_ms;
    fault_inject_stall_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(clamped_ms);
}

bool fpga_dsp_hal_fault_inject_stall_active(void)
{
    return ((int32_t)(fault_inject_stall_until_tick - xTaskGetTickCount())) > 0;
}

uint32_t fpga_dsp_hal_fault_inject_stall_hold_ms_applied(void)
{
    return fault_inject_stall_hold_ms_applied;
}

int fpga_dsp_hal_init(void)
{
    fpga_dsp_hal_soft_reset_pulse();
    return 0;
}

static bool fpga_dsp_hal_gic_dist_enabled(void)
{
    return (Xil_In32(ZMPIO_GIC_ICDDCR) & ZMPIO_GIC_ICDDCR_EN_MASK) != 0U;
}

static bool fpga_dsp_hal_gic_irq_armed(void)
{
    return (Xil_In32(ZMPIO_GIC_ICDISER) & ZMPIO_GIC_ICDISER_MASK) != 0U;
}

static bool fpga_dsp_hal_gic_target_is_cpu1_only(void)
{
    const uint32_t byte = (Xil_In32(ZMPIO_GIC_ICDIPTR) &
                           ZMPIO_GIC_ICDIPTR_BYTE_MASK) >>
                          ZMPIO_GIC_ICDIPTR_BYTE_SHIFT;
    return byte == ZMPIO_GIC_TARGET_CPU1;
}

/* Read-modify-write: touches only this IRQ's byte, leaving the other three
 * interrupts' target bytes in the same 32-bit ICDIPTR word untouched. Forces
 * (not ORs) the byte to CPU1-exclusive -- see the doc comment on
 * ZMPIO_GIC_ICDIPTR above for why OR-in is not safe here. */
static void fpga_dsp_hal_gic_force_target_cpu1(void)
{
    uint32_t word = Xil_In32(ZMPIO_GIC_ICDIPTR);

    word &= ~ZMPIO_GIC_ICDIPTR_BYTE_MASK;
    word |= (ZMPIO_GIC_TARGET_CPU1 << ZMPIO_GIC_ICDIPTR_BYTE_SHIFT);
    Xil_Out32(ZMPIO_GIC_ICDIPTR, word);
}

/* Programs priority + trigger + handler + enable for this IRQ in one go.
 * Re-arming deliberately reuses the SAME call as the first arm: a distributor
 * re-init does not only clear the enable bit, it also resets this interrupt
 * priority to the BSP default 0xA0 -- which outranks the FreeRTOS tick and
 * would reintroduce the tick-starvation hazard described at
 * ZMPIO_DSP_IRQ_PRIORITY above. XSetupInterruptSystem() restores all of it
 * together, and never re-runs the destructive XScuGic_CfgInitialize() body
 * once the shared instance is ready (xinterrupt_wrap.c's
 * XConfigInterruptCntrl() returns early on InstancePtr->IsReady). */
static void fpga_dsp_hal_irq_program(void)
{
    int status;

    status = XSetupInterruptSystem(NULL, (void *)fpga_dsp_hal_isr,
                                   ZMPIO_DSP_IRQ_LOCAL_ID,
                                   ZMPIO_DSP_GIC_DIST_BASEADDR,
                                   ZMPIO_DSP_IRQ_PRIORITY);
    configASSERT(status == XST_SUCCESS);
    (void)status;

    /* The Enable() step of XSetupInterruptSystem() only ORs the CPU1 target
     * bit in; it never clears what a prior distributor re-init left in the
     * other bits. Force CPU1-exclusive on every arm, not just the first --
     * see the doc comment on ZMPIO_GIC_ICDIPTR. */
    fpga_dsp_hal_gic_force_target_cpu1();
}

/* Connect/enable the zmpio_dsp_ctrl IRQ through XSetupInterruptSystem()
 * (xinterrupt_wrap.c), the SAME GIC entry point the FreeRTOS tick timer
 * uses (portZynq7000.c's scutimer backend, since XPAR_XILTIMER_ENABLED is
 * defined for this BSP -- see FreeRTOS_SetupTickInterrupt()). This must NOT
 * go through port.c xPortInstallInterruptHandler()/XScuGic_InterruptMaptoCpu(),
 * even after the scheduler has started.
 *
 * Never create a second XScuGic instance. This BSP contains two uncoordinated
 * instances describing the SAME physical GIC: xInterruptController in
 * portZynq7000.c (used by xPortInstallInterruptHandler) and the file-static
 * XScuGicInstance in xinterrupt_wrap.c (used by the tick timer through
 * XSetupInterruptSystem). XScuGic_CfgInitialize() skips its destructive body
 * -- XScuGic_Stop() plus CPUInitialize(), which reprogram the live CPU
 * interface and distributor -- only when ITS OWN InstancePtr->IsReady is set,
 * and the two instances are different C structs. Installing through port.c
 * therefore re-runs that destructive init on live hardware however late it is
 * called, and kills the FreeRTOS tick for the whole system. Routing through
 * XSetupInterruptSystem() reuses the instance the tick timer already
 * initialised, so this call only connects, sets priority and trigger, and
 * enables (XScuGic_Enable() targets the calling CPU internally; no separate
 * MaptoCpu call is needed).
 *
 * Arming is also ordered against CPU0. In this AMP split CPU0, not CPU1, owns
 * the GIC distributor, and it brings the distributor up AFTER CPU1 is already
 * running: the CPU1 BSP is built with -DUSE_AMP=1, so DistributorInit() in
 * xscugic.c compiles to a bare return, while the first XSetupInterruptSystem()
 * on CPU0 runs the full DoDistributorInit(), which writes ICDICER=0xFFFFFFFF
 * over every SPI bank, resets every SPI priority to 0xA0, and only then sets
 * ICDDCR=1. Without ordering, CPU1 enables SPI 62 and CPU0 then wipes it --
 * and it fails only when the GIC starts from a reset state, because a
 * distributor left enabled by a previous CPU0 makes DistributorInit() skip the
 * wipe. Hence the two halves implemented here: wait for ICDDCR before the
 * first arm, and let fpga_result_task re-arm through
 * fpga_dsp_hal_irq_watchdog() if the enable bit disappears later (CPU0
 * restart, Linux boot, any second distributor re-init). */
void fpga_dsp_hal_irq_start(void)
{
    const uint32_t budget =
        platform_time_us_to_counts(APP_DSP_IRQ_GIC_WAIT_MS * 1000U);
    const uint32_t start = platform_time_counter();

    /* Wait for CPU0 to bring the GIC distributor up before programming
     * anything into it -- see this file's doc comment on why CPU0 owns it.
     *
     * Busy-wait on purpose, and on the global timer rather than the tick:
     * while ICDDCR is still 0 the distributor forwards nothing at all, so the
     * FreeRTOS tick IRQ never arrives and a vTaskDelay() here would block
     * forever. platform_time_counter() is the free-running ARM global timer
     * and has no tick dependency (the same primitive iic_polled.c uses). */
    while (!fpga_dsp_hal_gic_dist_enabled() &&
           ((platform_time_counter() - start) < budget)) {
        /* spin */
    }

    fpga_dsp_hal_irq_program();

    if (!fpga_dsp_hal_gic_irq_armed()) {
        CPU1_LOG("CPU1: DSP IRQ %u not enabled in GIC after arm "
                   "(ICDDCR=0x%08lx) -- watchdog will retry\r\n",
                   (unsigned)FPGA_DSP_HAL_IRQ_ID,
                   (unsigned long)Xil_In32(ZMPIO_GIC_ICDDCR));
    }
    if (!fpga_dsp_hal_gic_target_is_cpu1_only()) {
        CPU1_LOG("CPU1: DSP IRQ %u not CPU1-exclusive in GIC after arm "
                   "(ICDIPTR byte=0x%02lx) -- watchdog will retry\r\n",
                   (unsigned)FPGA_DSP_HAL_IRQ_ID,
                   (unsigned long)((Xil_In32(ZMPIO_GIC_ICDIPTR) &
                                    ZMPIO_GIC_ICDIPTR_BYTE_MASK) >>
                                   ZMPIO_GIC_ICDIPTR_BYTE_SHIFT));
    }
}

/* The enable bit and the target byte are checked, and if needed corrected,
 * independently: gic_dist_init() in Linux always wipes both together, but a
 * partial failure of fpga_dsp_hal_irq_program() (this function observing it
 * between its two writes) must not let one hazard hide behind the other.
 * The distinct CPU1_LOG lines, rather than the single generic message main.c
 * prints on any true return, are what let a capture show which of the two
 * drifted. */
bool fpga_dsp_hal_irq_watchdog(void)
{
    const bool armed = fpga_dsp_hal_gic_irq_armed();
    const bool target_ok = fpga_dsp_hal_gic_target_is_cpu1_only();

    if (armed && target_ok) {
        return false;
    }

    if (!armed) {
        CPU1_LOG("CPU1: DSP IRQ %u enable bit cleared in GIC (ICDISER=0x%08lx) "
                   "-- forcing back\r\n",
                   (unsigned)FPGA_DSP_HAL_IRQ_ID,
                   (unsigned long)Xil_In32(ZMPIO_GIC_ICDISER));
    }

    if (!target_ok) {
        CPU1_LOG("CPU1: DSP IRQ %u ICDIPTR drifted from CPU1-exclusive "
                   "(byte=0x%02lx) -- forcing back\r\n",
                   (unsigned)FPGA_DSP_HAL_IRQ_ID,
                   (unsigned long)((Xil_In32(ZMPIO_GIC_ICDIPTR) &
                                    ZMPIO_GIC_ICDIPTR_BYTE_MASK) >>
                                   ZMPIO_GIC_ICDIPTR_BYTE_SHIFT));
    }

    fpga_dsp_hal_irq_program();
    return true;
}

bool fpga_dsp_hal_feature_ready(void)
{
    return (Xil_In32(dsp_ctrl_base + CTRL_REG_STATUS) &
            CTRL_STATUS_FEATURE_READY_BIT) != 0U;
}

void fpga_dsp_hal_push_sample(int16_t accel_x, int16_t accel_y, int16_t accel_z,
                              int16_t temperature, int16_t gyro_x, int16_t gyro_y,
                              int16_t gyro_z, uint16_t flags,
                              uint32_t sample_sequence)
{
    const uint32_t w0 = (uint32_t)(uint16_t)accel_x |
                        ((uint32_t)(uint16_t)accel_y << 16);
    const uint32_t w1 = (uint32_t)(uint16_t)accel_z |
                        ((uint32_t)(uint16_t)temperature << 16);
    const uint32_t w2 = (uint32_t)(uint16_t)gyro_x |
                        ((uint32_t)(uint16_t)gyro_y << 16);
    const uint32_t w3 = (uint32_t)(uint16_t)gyro_z | ((uint32_t)flags << 16);

    Xil_Out32(mmio_bridge_base + BRIDGE_REG_SAMPLE_W0, w0);
    Xil_Out32(mmio_bridge_base + BRIDGE_REG_SAMPLE_W1, w1);
    Xil_Out32(mmio_bridge_base + BRIDGE_REG_SAMPLE_W2, w2);
    Xil_Out32(mmio_bridge_base + BRIDGE_REG_SAMPLE_W3, w3);
    /* Writing W4 last is the commit strobe -- see mmio_axis_bridge.v. */
    Xil_Out32(mmio_bridge_base + BRIDGE_REG_SAMPLE_W4, sample_sequence);
}

void fpga_dsp_hal_irq_disable(void)
{
    Xil_Out32(dsp_ctrl_base + CTRL_REG_CONTROL, CTRL_CONTROL_RUN_BIT);
}

void fpga_dsp_hal_irq_enable(void)
{
    Xil_Out32(dsp_ctrl_base + CTRL_REG_CONTROL,
             CTRL_CONTROL_RUN_BIT | CTRL_CONTROL_IRQ_ENABLE_BIT);
}

bool fpga_dsp_hal_pop_feature(fpga_feature_frame_t *out)
{
    uint32_t status;
    uint32_t words[CTRL_FEATURE_POP_WORDS];
    uint32_t i;

    status = Xil_In32(dsp_ctrl_base + CTRL_REG_STATUS);
    if ((status & CTRL_STATUS_FEATURE_READY_BIT) == 0U) {
        return false;
    }

    /* Reading the last word (offset 0x44) is what advances the FIFO in
     * hardware -- see zmpio_dsp_ctrl.v FEATURE_POP. */
    for (i = 0U; i < CTRL_FEATURE_POP_WORDS; ++i) {
        words[i] = Xil_In32(dsp_ctrl_base + CTRL_REG_FEATURE_POP0 + 4U * i);
    }

    out->frame_sequence = words[0];
    out->window_end_sample_sequence = words[1];
    out->rms_q24_8 = words[2];
    out->peak_q24_8 = words[3];
    out->variance_q32_0 = words[4];
    out->kurtosis_q16_16 = words[5];
    out->dominant_frequency_q16_16 = words[6];
    out->dominant_power_q32_0 = words[7];
    out->band_energy_q32_0[0] = words[8];
    out->band_energy_q32_0[1] = words[9];
    out->band_energy_q32_0[2] = words[10];
    out->band_energy_q32_0[3] = words[11];
    return true;
}

void fpga_dsp_hal_get_counters(fpga_dsp_hal_counters_t *out)
{
    uint32_t status;

    out->feature_count = Xil_In32(dsp_ctrl_base + CTRL_REG_FEATURE_COUNT);
    out->ctrl_drop_count = Xil_In32(dsp_ctrl_base + CTRL_REG_DROP_COUNT);
    out->bridge_drop_count = Xil_In32(mmio_bridge_base + BRIDGE_REG_DROP_COUNT);

    status = Xil_In32(dsp_ctrl_base + CTRL_REG_STATUS);
    out->result_overflow = (status & CTRL_STATUS_RESULT_OVERFLOW_BIT) != 0U;
    if (out->result_overflow) {
        /* Write-1-to-clear the sticky bit now that the caller has observed
         * it -- next call reports only overflow events since this one. */
        Xil_Out32(dsp_ctrl_base + CTRL_REG_STATUS, CTRL_STATUS_RESULT_OVERFLOW_BIT);
    }
}

void fpga_dsp_hal_update_health(const fpga_dsp_hal_counters_t *counters,
                                fpga_dsp_hal_health_t *out)
{
    static uint32_t prev_ctrl_drop = 0U;
    static uint32_t prev_bridge_drop = 0U;
    static uint8_t state = 0U;       /* 0 = OK */
    static uint8_t last_fault = 0U;  /* 0 = none */
    static uint32_t fault_streak = 0U;
    static uint32_t ok_streak = 0U;
    bool fault_now;

    if (counters->result_overflow) {
        fault_now = true;
        last_fault = 3U; /* zlog_dsp_fault_t: ZLOG_DSP_FAULT_RESULT_OVERFLOW */
    } else if (counters->ctrl_drop_count != prev_ctrl_drop) {
        fault_now = true;
        last_fault = 1U; /* ZLOG_DSP_FAULT_CTRL_FIFO_DROP */
    } else if (counters->bridge_drop_count != prev_bridge_drop) {
        fault_now = true;
        last_fault = 2U; /* ZLOG_DSP_FAULT_BRIDGE_SAMPLE_DROP */
    } else {
        fault_now = false;
    }
    prev_ctrl_drop = counters->ctrl_drop_count;
    prev_bridge_drop = counters->bridge_drop_count;

    out->state_changed = false;
    if (fault_now) {
        ++fault_streak;
        ok_streak = 0U;
        if ((state == 0U) &&
            (fault_streak >= APP_DSP_HEALTH_FAULT_STREAK_TO_DEGRADED)) {
            state = 1U;
            out->state_changed = true;
        }
    } else {
        ++ok_streak;
        fault_streak = 0U;
        if ((state == 1U) &&
            (ok_streak >= APP_DSP_HEALTH_OK_STREAK_TO_RECOVER)) {
            state = 0U;
            out->state_changed = true;
        }
    }

    out->state = state;
    out->last_fault = last_fault;
    out->consecutive_fault_count = fault_streak;
}

void fpga_dsp_hal_bump_config_seq(void)
{
    static uint32_t config_seq = 0U;

    ++config_seq;
    Xil_Out32(dsp_ctrl_base + CTRL_REG_CONFIG_SEQ, config_seq);
}

void fpga_dsp_hal_set_result_semaphore(void *semaphore_handle)
{
    result_semaphore = (SemaphoreHandle_t)semaphore_handle;
}
