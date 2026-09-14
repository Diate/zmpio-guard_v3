#ifndef PLATFORM_TIME_H
#define PLATFORM_TIME_H

#include <stdint.h>

/*
 * Start the shared 64-bit ARM global timer if neither CPU has started it yet.
 * CPU1 only reads the counter after this initialization and preserves the
 * shared control-register prescaler, so Linux can keep using the same timer
 * as its clocksource.
 */
void platform_time_init(void);

/*
 * Correct the FreeRTOS SCU private-timer reload after the scheduler starts.
 * Vitis 2023.2 SDT xiltimer uses the TTC/CPU_1x frequency for this timer even
 * though the Cortex-A9 private timer is clocked by CPU_3x2x (CPU/2).
 */
void platform_freertos_tick_fix(void);

uint64_t platform_time_us(void);

/*
 * Low 32 bits of the free-running global timer, plus the conversion factor
 * needed to turn a microsecond budget into a count of its increments.
 *
 * Intended for hardware-polling deadlines that must stay bounded no matter
 * what the peripheral does (see iic_polled.c). Deliberately cheaper than
 * platform_time_us(): no 64-bit division per poll, and no dependency on the
 * FreeRTOS tick, so the same deadline works before and after the scheduler
 * starts. Wrapping is handled by unsigned subtraction at the call site; the
 * counter wraps roughly every 13 s at CPU_3x2x = 333 MHz, which bounds any
 * single deadline these callers may use.
 */
uint32_t platform_time_counter(void);
uint32_t platform_time_us_to_counts(uint32_t microseconds);

#endif
