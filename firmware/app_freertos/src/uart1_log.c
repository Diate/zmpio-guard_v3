#include "uart1_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "xparameters.h"
#include "xstatus.h"
#include "xuartps.h"

static XUartPs uart1_instance;
static bool uart1_ready;

int uart1_log_init(void)
{
    XUartPs_Config *config = XUartPs_LookupConfig(XPAR_XUARTPS_1_BASEADDR);

    if (config == NULL) {
        return XST_FAILURE;
    }

    /*
     * PCW_UART1_BAUD_RATE=115200 in the Vivado PS7 config is metadata only
     * -- the FSBL never programs UART1's baud divisors because UART0 is its
     * console.  XUartPs_CfgInitialize() applies XUARTPS_DFT_BAUDRATE
     * (115200) itself and leaves the device in polled 8N1 mode, so this call
     * is what actually brings UART1 up at the documented rate.
     */
    if (XUartPs_CfgInitialize(&uart1_instance, config, config->BaseAddress) !=
        XST_SUCCESS) {
        return XST_FAILURE;
    }

    uart1_ready = true;
    return XST_SUCCESS;
}

void uart1_log_write(const char *format, ...)
{
    char buffer[160];
    va_list args;
    int length;

    if (!uart1_ready) {
        return;
    }

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(buffer)) {
        length = (int)sizeof(buffer) - 1;
    }

    for (int i = 0; i < length; ++i) {
        XUartPs_SendByte(uart1_instance.Config.BaseAddress,
                         (u8)buffer[i]);
    }
}
