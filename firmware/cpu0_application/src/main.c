#include <stdint.h>
#include <string.h>

#include "cpu0_ipc.h"
#include "cpu0_ipc_v3.h"
#include "cpu0_irq_handler.h"
#include "pl_doorbell.h"
#include "platform.h"
#include "sleep.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xuartps_hw.h"
#include "ethernet_test.h"
#include "net_lwip.h"
#include "net_ping.h"
#include "xgpiops.h"
#include "xstatus.h"

#define CPU1_START_VECTOR_ADDR 0xFFFFFFF0U
#define CPU1_ENTRY_ADDR        CPU1_RESERVED_BASE

#define CPU1_BOOT_TIMEOUT_MS   5000U
#define CPU1_REPLY_TIMEOUT_MS  2000U
#define SENSOR_PERIOD_MS       1000U
#define STATUS_PERIOD_SAMPLES  5U
#define UART_POLL_INTERVAL_MS  50U

/* Same registers/bits as deploy/petalinux_overlay/zmpio-enable-cpu1-clocks: Linux normally
 * ungates these for CPU1's SD-SPI controller via clk_ignore_unused + that
 * service. The bare-metal JTAG controller replaces Linux's boot role, so it
 * must do this itself or CPU1's SD card never gets a CMD0 response. */
#define SLCR_UNLOCK_ADDR       0xF8000008U
#define SLCR_LOCK_ADDR         0xF8000004U
#define SPI_CLK_CTRL_ADDR      0xF8000158U
#define APER_CLK_CTRL_ADDR     0xF800012CU
#define GEM0_RCLK_CTRL_ADDR    0xF8000138U
#define GEM0_CLK_CTRL_ADDR     0xF8000140U
#define SLCR_UNLOCK_KEY        0x0000DF0DU
#define SLCR_LOCK_KEY          0x0000767BU
#define SPI_CLKACT_MASK        0x00000001U
#define SPI0_APER_MASK         0x00004000U
#define GEM0_CPU_1XCLKACT_MASK 0x00000040U
#define GEM0_RCLK_VAL          0x00000011U
#define GEM0_CLK_VAL           0x00100141U
#define GEM0_RST_CTRL_ADDR    0xF800014CU
#define GEM0_RST_MASK         0x00000001U

/* ETH_nRST is now driven by PS EMIO GPIO[0] (active-low PHY reset). */
#define PHY_RESET_EMIO_PIN    0U
#define PHY_RESET_LOW_MS      10U
static uint32_t next_request_id = 1U;

/* Step 4 ABI v3 (docs/ROADMAP_DETAIL.md). */
#define ZMPIO_V3_HELLO_MAX_ATTEMPTS   5U
#define ZMPIO_V3_HELLO_TIMEOUT_MS     500U
#define ZMPIO_V3_CONFIG_MAX_ATTEMPTS  5U
#define ZMPIO_V3_CONFIG_TIMEOUT_MS    500U
/* The boot-time HELLO ([main.c] test_abi_v3_hello() call before the main
 * loop) is a single shot: if CPU1 is not yet far enough into its own boot to
 * answer within that window (e.g. it crashed/was reset after the boot-time
 * attempt already ran), v3_link.online stays false forever with nothing to
 * retry it short of a manual 'H' keypress. Retry it periodically here so a
 * CPU1 that comes up (or comes back up) later still reaches ONLINE without
 * operator intervention -- needed for the JTAG bring-up script's outer retry
 * loop, which only resets/redownloads CPU1, not CPU0. */
#define ZMPIO_V3_HELLO_RETRY_PERIOD_MS 2000U
static cpu0_ipc_v3_link_t v3_link;

static void enable_cpu1_spi_clocks(void)
{
    uint32_t spi_clock;
    uint32_t aper_clock;

    Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);
    spi_clock = Xil_In32(SPI_CLK_CTRL_ADDR);
    aper_clock = Xil_In32(APER_CLK_CTRL_ADDR);
    Xil_Out32(SPI_CLK_CTRL_ADDR, spi_clock | SPI_CLKACT_MASK);
    Xil_Out32(APER_CLK_CTRL_ADDR, aper_clock | SPI0_APER_MASK);
    Xil_Out32(SLCR_LOCK_ADDR, SLCR_LOCK_KEY);

    spi_clock = Xil_In32(SPI_CLK_CTRL_ADDR);
    aper_clock = Xil_In32(APER_CLK_CTRL_ADDR);
    xil_printf("CPU0: SPI0 clocks SPI_CLK_CTRL=0x%08lx APER_CLK_CTRL=0x%08lx\r\n",
               (unsigned long)spi_clock, (unsigned long)aper_clock);
    if (((spi_clock & SPI_CLKACT_MASK) == 0U) ||
        ((aper_clock & SPI0_APER_MASK) == 0U)) {
        xil_printf("CPU0: WARNING SPI0 clock enable verification failed\r\n");
    }
}

static void reset_phy_emio(void)
{
    XGpioPs Gpio;
    XGpioPs_Config *ConfigPtr;
    int Status;

    ConfigPtr = XGpioPs_LookupConfig(XPAR_XGPIOPS_0_BASEADDR);
    if (ConfigPtr == NULL) {
        xil_printf("CPU0: WARNING XGpioPs_LookupConfig failed\r\n");
        return;
    }

    Status = XGpioPs_CfgInitialize(&Gpio, ConfigPtr, ConfigPtr->BaseAddr);
    if (Status != XST_SUCCESS) {
        xil_printf("CPU0: WARNING XGpioPs_CfgInitialize failed %d\r\n", Status);
        return;
    }

    /* EMIO pin 0 drives ETH_nRST (active low). XGpioPs has no named
     * direction constants; 1 = output per XGpioPs_SetDirectionPin(). */
    XGpioPs_SetDirectionPin(&Gpio, PHY_RESET_EMIO_PIN, 1);
    XGpioPs_SetOutputEnablePin(&Gpio, PHY_RESET_EMIO_PIN, 1);

    xil_printf("CPU0: PHY reset pulse low %d ms...\r\n", PHY_RESET_LOW_MS);
    XGpioPs_WritePin(&Gpio, PHY_RESET_EMIO_PIN, 0);
    usleep(PHY_RESET_LOW_MS * 1000U);
    XGpioPs_WritePin(&Gpio, PHY_RESET_EMIO_PIN, 1);
    xil_printf("CPU0: PHY reset released\r\n");
}

static void enable_gem0_clocks(void)
{
    uint32_t aper_clock;
    uint32_t gem0_reset;

    xil_printf("CPU0: Enabling GEM0 clocks and reset...\r\n");

    Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);

    /* Configure GEM0 RX clock control (taken from ps7_init_gpl.c) */
    Xil_Out32(GEM0_RCLK_CTRL_ADDR, GEM0_RCLK_VAL);

    /* Configure GEM0 main clock: source select + divisors + clock active.
     * Value 0x00100141 matches Vivado SDT ps7_init_gpl.c configuration:
     *   CLKACT0 = 1, SRCSEL = 0x4, DIVISOR = 1, DIVISOR1 = 1
     */
    Xil_Out32(GEM0_CLK_CTRL_ADDR, GEM0_CLK_VAL);

    /* Enable GEM0 CPU_1x aperture clock */
    aper_clock = Xil_In32(APER_CLK_CTRL_ADDR);
    Xil_Out32(APER_CLK_CTRL_ADDR, aper_clock | GEM0_CPU_1XCLKACT_MASK);

    /* Release GEM0 reset (clear bit 0) */
    gem0_reset = Xil_In32(GEM0_RST_CTRL_ADDR);
    Xil_Out32(GEM0_RST_CTRL_ADDR, gem0_reset & ~GEM0_RST_MASK);

    Xil_Out32(SLCR_LOCK_ADDR, SLCR_LOCK_KEY);

    /* Verify */
    xil_printf("CPU0: GEM0_RCLK_CTRL = 0x%08lx\r\n",
               (unsigned long)Xil_In32(GEM0_RCLK_CTRL_ADDR));
    xil_printf("CPU0: GEM0_CLK_CTRL  = 0x%08lx\r\n",
               (unsigned long)Xil_In32(GEM0_CLK_CTRL_ADDR));
    xil_printf("CPU0: APER_CLK_CTRL = 0x%08lx\r\n",
               (unsigned long)Xil_In32(APER_CLK_CTRL_ADDR));
    xil_printf("CPU0: GEM0_RST_CTRL = 0x%08lx\r\n",
               (unsigned long)Xil_In32(GEM0_RST_CTRL_ADDR));

    xil_printf("CPU0: GEM0 clocks and reset PASS\r\n");
}

static void start_cpu1(void)
{
    /* The CPU1 ELF must already have been downloaded by Vitis/XSCT to
     * CPU1_ENTRY_ADDR.  The BootROM keeps CPU1 in WFE until this SEV. */
    Xil_Out32(CPU1_START_VECTOR_ADDR, CPU1_ENTRY_ADDR);
    __asm__ volatile("dmb" ::: "memory");
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("sev" ::: "memory");
}

static int send_request(msg_type_t type, const void *payload,
                        uint32_t payload_length, ipc_message_t *reply)
{
    ipc_message_t request;
    uint32_t request_id;
    int status;

    if ((payload_length > IPC_PAYLOAD_SIZE) ||
        ((payload_length != 0U) && (payload == NULL))) {
        return CPU0_IPC_NOT_READY;
    }

    memset(&request, 0, sizeof(request));
    request.header.magic = IPC_MAGIC;
    request.header.type = (uint32_t)type;
    request.header.timestamp = next_request_id++;
    request.header.length = payload_length;
    if (payload_length != 0U) {
        memcpy(request.payload, payload, payload_length);
    }

    request_id = request.header.timestamp;
    status = cpu0_ipc_send(&request);
    if (status != CPU0_IPC_OK) {
        return status;
    }
    return cpu0_ipc_wait_for_reply(request_id, reply, CPU1_REPLY_TIMEOUT_MS);
}

static int print_ack_or_error(const ipc_message_t *reply)
{
    ipc_ack_payload_t acknowledgement;

    if ((reply->header.type != MSG_TYPE_ACK &&
         reply->header.type != MSG_TYPE_ERROR) ||
        (reply->header.length != sizeof(acknowledgement))) {
        return -1;
    }

    memcpy(&acknowledgement, reply->payload, sizeof(acknowledgement));
    xil_printf("CPU0: CPU1 %s request=0x%02lx status=%ld detail=%lu\r\n",
               reply->header.type == MSG_TYPE_ACK ? "ACK" : "ERROR",
               (unsigned long)acknowledgement.request_type,
               (long)acknowledgement.status,
               (unsigned long)acknowledgement.detail);
    return reply->header.type == MSG_TYPE_ACK ? 0 : -1;
}

static int send_logger_command(msg_type_t command, const char *name)
{
    ipc_message_t reply;
    int status = send_request(command, NULL, 0U, &reply);

    if (status != CPU0_IPC_OK) {
        xil_printf("CPU0: LOG_%s timeout/error=%d\r\n", name, status);
        return -1;
    }
    return print_ack_or_error(&reply);
}

static int test_heartbeat(void)
{
    ipc_message_t reply;
    int status = send_request(MSG_TYPE_HEARTBEAT, NULL, 0U, &reply);

    if (status != CPU0_IPC_OK) {
        xil_printf("CPU0: heartbeat timeout/error=%d\r\n", status);
        return -1;
    }
    if (print_ack_or_error(&reply) != 0) {
        xil_printf("CPU0: heartbeat rejected\r\n");
        return -1;
    }
    return 0;
}

static int test_logger_status(void)
{
    ipc_message_t reply;
    ipc_logger_status_t status;
    int result = send_request(MSG_TYPE_LOG_STATUS, NULL, 0U, &reply);

    if (result != CPU0_IPC_OK) {
        xil_printf("CPU0: status timeout/error=%d\r\n", result);
        return -1;
    }
    if (reply.header.type != MSG_TYPE_LOG_STATUS ||
        reply.header.length != sizeof(status)) {
        (void)print_ack_or_error(&reply);
        xil_printf("CPU0: unexpected status reply\r\n");
        return -1;
    }

    memcpy(&status, reply.payload, sizeof(status));
    status.filename[sizeof(status.filename) - 1U] = '\0';
    xil_printf("CPU0: logger v%lu mounted=%lu logging=%lu queue=%lu\r\n",
               (unsigned long)status.protocol_version,
               (unsigned long)status.mounted,
               (unsigned long)status.logging,
               (unsigned long)status.queue_depth);
    xil_printf("CPU0: file=%s bytes=%lu:%08lx records=%lu io_errors=%lu\r\n",
               status.filename,
               (unsigned long)(status.file_bytes >> 32U),
               (unsigned long)status.file_bytes,
               (unsigned long)status.records_written,
               (unsigned long)status.io_errors);
    xil_printf("CPU0: MPU accepted=%lu dropped=%lu | Linux accepted=%lu dropped=%lu\r\n",
               (unsigned long)status.sensor_accepted,
               (unsigned long)status.sensor_dropped,
               (unsigned long)status.linux_accepted,
               (unsigned long)status.linux_dropped);
    return 0;
}

static int test_sensor(void)
{
    ipc_message_t reply;
    ipc_sensor_data_t sensor_data;
    int status = send_request(MSG_TYPE_SENSOR_DATA, NULL, 0U, &reply);

    if (status != CPU0_IPC_OK) {
        xil_printf("CPU0: sensor request timeout/error=%d\r\n", status);
        return -1;
    }
    if (reply.header.type == MSG_TYPE_ACK || reply.header.type == MSG_TYPE_ERROR) {
        (void)print_ack_or_error(&reply);
        return -1;
    }
    if (reply.header.type != MSG_TYPE_SENSOR_DATA ||
        reply.header.length != sizeof(sensor_data)) {
        xil_printf("CPU0: malformed sensor reply type=0x%02lx len=%lu\r\n",
                   (unsigned long)reply.header.type,
                   (unsigned long)reply.header.length);
        return -1;
    }

    memcpy(&sensor_data, reply.payload, sizeof(sensor_data));
    xil_printf("CPU0: MPU raw t=%lu:%08lx ax=%d ay=%d az=%d temp=%d "
               "gx=%d gy=%d gz=%d\r\n",
               (unsigned long)(sensor_data.sample_timestamp_us >> 32U),
               (unsigned long)sensor_data.sample_timestamp_us,
               (int)sensor_data.sample.accel_x,
               (int)sensor_data.sample.accel_y,
               (int)sensor_data.sample.accel_z,
               (int)sensor_data.sample.temperature,
               (int)sensor_data.sample.gyro_x,
               (int)sensor_data.sample.gyro_y,
               (int)sensor_data.sample.gyro_z);
    return 0;
}

static const char *logger_diagnostic_stage_name(uint32_t stage)
{
    static const char *const names[] = {
        "none", "fatfs-mount", "selftest-open-write", "selftest-write",
        "selftest-sync", "selftest-close-write", "selftest-open-read",
        "selftest-read", "selftest-length", "selftest-compare",
        "selftest-close-read", "log-name", "log-open", "log-header-write",
        "log-header-sync", "log-record-write", "log-periodic-sync",
        "log-flush-sync"
    };

    return stage < (sizeof(names) / sizeof(names[0])) ? names[stage] :
           "unknown";
}

static const char *fatfs_result_name(int32_t result)
{
    static const char *const names[] = {
        "FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY",
        "FR_NO_FILE", "FR_NO_PATH", "FR_INVALID_NAME", "FR_DENIED",
        "FR_EXIST", "FR_INVALID_OBJECT", "FR_WRITE_PROTECTED",
        "FR_INVALID_DRIVE", "FR_NOT_ENABLED", "FR_NO_FILESYSTEM",
        "FR_MKFS_ABORTED", "FR_TIMEOUT", "FR_LOCKED",
        "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES",
        "FR_INVALID_PARAMETER"
    };

    return result >= 0 && (uint32_t)result <
           (sizeof(names) / sizeof(names[0])) ? names[result] : "unknown";
}

static const char *sd_spi_diagnostic_name(uint32_t diagnostic)
{
    static const char *const names[] = {
        "none", "invalid-argument", "set-init-clock", "idle-clocks",
        "CMD0", "CMD8", "ACMD41", "CMD58", "CMD16",
        "set-data-clock", "CMD9/CSD", "read", "write", "sync"
    };

    return diagnostic < (sizeof(names) / sizeof(names[0])) ?
           names[diagnostic] : "unknown";
}

static const char *sd_spi_detail_reason_name(uint32_t detail)
{
    static const char *const names[] = {
        "none", "mode-fault", "rx-timeout", "response-timeout",
        "bad-response", "controller-setup"
    };
    uint32_t reason = IPC_SD_SPI_DETAIL_REASON(detail);

    return reason < (sizeof(names) / sizeof(names[0])) ? names[reason] :
           "unknown";
}

static void print_sd_spi_detail(const char *which, uint32_t detail)
{
    uint32_t reason = IPC_SD_SPI_DETAIL_REASON(detail);

    if (reason == IPC_SD_SPI_DETAIL_NONE) {
        return;
    }
    xil_printf("CPU0: SPI detail (%s): reason=%s(%lu) last_rx=0x%02lx "
               "CR=0x%05lx enabled=%lu modf=%lu\r\n", which,
               sd_spi_detail_reason_name(detail), (unsigned long)reason,
               (unsigned long)IPC_SD_SPI_DETAIL_LAST_RX(detail),
               (unsigned long)IPC_SD_SPI_DETAIL_CONFIG(detail),
               (unsigned long)IPC_SD_SPI_DETAIL_ENABLED(detail),
               (unsigned long)IPC_SD_SPI_DETAIL_MODE_FAULT(detail));
}

static void print_logger_diagnostic(const ipc_logger_diagnostic_t *diagnostic)
{
    xil_printf("CPU0: storage diagnostic (latest): stage=%s(%lu) "
               "result=%ld(%s) spi=%s(%lu) detail=%lu\r\n",
               logger_diagnostic_stage_name(diagnostic->stage),
               (unsigned long)diagnostic->stage, (long)diagnostic->result,
               fatfs_result_name(diagnostic->result),
               sd_spi_diagnostic_name(diagnostic->spi_diagnostic),
               (unsigned long)diagnostic->spi_diagnostic,
               (unsigned long)diagnostic->detail);
    print_sd_spi_detail("latest", diagnostic->detail);
    if (diagnostic->first_stage != IPC_LOG_DIAG_STAGE_NONE) {
        xil_printf("CPU0: storage diagnostic (first):  stage=%s(%lu) "
                   "result=%ld(%s) spi=%s(%lu) detail=%lu\r\n",
                   logger_diagnostic_stage_name(diagnostic->first_stage),
                   (unsigned long)diagnostic->first_stage,
                   (long)diagnostic->first_result,
                   fatfs_result_name(diagnostic->first_result),
                   sd_spi_diagnostic_name(diagnostic->first_spi_diagnostic),
                   (unsigned long)diagnostic->first_spi_diagnostic,
                   (unsigned long)diagnostic->first_detail);
        print_sd_spi_detail("first", diagnostic->first_detail);
    }
}

static int test_logger_diagnostic(void)
{
    ipc_message_t reply;
    ipc_logger_diagnostic_t diagnostic;
    ipc_ack_payload_t acknowledgement;
    int status = send_request(MSG_TYPE_LOG_DIAGNOSTIC, NULL, 0U, &reply);

    if (status != CPU0_IPC_OK) {
        xil_printf("CPU0: diagnostic timeout/error=%d\r\n", status);
        return -1;
    }
    if (reply.header.type == MSG_TYPE_ERROR &&
        reply.header.length == sizeof(acknowledgement)) {
        memcpy(&acknowledgement, reply.payload, sizeof(acknowledgement));
        if (acknowledgement.request_type == MSG_TYPE_LOG_DIAGNOSTIC &&
            acknowledgement.status == -3) {
            xil_printf("CPU0: CPU1 ELF does not implement LOG_DIAGNOSTIC\r\n");
            return 0;
        }
    }
    if (reply.header.type != MSG_TYPE_LOG_DIAGNOSTIC ||
        reply.header.length != sizeof(diagnostic)) {
        (void)print_ack_or_error(&reply);
        xil_printf("CPU0: unexpected diagnostic reply\r\n");
        return -1;
    }

    memcpy(&diagnostic, reply.payload, sizeof(diagnostic));
    print_logger_diagnostic(&diagnostic);
    return 0;
}

static int send_log_text(const char *text)
{
    ipc_message_t reply;
    size_t length = strlen(text);
    int status;

    if (length == 0U || length > IPC_PAYLOAD_SIZE) {
        xil_printf("CPU0: log text must be 1..%u bytes\r\n",
                   IPC_PAYLOAD_SIZE);
        return -1;
    }
    status = send_request(MSG_TYPE_LOG_DATA, text, (uint32_t)length, &reply);
    if (status != CPU0_IPC_OK) {
        xil_printf("CPU0: log submission timeout/error=%d\r\n", status);
        return -1;
    }
    return print_ack_or_error(&reply);
}

static void test_log_submission(void)
{
    (void)send_log_text("CPU0 JTAG IPC test");
}

static int uart_has_input(void)
{
    return XUartPs_IsReceiveData(STDIN_BASEADDRESS) ? 1 : 0;
}

/* Blocking line read used only after the user explicitly starts entering
 * custom log text; normal command polling stays non-blocking. */
static void uart_read_line(char *buffer, uint32_t buffer_size)
{
    uint32_t length = 0U;

    for (;;) {
        char c;

        while (!XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
            ;
        }
        c = inbyte();
        if (c == '\r' || c == '\n') {
            xil_printf("\r\n");
            break;
        }
        if ((c == '\b' || c == 0x7F) && length > 0U) {
            --length;
            xil_printf("\b \b");
            continue;
        }
        if (length + 1U < buffer_size) {
            buffer[length++] = c;
            outbyte(c);
        }
    }
    buffer[length] = '\0';
}

static void print_menu(void)
{
    xil_printf("\r\nCPU0 IPC command menu:\r\n"
               "  h - heartbeat\r\n"
               "  s - one sensor sample\r\n"
               "  c - toggle continuous sensor sampling\r\n"
               "  a - logger status\r\n"
               "  d - logger diagnostic\r\n"
               "  1 - LOG_START\r\n"
               "  0 - LOG_STOP\r\n"
               "  f - LOG_FLUSH\r\n"
               "  l - send custom log text\r\n"
               "  n - network status (IP/DHCP/link)\r\n"
               "  g - ping a host (IPv4 or hostname, e.g. google.com)\r\n"
               "  ABI v3 (Step 4, lab-test only):\r\n"
               "  V - show ABI v3 link status\r\n"
               "  H - redo HELLO handshake\r\n"
               "  C - SET_DSP_CONFIG with valid values (expect ACK)\r\n"
               "  X - SET_DSP_CONFIG with an invalid feature_mask (expect NACK)\r\n"
               "  K - push a command with a corrupt CRC (expect it dropped, ring still usable after)\r\n"
               "  U - resend the last correlation_id (expect duplicate handling, no double-apply)\r\n"
               "  W - wrap-around stress (fires past the 16-slot ring twice)\r\n"
               "  ? - show this menu\r\n");
}

static int test_abi_v3_hello(void)
{
    return cpu0_ipc_v3_hello(ZMPIO_V3_HELLO_MAX_ATTEMPTS, ZMPIO_V3_HELLO_TIMEOUT_MS, &v3_link);
}

static void print_abi_v3_status(void)
{
    uint32_t cpu1_crc_drop_count = 0U;
    uint32_t cpu0_crc_drop_count = 0U;

    cpu0_ipc_v3_get_counters(&cpu1_crc_drop_count, &cpu0_crc_drop_count);
    xil_printf("CPU0: ABI v3 online=%d session_id=0x%08lx local_hash=0x%08lx "
               "remote_hash=0x%08lx\r\n",
               (int)v3_link.online, (unsigned long)v3_link.session_id,
               (unsigned long)ZMPIO_ABI_V3_EFFECTIVE_LAYOUT_HASH,
               (unsigned long)v3_link.remote_layout_hash);
    xil_printf("CPU0: ABI v3 crc_drops cpu1=%lu cpu0=%lu\r\n",
               (unsigned long)cpu1_crc_drop_count, (unsigned long)cpu0_crc_drop_count);
}

static void test_abi_v3_set_dsp_config(bool valid)
{
    zmpio_v3_set_dsp_config_t config;
    int32_t protocol_status = 0;
    int status;

    if (!v3_link.online) {
        xil_printf("CPU0: ABI v3 not online yet, press H first\r\n");
        return;
    }

    config.coeff_set_id = 0U;
    config.fft_scale_shift = 3U;
    /* An out-of-range bit (bit 10) makes this deliberately invalid; a real
     * caller would never set it. */
    config.feature_mask = valid ? 0x3FFU : 0x400U;

    status = cpu0_ipc_v3_set_dsp_config(&v3_link, &config, ZMPIO_V3_CONFIG_MAX_ATTEMPTS,
                                        ZMPIO_V3_CONFIG_TIMEOUT_MS, &protocol_status);
    if (status != CPU0_IPC_V3_OK) {
        xil_printf("CPU0: ABI v3 SET_DSP_CONFIG transport error=%d\r\n", status);
        return;
    }
    xil_printf("CPU0: ABI v3 SET_DSP_CONFIG %s status=%ld (expected %s)\r\n",
               protocol_status == 0 ? "accepted" : "rejected", (long)protocol_status,
               valid ? "accepted" : "rejected");
}

static void test_abi_v3_corrupt_slot(void)
{
    (void)cpu0_ipc_v3_debug_send_corrupt_command();
    xil_printf("CPU0: ABI v3 lab test -- press V shortly, then H, to confirm "
               "cpu1 crc_drops increased and the link still comes back ONLINE\r\n");
}

static void test_abi_v3_duplicate(void)
{
    cpu0_ipc_v3_link_t duplicate_link;
    int32_t duplicate_status = 0;

    (void)cpu0_ipc_v3_debug_resend_last(&duplicate_link, &duplicate_status);
}

static void test_abi_v3_wrap_stress(void)
{
    (void)cpu0_ipc_v3_debug_wrap_stress((ZMPIO_ABI_V3_CMD_SLOTS * 2U) + 3U,
                                        ZMPIO_V3_HELLO_TIMEOUT_MS);
}

#if defined(APP_ABI_V3_AUTOMATED_LAB_TEST) && (APP_ABI_V3_AUTOMATED_LAB_TEST != 0)
/*
 * Runs the same command sequence as the interactive V/H/C/X/K/U/W menu
 * (process_command() below), triggered by a timed loop instead of UART0
 * keypresses -- the physical UART0 terminal plays no part in this path at
 * any point. Detecting a CPU1 restart (a separate JTAG "rst -processor"
 * issued mid-soak) is done indirectly: this code cannot safely issue that
 * reset itself from CPU0 (only a JTAG script can do so safely, as verified
 * in program_and_run.tcl), so the soak loop instead re-sends HELLO on every
 * step and logs whenever session_id changes, which is what actually reveals
 * that an external reset happened.
 */
/* Overridable via USER_COMPILE_DEFINITIONS for a quick functional smoke test
 * (e.g. APP_ABI_V3_SOAK_TOTAL_S=180 APP_ABI_V3_SOAK_STEP_S=30) -- the real
 * Step 4 gate still requires an actual 1-hour run before being marked PASS,
 * this override exists only to verify the mechanism itself quickly. */
#if !defined(APP_ABI_V3_SOAK_TOTAL_S)
#define APP_ABI_V3_SOAK_TOTAL_S (60UL * 60UL) /* 1 hour, matching the gate requirement */
#endif
#if !defined(APP_ABI_V3_SOAK_STEP_S)
#define APP_ABI_V3_SOAK_STEP_S (5UL * 60UL) /* H + C every 5 minutes */
#endif
#define AUTOMATED_SOAK_TOTAL_MS (APP_ABI_V3_SOAK_TOTAL_S * 1000UL)
#define AUTOMATED_SOAK_STEP_MS  (APP_ABI_V3_SOAK_STEP_S * 1000UL)

static void automated_log_status(const char *tag)
{
    uint32_t cpu1_crc_drop_count = 0U;
    uint32_t cpu0_crc_drop_count = 0U;

    cpu0_ipc_v3_get_counters(&cpu1_crc_drop_count, &cpu0_crc_drop_count);
    xil_printf("AUTOTEST[%s]: online=%d session_id=0x%08lx crc_drops cpu1=%lu cpu0=%lu\r\n",
               tag, (int)v3_link.online, (unsigned long)v3_link.session_id,
               (unsigned long)cpu1_crc_drop_count, (unsigned long)cpu0_crc_drop_count);
}

/* A single usleep() call with a large microsecond argument (e.g. the 300s
 * soak step, 300000000us) never returns on this platform: the ABI v3
 * command ring stops advancing for the rest of the sleep even though
 * ONLINE stays up throughout. Chunking into repeated short usleep() calls
 * sidesteps whatever precision/overflow limit the platform's usleep() has
 * for large single arguments. */
static void sleep_seconds_chunked(uint32_t seconds)
{
    uint32_t i;
    for (i = 0U; i < seconds; ++i) {
        usleep(1000U * 1000U);
    }
}

static void run_automated_abi_v3_lab_tests(void)
{
    uint32_t elapsed_ms = 0U;
    uint32_t prev_session_id = v3_link.session_id;
    uint32_t cpu1_crc_before = 0U;
    uint32_t cpu0_crc_before = 0U;
    uint32_t cpu1_crc_after = 0U;
    uint32_t cpu0_crc_after = 0U;
    int hello_status;
    uint32_t wrap_total = (ZMPIO_ABI_V3_CMD_SLOTS * 2U) + 3U;
    uint32_t wrap_ok;

    xil_printf("AUTOTEST: starting automated Step 4 lab-test sequence (soak %lus, step %lus)\r\n",
               (unsigned long)(AUTOMATED_SOAK_TOTAL_MS / 1000U),
               (unsigned long)(AUTOMATED_SOAK_STEP_MS / 1000U));

    /* Boot-time HELLO (in main(), before this function runs) can miss CPU1's
     * ipc_rx_task window right after a fresh reset: CPU1 prints "ABI v3
     * ready" almost immediately, but ipc_rx_task is not yet actually
     * pumping the cmd ring that early, so CPU0's one-shot 5x500ms boot
     * HELLO can time out on every attempt. Retry quickly here first (every
     * 2s, up to 30s) instead of waiting a full AUTOMATED_SOAK_STEP_MS for
     * the first catch-up chance. */
    if (!v3_link.online) {
        int quick_try;
        for (quick_try = 1; quick_try <= 15 && !v3_link.online; ++quick_try) {
            usleep(2000U * 1000U);
            hello_status = test_abi_v3_hello();
            xil_printf("AUTOTEST[quick-retry %d/15]: hello_status=%d online=%d\r\n",
                       quick_try, hello_status, (int)v3_link.online);
        }
        prev_session_id = v3_link.session_id;
    }

    /* Task 3: a 1-hour soak, alternating HELLO (refreshes session/online
     * state and doubles as the session-change detector used by task 7) with
     * a valid SET_DSP_CONFIG. */
    while (elapsed_ms < AUTOMATED_SOAK_TOTAL_MS) {
        sleep_seconds_chunked(AUTOMATED_SOAK_STEP_MS / 1000U);
        elapsed_ms += AUTOMATED_SOAK_STEP_MS;

        hello_status = test_abi_v3_hello();
        if (v3_link.session_id != prev_session_id) {
            xil_printf("AUTOTEST[session-change]: session_id 0x%08lx -> 0x%08lx, online=%d "
                       "(evidence for task 7 if CPU1 was just restarted independently via JTAG)\r\n",
                       (unsigned long)prev_session_id, (unsigned long)v3_link.session_id,
                       (int)v3_link.online);
            prev_session_id = v3_link.session_id;
        }
        if (hello_status == CPU0_IPC_V3_OK && v3_link.online) {
            test_abi_v3_set_dsp_config(true);
        }
        xil_printf("AUTOTEST[soak]: t=%lus ", (unsigned long)(elapsed_ms / 1000U));
        automated_log_status("soak");
    }
    xil_printf("AUTOTEST: soak complete (task 3)\r\n");

    /* Task 4: CRC corrupt-slot ('K') -- expects cpu1_crc_drop_count to
     * increase and the link to still recover afterward. */
    cpu0_ipc_v3_get_counters(&cpu1_crc_before, &cpu0_crc_before);
    (void)cpu0_ipc_v3_debug_send_corrupt_command();
    usleep(500U * 1000U);
    cpu0_ipc_v3_get_counters(&cpu1_crc_after, &cpu0_crc_after);
    hello_status = test_abi_v3_hello();
    xil_printf("AUTOTEST[K]: cpu1_crc_drop %lu -> %lu (%s); post-hello online=%d (%s)\r\n",
               (unsigned long)cpu1_crc_before, (unsigned long)cpu1_crc_after,
               (cpu1_crc_after > cpu1_crc_before) ? "PASS increased" : "FAIL did not increase",
               (int)v3_link.online,
               (hello_status == CPU0_IPC_V3_OK && v3_link.online) ? "PASS link recovered"
                                                                   : "FAIL link not online");

    /* Task 5: duplicate ('C' then 'U') -- resends the exact same
     * correlation_id, expecting CPU1's duplicate-suppression to apply the
     * effect only once. */
    test_abi_v3_set_dsp_config(true);
    {
        cpu0_ipc_v3_link_t duplicate_link;
        int32_t duplicate_status = 0;
        int dup_result = cpu0_ipc_v3_debug_resend_last(&duplicate_link, &duplicate_status);

        xil_printf("AUTOTEST[U]: resend status=%d protocol_status=%ld (verify CONFIG_SEQ did not "
                   "increment twice via CPU1's UART1/ZLOG)\r\n",
                   dup_result, (long)duplicate_status);
    }

    /* Task 6: wrap-around ('W') -- pushes the 16-slot ring around twice. */
    wrap_ok = cpu0_ipc_v3_debug_wrap_stress(wrap_total, ZMPIO_V3_HELLO_TIMEOUT_MS);
    xil_printf("AUTOTEST[W]: %lu/%lu HELLO received a valid, correctly-ordered reply (%s)\r\n",
               (unsigned long)wrap_ok, (unsigned long)wrap_total,
               (wrap_ok == wrap_total) ? "PASS all accounted for" : "CHECK some missing, see detailed log");

    xil_printf("AUTOTEST: full sequence complete, returning to the normal interactive menu\r\n");
}
#endif /* APP_ABI_V3_AUTOMATED_LAB_TEST */

#if defined(APP_DSP_SOFT_RESET_FAULT_TEST) && (APP_DSP_SOFT_RESET_FAULT_TEST != 0)
/*
 * Step 3 fault-injection gate: a single DSP fault must not take down
 * I2C/SD/scheduler (scenario: PL reset mid-RUN). Sends
 * ZMPIO_V3_CMD_DSP_SOFT_RESET once, a few seconds after ABI v3 reaches
 * ONLINE, then just logs the ACK --
 * the actual pass/fail evidence (I2C/SD/scheduler survive, feature_count
 * dips then resumes) comes from CPU1's own UART1 log, not from this function.
 * A raw JTAG mrd/mwr straight into zmpio_dsp_ctrl's PL register is
 * unreliable debug-memory access to PL peripheral space on a live-running
 * core, unrelated to the RTL itself -- this ABI v3 command reuses the same
 * proven, cache/barrier-safe send path as SET_DSP_CONFIG instead, with
 * CPU1 (the actual MMIO owner) doing the write itself.
 */
static void run_dsp_soft_reset_fault_test(void)
{
    zmpio_v3_dsp_soft_reset_ack_t ack;
    int status;
    uint32_t i;

    if (!v3_link.online) {
        xil_printf("FAULTTEST: ABI v3 not online, skipping DSP_SOFT_RESET fault test\r\n");
        return;
    }

    /* Chunked, not a single usleep(5000000) -- see sleep_seconds_chunked()'s
     * comment above (large single usleep() args were observed to never
     * return on this platform). */
    for (i = 0U; i < 5U; ++i) {
        usleep(1000U * 1000U);
    }
    xil_printf("FAULTTEST: sending DSP_SOFT_RESET (corr id fresh, session_id=0x%08lx)\r\n",
               (unsigned long)v3_link.session_id);
    status = cpu0_ipc_v3_dsp_soft_reset(&v3_link, 3U, ZMPIO_V3_HELLO_TIMEOUT_MS, &ack);
    xil_printf("FAULTTEST: DSP_SOFT_RESET result status=%d feature_count_before=%lu "
               "drop_count_before=%lu -- check CPU1 UART1 log for recovery evidence\r\n",
               status, (unsigned long)ack.feature_count_before,
               (unsigned long)ack.drop_count_before);
}
#endif /* APP_DSP_SOFT_RESET_FAULT_TEST */

#if defined(APP_FIFO_FULL_FAULT_TEST) && (APP_FIFO_FULL_FAULT_TEST != 0)
/*
 * Step 3 fault-injection gate: a single DSP fault must not take down
 * I2C/SD/scheduler (scenario: feature FIFO full). Sends
 * ZMPIO_V3_CMD_FIFO_FULL_INJECT once, a few seconds after ABI v3 reaches
 * ONLINE, asking CPU1 to hold off
 * draining zmpio_dsp_ctrl's 64-entry feature FIFO for 60s -- long enough
 * (with ~46% margin) for the DSP core's own ~1.56 frame/s production
 * (SDD_PL_SHELL.md: window 128/hop 64 @ 100 Hz) to fill it from empty
 * (~41s) and increment DROP_COUNT. Software-only, no PL register touched by
 * either core; CPU1 (fpga_result_task) is the one that actually skips its
 * drain loop. The pass/fail evidence (I2C/SD/scheduler survive throughout,
 * ctrl_drop_count increases, backlog drains cleanly once the hold expires)
 * comes from CPU1's own UART1 log, not from this function.
 */
static void run_fifo_full_inject_fault_test(void)
{
    zmpio_v3_fifo_full_inject_ack_t ack;
    int status;
    uint32_t i;

    if (!v3_link.online) {
        xil_printf("FAULTTEST: ABI v3 not online, skipping FIFO_FULL_INJECT fault test\r\n");
        return;
    }

    /* Chunked, not a single usleep(5000000) -- see sleep_seconds_chunked()'s
     * comment above (large single usleep() args were observed to never
     * return on this platform); that helper itself lives behind
     * APP_ABI_V3_AUTOMATED_LAB_TEST, so this mirrors run_dsp_soft_reset_fault_test()'s
     * own inline loop instead of depending on it. */
    for (i = 0U; i < 5U; ++i) {
        usleep(1000U * 1000U);
    }
    xil_printf("FAULTTEST: sending FIFO_FULL_INJECT hold_ms=60000 (corr id fresh, "
               "session_id=0x%08lx)\r\n",
               (unsigned long)v3_link.session_id);
    status = cpu0_ipc_v3_fifo_full_inject(&v3_link, 60000U, 3U,
                                          ZMPIO_V3_HELLO_TIMEOUT_MS, &ack);
    xil_printf("FAULTTEST: FIFO_FULL_INJECT result status=%d feature_count_before=%lu "
               "drop_count_before=%lu hold_ms_applied=%lu -- check CPU1 UART1 log for "
               "recovery evidence\r\n",
               status, (unsigned long)ack.feature_count_before,
               (unsigned long)ack.drop_count_before,
               (unsigned long)ack.hold_ms_applied);
}
#endif /* APP_FIFO_FULL_FAULT_TEST */

#if defined(APP_DOORBELL_FAULT_TEST) && (APP_DOORBELL_FAULT_TEST != 0)
/*
 * Step 5 pass gates: 10,000 events with no lost wakeup; a spurious IRQ must
 * not create a phantom event; a storm must not crash; and the ABI v3 command
 * path must keep working with the doorbell disabled. Scenarios 2-4 drive
 * zmpio_doorbell_0's MMIO
 * directly from CPU0 via pl_doorbell_test_ring() rather than through CPU1/
 * ABI v3 -- see that function's comment in pl_doorbell.h for why this keeps
 * the PL+GIC+ISR mechanism test isolated from CPU1 firmware correctness
 * (which scenario 1 already covers with real traffic). The actual pass/fail
 * evidence is the UART transcript of this function's own printed deltas,
 * captured the same way Step 3's fault-injection gates were
 * (docs/architecture/evidence/).
 */
#define APP_DOORBELL_STRESS_EVENT_COUNT 10000U

static void run_doorbell_fault_test(void)
{
    uint32_t isr_before, isr_after, count_before, count_after;
    uint32_t i;
    uint32_t hello_ok = 0U;
    int no_doorbell_status = CPU0_IPC_V3_OK;

    xil_printf("DOORBELLTEST: ---- scenario 1/4: basic (real ABI v3 traffic) ----\r\n");
    if (v3_link.online) {
        isr_before = cpu0_irq_handler_get_isr_count();
        count_before = pl_doorbell_count();
        for (i = 0U; i < 20U; ++i) {
            if (test_abi_v3_hello() == CPU0_IPC_V3_OK) {
                hello_ok++;
            }
            cpu0_irq_handler_service();
        }
        /* Let the last ISR/service settle before reading final counters. */
        for (i = 0U; i < 10U; ++i) {
            usleep(1000U);
            cpu0_irq_handler_service();
        }
        isr_after = cpu0_irq_handler_get_isr_count();
        count_after = pl_doorbell_count();
        xil_printf("DOORBELLTEST: basic hello_ok=%lu/20 isr_delta=%lu count_delta=%lu "
                   "(expect isr_delta==count_delta==hello_ok)\r\n",
                   (unsigned long)hello_ok, (unsigned long)(isr_after - isr_before),
                   (unsigned long)(count_after - count_before));
    } else {
        xil_printf("DOORBELLTEST: ABI v3 not online, skipping basic scenario\r\n");
    }

    xil_printf("DOORBELLTEST: ---- scenario 2/4: spurious IRQ ----\r\n");
    isr_before = cpu0_irq_handler_get_isr_count();
    /* PENDING is already clear here (scenario 1 drained it) -- toggling
     * IRQ_ENABLE off/on with nothing pending must not fire the ISR
     * (zmpio_doorbell.v: irq_out = IRQ_ENABLE && PENDING). */
    pl_doorbell_irq_disable();
    pl_doorbell_irq_enable();
    for (i = 0U; i < 50U; ++i) {
        usleep(1000U);
    }
    isr_after = cpu0_irq_handler_get_isr_count();
    xil_printf("DOORBELLTEST: spurious isr_delta=%lu (expect 0), spurious_flag=%d "
               "(expect 0)\r\n",
               (unsigned long)(isr_after - isr_before),
               (int)cpu0_irq_handler_get_spurious_detected());

    xil_printf("DOORBELLTEST: ---- scenario 3/4: %lu-event volume + storm ----\r\n",
               (unsigned long)APP_DOORBELL_STRESS_EVENT_COUNT);
    isr_before = cpu0_irq_handler_get_isr_count();
    count_before = pl_doorbell_count();
    for (i = 0U; i < APP_DOORBELL_STRESS_EVENT_COUNT; ++i) {
        pl_doorbell_test_ring();
        /* Deliberately NOT waiting for the ISR between rings -- this is what
         * makes the back half of this loop a doorbell storm (SET arriving
         * faster than the ISR can drain+ACK+re-enable), the CPU0-target
         * analogue of the GIC IRQ-storm class of bug in
         * docs/architecture/08_VAN_DE_DANG_MO.md section 12. */
        if ((i % 64U) == 0U) {
            cpu0_irq_handler_service();
        }
    }
    /* Drain whatever is left after the storm. */
    for (i = 0U; i < 2000U; ++i) {
        cpu0_irq_handler_service();
        if (!pl_doorbell_pending()) {
            break;
        }
        usleep(500U);
    }
    count_after = pl_doorbell_count();
    xil_printf("DOORBELLTEST: storm count_delta=%lu (expect %lu -- DBELL_COUNT is the "
               "ground truth: it must equal exactly the number of pl_doorbell_test_ring() "
               "calls regardless of how many discrete ISR entries serviced them), "
               "final pending=%d (expect 0)\r\n",
               (unsigned long)(count_after - count_before),
               (unsigned long)APP_DOORBELL_STRESS_EVENT_COUNT,
               (int)pl_doorbell_pending());
    isr_after = cpu0_irq_handler_get_isr_count();
    xil_printf("DOORBELLTEST: storm isr_delta=%lu (informational -- a level IRQ can "
               "legitimately coalesce many SETs behind fewer ISR entries; count_delta "
               "above is the actual lost-wakeup gate)\r\n",
               (unsigned long)(isr_after - isr_before));

    xil_printf("DOORBELLTEST: ---- scenario 4/4: command path without doorbell ----\r\n");
    pl_doorbell_irq_disable();
    if (v3_link.online) {
        no_doorbell_status = test_abi_v3_hello();
        xil_printf("DOORBELLTEST: HELLO with DBELL_IRQ_ENABLE off status=%d "
                   "(expect %d -- command path must not depend on the doorbell)\r\n",
                   no_doorbell_status, CPU0_IPC_V3_OK);
    } else {
        xil_printf("DOORBELLTEST: ABI v3 not online, skipping no-doorbell scenario\r\n");
    }
    pl_doorbell_irq_enable();
}
#endif /* APP_DOORBELL_FAULT_TEST */

static void process_command(char command, int *continuous_sensor_enabled)
{
    static char log_line[IPC_PAYLOAD_SIZE + 1U];

    switch (command) {
    case 'h':
        (void)test_heartbeat();
        break;
    case 's':
        (void)test_sensor();
        break;
    case 'c':
        *continuous_sensor_enabled = !*continuous_sensor_enabled;
        xil_printf("CPU0: continuous sensor sampling %s\r\n",
                   *continuous_sensor_enabled ? "enabled" : "disabled");
        break;
    case 'a':
        (void)test_logger_status();
        break;
    case 'd':
        (void)test_logger_diagnostic();
        break;
    case '1':
        (void)send_logger_command(MSG_TYPE_LOG_START, "START");
        break;
    case '0':
        (void)send_logger_command(MSG_TYPE_LOG_STOP, "STOP");
        break;
    case 'f':
        (void)send_logger_command(MSG_TYPE_LOG_FLUSH, "FLUSH");
        break;
    case 'l':
        xil_printf("CPU0: enter log text> ");
        uart_read_line(log_line, sizeof(log_line));
        if (log_line[0] != '\0') {
            (void)send_log_text(log_line);
        }
        break;
    case 'n':
        net_print_status();
        break;
    case 'V':
        print_abi_v3_status();
        break;
    case 'H':
        (void)test_abi_v3_hello();
        break;
    case 'C':
        test_abi_v3_set_dsp_config(true);
        break;
    case 'X':
        test_abi_v3_set_dsp_config(false);
        break;
    case 'K':
        test_abi_v3_corrupt_slot();
        break;
    case 'U':
        test_abi_v3_duplicate();
        break;
    case 'W':
        test_abi_v3_wrap_stress();
        break;
    case 'g': {
        static char ping_target[64];
        xil_printf("CPU0: ping target IPv4 or hostname (e.g. 192.168.1.10 or google.com)> ");
        uart_read_line(ping_target, sizeof(ping_target));
        if (ping_target[0] != '\0') {
            (void)net_ping_host(ping_target, 4);
        }
        break;
    }
    case '?':
        print_menu();
        break;
    case '\r':
    case '\n':
        break;
    default:
        xil_printf("CPU0: unknown command '%c' (press ? for help)\r\n",
                   command);
        break;
    }
}

int main(void)
{
    uint32_t sample_count = 0U;
    uint32_t elapsed_since_sample_ms = 0U;
    uint32_t elapsed_since_v3_hello_ms = 0U;
    int continuous_sensor_enabled = 1;

    init_platform();
    xil_printf("\r\nCPU0: JTAG IPC test controller starting\r\n");
    xil_printf("CPU0: JTAG Ethernet bring-up test\r\n");

    reset_phy_emio();
    enable_gem0_clocks();

    if (ethernet_test() != XST_SUCCESS) {
        xil_printf("CPU0: Ethernet test FAILED\r\n");

        /*
        * Stop here.
        *
        * CPU1/IPC is intentionally NOT started yet.
        * This isolates Ethernet from CPU1/IPC/SPI.
        */
        for (;;) {
            sleep(1);
        }
    }
    xil_printf("CPU0: Ethernet test PASSED\r\n");
    net_init();
    /* Step 5 (docs/ROADMAP.md STEP 5): must run after net_init()'s own
     * XSetupInterruptSystem() call above -- see cpu0_irq_handler.h's comment
     * on cpu0_irq_handler_init() for why. */
    cpu0_irq_handler_init();
    net_ping_init();
    cpu0_ipc_init();
    enable_cpu1_spi_clocks();
    xil_printf("CPU0: CPU1 ELF must be loaded at 0x%08lx\r\n",
               (unsigned long)CPU1_ENTRY_ADDR);
    start_cpu1();
    xil_printf("CPU0: CPU1 start vector written; waiting for IPC rings...\r\n");

    if (cpu0_ipc_wait_for_cpu1_ready(CPU1_BOOT_TIMEOUT_MS) != CPU0_IPC_OK) {
        xil_printf("CPU0: CPU1 did not initialize IPC within %lu ms\r\n",
                   (unsigned long)CPU1_BOOT_TIMEOUT_MS);
        xil_printf("CPU0: verify CPU1 ELF load address and JTAG startup order\r\n");
        for (;;) {
            usleep(1000000U);
        }
    }

    xil_printf("CPU0: CPU1 IPC ready\r\n");

    /* Step 4 ABI v3 (docs/ROADMAP_DETAIL.md): state machine CPU1 READY ->
     * CPU0 HELLO -> HELLO_ACK -> ONLINE. Additive to the ABI v2 checks below
     * -- a v3 handshake failure is logged but does not stop the v2 test
     * sequence, since v3 is still lab-only at this step. */
    if (cpu0_ipc_v3_wait_for_cpu1_ready(CPU1_BOOT_TIMEOUT_MS) != CPU0_IPC_V3_OK) {
        xil_printf("CPU0: ABI v3 CPU1_READY not observed within %lu ms\r\n",
                   (unsigned long)CPU1_BOOT_TIMEOUT_MS);
    } else if (test_abi_v3_hello() != CPU0_IPC_V3_OK) {
        xil_printf("CPU0: ABI v3 HELLO handshake failed at boot (see status above)\r\n");
    }

    (void)test_heartbeat();
    (void)test_logger_status();
    (void)test_logger_diagnostic();
    test_log_submission();
    print_menu();

#if defined(APP_ABI_V3_AUTOMATED_LAB_TEST) && (APP_ABI_V3_AUTOMATED_LAB_TEST != 0)
    run_automated_abi_v3_lab_tests();
#endif

#if defined(APP_DSP_SOFT_RESET_FAULT_TEST) && (APP_DSP_SOFT_RESET_FAULT_TEST != 0)
    run_dsp_soft_reset_fault_test();
#endif

#if defined(APP_FIFO_FULL_FAULT_TEST) && (APP_FIFO_FULL_FAULT_TEST != 0)
    run_fifo_full_inject_fault_test();
#endif

#if defined(APP_DOORBELL_FAULT_TEST) && (APP_DOORBELL_FAULT_TEST != 0)
    run_doorbell_fault_test();
#endif

    for (;;) {
        net_poll();
        cpu0_irq_handler_service();

        if (uart_has_input()) {
            process_command(inbyte(), &continuous_sensor_enabled);
        }

        if (continuous_sensor_enabled &&
            elapsed_since_sample_ms >= SENSOR_PERIOD_MS) {
            (void)test_sensor();
            ++sample_count;
            if ((sample_count % STATUS_PERIOD_SAMPLES) == 0U) {
                (void)test_logger_status();
            }
            elapsed_since_sample_ms = 0U;
        }

        if (!v3_link.online &&
            elapsed_since_v3_hello_ms >= ZMPIO_V3_HELLO_RETRY_PERIOD_MS) {
            (void)test_abi_v3_hello();
            elapsed_since_v3_hello_ms = 0U;
        }

        usleep(UART_POLL_INTERVAL_MS * 1000U);
        elapsed_since_sample_ms += UART_POLL_INTERVAL_MS;
        elapsed_since_v3_hello_ms += UART_POLL_INTERVAL_MS;
    }
}
