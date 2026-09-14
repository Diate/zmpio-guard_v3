#ifndef UART1_LOG_H
#define UART1_LOG_H

/*
 * PS UART1 (EMIO, K19 TX / M19 RX, JP2 header) is the dedicated CPU1 debug
 * console. It is physically independent from PS UART0, which CPU0 and Linux
 * share as their boot console, so CPU1 can log here at any time without the
 * shared-UART conflict documented in cpu1_log.h.
 */
int uart1_log_init(void);
void uart1_log_write(const char *format, ...);

#endif
