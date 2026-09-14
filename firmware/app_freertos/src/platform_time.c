#include "platform_time.h"

#include "FreeRTOS.h"
#include "task.h"
#include "xil_io.h"
#include "xparameters.h"
#include "xscutimer_hw.h"

/* Cortex-A9 global timer registers (shared by CPU0 and CPU1). */
#define ARM_GLOBAL_TIMER_BASE       0xF8F00200U
#define ARM_GLOBAL_TIMER_LOW        (ARM_GLOBAL_TIMER_BASE + 0x00U)
#define ARM_GLOBAL_TIMER_HIGH       (ARM_GLOBAL_TIMER_BASE + 0x04U)
#define ARM_GLOBAL_TIMER_CONTROL    (ARM_GLOBAL_TIMER_BASE + 0x08U)
#define ARM_GLOBAL_TIMER_ENABLE     0x00000001U
#define ARM_GLOBAL_TIMER_PRESCALER_MASK  0x0000FF00U
#define ARM_GLOBAL_TIMER_PRESCALER_SHIFT 8U

/* The private timer and the global-timer prescaler input run from
 * CPU_3x2x, i.e. CPU/2. */
#define APU_TIMER_INPUT_CLOCK_HZ \
    ((uint32_t)(XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2U))

static uint64_t read_global_timer(void)
{
    uint32_t high_before;
    uint32_t high_after;
    uint32_t low;

    /* The counter can roll from LOW into HIGH between MMIO reads. */
    do {
        high_before = Xil_In32(ARM_GLOBAL_TIMER_HIGH);
        low = Xil_In32(ARM_GLOBAL_TIMER_LOW);
        high_after = Xil_In32(ARM_GLOBAL_TIMER_HIGH);
    } while (high_before != high_after);

    return ((uint64_t)high_after << 32U) | (uint64_t)low;
}

static uint32_t global_timer_counts_per_second(void)
{
    const uint32_t control = Xil_In32(ARM_GLOBAL_TIMER_CONTROL);
    const uint32_t prescaler =
        (control & ARM_GLOBAL_TIMER_PRESCALER_MASK) >>
        ARM_GLOBAL_TIMER_PRESCALER_SHIFT;

    /* The shared timer increments at CPU_3x2x / (PRESCALER + 1).
     * Linux may legitimately leave a non-zero prescaler in the shared
     * control register, so CPU1 must account for it rather than clearing it. */
    return APU_TIMER_INPUT_CLOCK_HZ / (prescaler + 1U);
}

void platform_time_init(void)
{
    uint32_t control = Xil_In32(ARM_GLOBAL_TIMER_CONTROL);

    /* Linux normally enables this timer.  Starting it here as well makes
     * timestamps valid when CPU1 is released before the Linux clocksource. */
    if ((control & ARM_GLOBAL_TIMER_ENABLE) == 0U) {
        Xil_Out32(ARM_GLOBAL_TIMER_CONTROL,
                  control | ARM_GLOBAL_TIMER_ENABLE);
    }
}

void platform_freertos_tick_fix(void)
{
    const uint32_t reload =
        APU_TIMER_INPUT_CLOCK_HZ / configTICK_RATE_HZ;

    /* This function runs as the first action of the highest-priority IPC
     * task.  The scheduler has configured the private timer by then, so
     * replace both its reload and live counter atomically on CPU1. */
    taskENTER_CRITICAL();
    Xil_Out32(XPAR_SCUTIMER_BASEADDR + XSCUTIMER_LOAD_OFFSET, reload);
    Xil_Out32(XPAR_SCUTIMER_BASEADDR + XSCUTIMER_COUNTER_OFFSET, reload);
    Xil_Out32(XPAR_SCUTIMER_BASEADDR + XSCUTIMER_ISR_OFFSET,
              XSCUTIMER_ISR_EVENT_FLAG_MASK);
    taskEXIT_CRITICAL();
}

uint32_t platform_time_counter(void)
{
    return Xil_In32(ARM_GLOBAL_TIMER_LOW);
}

uint32_t platform_time_us_to_counts(uint32_t microseconds)
{
    const uint32_t counts_per_second = global_timer_counts_per_second();
    uint32_t counts_per_us = counts_per_second / 1000000U;

    /* A zero factor would turn every deadline into "already expired"; the
     * timer never runs that slowly on this part, but a corrupted prescaler
     * in the shared control register must not disable the timeout. */
    if (counts_per_us == 0U) {
        counts_per_us = 1U;
    }
    if (microseconds > (0xFFFFFFFFU / counts_per_us)) {
        return 0xFFFFFFFFU;
    }
    return microseconds * counts_per_us;
}

uint64_t platform_time_us(void)
{
    const uint64_t ticks = read_global_timer();
    const uint64_t counts_per_second =
        (uint64_t)global_timer_counts_per_second();

    return (ticks / counts_per_second) * 1000000ULL +
           ((ticks % counts_per_second) * 1000000ULL) /
               counts_per_second;
}
