#ifndef CPU1_LOG_H
#define CPU1_LOG_H

#include "app_config.h"

/*
 * CPU1 logs on PS UART1 (EMIO, K19 TX / M19 RX, JP2 header) instead of the
 * PS UART0 that CPU0/Linux share as their boot console.  UART1 has no other
 * consumer, so this can stay enabled at all times without the interleaved-
 * output problem a shared UART would cause.  APP_CPU1_UART_LOG_ENABLED is
 * kept only to strip log strings out of the build when they are not wanted.
 */
#if APP_CPU1_UART_LOG_ENABLED
#include "uart1_log.h"
#define CPU1_LOG(...) uart1_log_write(__VA_ARGS__)
#else
#define CPU1_LOG(...) do { } while (0)
#endif

#endif
