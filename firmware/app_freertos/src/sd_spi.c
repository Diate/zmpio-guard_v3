#include "sd_spi.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "app_config.h"
#include "task.h"
#include "xspips_hw.h"
#include "xstatus.h"
#include "zmpio_protocol.h"

#define SD_CMD0    0U
#define SD_CMD8    8U
#define SD_CMD9    9U
#define SD_CMD16   16U
#define SD_CMD17   17U
#define SD_CMD24   24U
#define SD_CMD55   55U
#define SD_CMD58   58U
#define SD_ACMD41  41U

#define SD_R1_IDLE             0x01U
#define SD_R1_ILLEGAL_COMMAND  0x04U
#define SD_DATA_TOKEN          0xFEU
#define SD_DATA_ACCEPTED       0x05U

#define SD_TYPE_V2             0x01U
#define SD_TYPE_BLOCK_ADDRESS  0x02U

static bool deadline_expired(TickType_t start, uint32_t timeout_ms)
{
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    if (timeout == 0U) {
        timeout = 1U;
    }
    return (TickType_t)(xTaskGetTickCount() - start) >= timeout;
}

static void spi_capture_detail(sd_spi_t *card,
                               ipc_sd_spi_detail_reason_t reason,
                               uint8_t last_rx, uint32_t status)
{
    uint32_t base;
    uint32_t config;
    uint32_t enabled;
    uint32_t current_reason;

    if ((card == NULL) || (card->spi == NULL)) {
        return;
    }

    current_reason = IPC_SD_SPI_DETAIL_REASON(card->detail);
    if ((card->detail != 0U) &&
        (current_reason == IPC_SD_SPI_DETAIL_MODE_FAULT)) {
        return;
    }
    if ((card->detail != 0U) && (reason != IPC_SD_SPI_DETAIL_MODE_FAULT)) {
        return;
    }

    base = card->spi->Config.BaseAddress;
    config = XSpiPs_ReadReg(base, XSPIPS_CR_OFFSET);
    enabled = XSpiPs_ReadReg(base, XSPIPS_ER_OFFSET) &
              XSPIPS_ER_ENABLE_MASK;
    card->detail = IPC_SD_SPI_DETAIL_PACK(
        reason, last_rx, config, enabled != 0U,
        (status & XSPIPS_IXR_MODF_MASK) != 0U);
}

static int spi_prepare_controller(sd_spi_t *card)
{
    uint32_t base;
    uint32_t config;
    int status;

    XSpiPs_Reset(card->spi);
    status = XSpiPs_SetOptions(card->spi,
                              XSPIPS_MASTER_OPTION |
                              XSPIPS_FORCE_SSELECT_OPTION);
    if (status != XST_SUCCESS) {
        spi_capture_detail(card, IPC_SD_SPI_DETAIL_CONTROLLER_SETUP,
                           card->last_rx,
                           XSpiPs_ReadReg(card->spi->Config.BaseAddress,
                                         XSPIPS_SR_OFFSET));
        return status;
    }
    status = XSpiPs_SetSlaveSelect(card->spi, APP_SD_SPI_SLAVE_SELECT);
    if (status != XST_SUCCESS) {
        spi_capture_detail(card, IPC_SD_SPI_DETAIL_CONTROLLER_SETUP,
                           card->last_rx,
                           XSpiPs_ReadReg(card->spi->Config.BaseAddress,
                                         XSPIPS_SR_OFFSET));
        return status;
    }

    base = card->spi->Config.BaseAddress;
    config = XSpiPs_ReadReg(base, XSPIPS_CR_OFFSET);
    config &= ~XSPIPS_CR_MODF_GEN_EN_MASK;
    config &= ~XSPIPS_CR_SSCTRL_MASK;
    config |= XSPIPS_CR_SSCTRL_MASK;
    XSpiPs_WriteReg(base, XSPIPS_CR_OFFSET, config);
    XSpiPs_WriteReg(base, XSPIPS_SR_OFFSET, XSPIPS_IXR_WR_TO_CLR_MASK);
    XSpiPs_Enable(card->spi);
    return XST_SUCCESS;
}

static void spi_set_selected(sd_spi_t *card, bool selected)
{
    uint32_t config;

    XSpiPs_Disable(card->spi);
    config = XSpiPs_ReadReg(card->spi->Config.BaseAddress, XSPIPS_CR_OFFSET);
    config &= ~XSPIPS_CR_SSCTRL_MASK;
    config |= selected ? card->spi->SlaveSelect : XSPIPS_CR_SSCTRL_MASK;
    XSpiPs_WriteReg(card->spi->Config.BaseAddress, XSPIPS_CR_OFFSET, config);
    XSpiPs_Enable(card->spi);
}

static int spi_exchange_byte(sd_spi_t *card, uint8_t transmit,
                             uint8_t *receive)
{
    uint32_t guard = 1000000U;
    uint32_t status;

    XSpiPs_WriteReg(card->spi->Config.BaseAddress,
                    XSPIPS_TXD_OFFSET, transmit);
    do {
        status = XSpiPs_ReadReg(card->spi->Config.BaseAddress,
                               XSPIPS_SR_OFFSET);
        if ((status & XSPIPS_IXR_MODF_MASK) != 0U) {
            spi_capture_detail(card, IPC_SD_SPI_DETAIL_MODE_FAULT,
                               card->last_rx, status);
            XSpiPs_WriteReg(card->spi->Config.BaseAddress,
                            XSPIPS_SR_OFFSET, XSPIPS_IXR_MODF_MASK);
            return XST_SEND_ERROR;
        }
        if ((status & XSPIPS_IXR_RXNEMPTY_MASK) != 0U) {
            uint8_t value = (uint8_t)XSpiPs_ReadReg(
                card->spi->Config.BaseAddress, XSPIPS_RXD_OFFSET);
            if (receive != NULL) {
                *receive = value;
            }
            card->last_rx = value;
            return XST_SUCCESS;
        }
        --guard;
    } while (guard != 0U);

    spi_capture_detail(card, IPC_SD_SPI_DETAIL_RX_TIMEOUT,
                       card->last_rx, status);
    return XST_FAILURE;
}

static int spi_receive(sd_spi_t *card, uint8_t *buffer, uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        if (spi_exchange_byte(card, 0xFFU, &buffer[index]) != XST_SUCCESS) {
            return XST_FAILURE;
        }
    }
    return XST_SUCCESS;
}

static int spi_transmit(sd_spi_t *card, const uint8_t *buffer,
                        uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        if (spi_exchange_byte(card, buffer[index], NULL) != XST_SUCCESS) {
            return XST_FAILURE;
        }
    }
    return XST_SUCCESS;
}

static void sd_deselect(sd_spi_t *card)
{
    uint8_t ignored;

    spi_set_selected(card, false);
    (void)spi_exchange_byte(card, 0xFFU, &ignored);
}

static int sd_select(sd_spi_t *card)
{
    uint8_t ignored;

    spi_set_selected(card, true);
    return spi_exchange_byte(card, 0xFFU, &ignored);
}

/*
 * Poll for the card to release its busy signal.
 *
 * The fast path matters: before every command the card is normally ready on
 * the first byte, so the first APP_SD_WAIT_READY_FAST_POLLS iterations spin
 * without yielding. Beyond that the card is in a programming cycle -- 1-3 ms
 * typically, up to a quarter second on a slow card. Sensor and FPGA_Result
 * run at the same priority as storage_task (APP_STORAGE_TASK_PRIORITY), so a
 * writer that spun instead of blocking here would leave them dependent on
 * tick time-slicing alone: sensor_task could miss its 10 ms deadline, causing
 * vTaskDelayUntil() to stop blocking (its wake time already in the past) and
 * hammer the I2C bus back-to-back. Blocking here instead hands those ticks
 * back to the other tasks and keeps the sensor cadence intact while the SD
 * card is writing.
 */
static int sd_wait_ready(sd_spi_t *card, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    uint8_t value = 0U;
    uint32_t polls = 0U;

    do {
        if (spi_exchange_byte(card, 0xFFU, &value) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        if (value == 0xFFU) {
            return XST_SUCCESS;
        }
        if (polls < APP_SD_WAIT_READY_FAST_POLLS) {
            ++polls;
        } else {
            vTaskDelay(1U);
        }
    } while (!deadline_expired(start, timeout_ms));

    return XST_FAILURE;
}

/* Leaves CS asserted so the caller can read/write the command payload. */
static int sd_send_command(sd_spi_t *card, uint8_t command,
                           uint32_t argument, uint8_t *response)
{
    uint8_t packet[6];
    uint8_t value = 0xFFU;
    uint32_t attempt;

    sd_deselect(card);
    if ((sd_select(card) != XST_SUCCESS) ||
        (sd_wait_ready(card, APP_SD_COMMAND_TIMEOUT_MS) != XST_SUCCESS)) {
        sd_deselect(card);
        return XST_FAILURE;
    }

    packet[0] = (uint8_t)(0x40U | command);
    packet[1] = (uint8_t)(argument >> 24U);
    packet[2] = (uint8_t)(argument >> 16U);
    packet[3] = (uint8_t)(argument >> 8U);
    packet[4] = (uint8_t)argument;
    packet[5] = command == SD_CMD0 ? 0x95U :
                (command == SD_CMD8 ? 0x87U : 0x01U);

    if (spi_transmit(card, packet, sizeof(packet)) != XST_SUCCESS) {
        sd_deselect(card);
        return XST_FAILURE;
    }

    for (attempt = 0U; attempt < 10U; ++attempt) {
        if (spi_exchange_byte(card, 0xFFU, &value) != XST_SUCCESS) {
            sd_deselect(card);
            return XST_FAILURE;
        }
        if ((value & 0x80U) == 0U) {
            *response = value;
            return XST_SUCCESS;
        }
    }

    spi_capture_detail(card, IPC_SD_SPI_DETAIL_RESPONSE_TIMEOUT, value,
                       XSpiPs_ReadReg(card->spi->Config.BaseAddress,
                                     XSPIPS_SR_OFFSET));
    sd_deselect(card);
    return XST_FAILURE;
}

static int sd_send_application_command(sd_spi_t *card, uint8_t command,
                                       uint32_t argument, uint8_t *response)
{
    uint8_t prefix_response;

    if (sd_send_command(card, SD_CMD55, 0U, &prefix_response) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    sd_deselect(card);
    if (prefix_response > SD_R1_IDLE) {
        return XST_FAILURE;
    }
    return sd_send_command(card, command, argument, response);
}

static int sd_wait_data_token(sd_spi_t *card, uint8_t expected,
                              uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    uint8_t value;

    do {
        if (spi_exchange_byte(card, 0xFFU, &value) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        if (value == expected) {
            return XST_SUCCESS;
        }
        if ((value != 0xFFU) && (value != 0x00U)) {
            return XST_FAILURE;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while (!deadline_expired(start, timeout_ms));

    return XST_FAILURE;
}

static int sd_read_csd(sd_spi_t *card)
{
    uint8_t response = 0xFFU;
    uint8_t csd[16];
    uint8_t crc[2];

    if ((sd_send_command(card, SD_CMD9, 0U, &response) != XST_SUCCESS) ||
        (response != 0U) ||
        (sd_wait_data_token(card, SD_DATA_TOKEN,
                            APP_SD_DATA_TIMEOUT_MS) != XST_SUCCESS) ||
        (spi_receive(card, csd, sizeof(csd)) != XST_SUCCESS) ||
        (spi_receive(card, crc, sizeof(crc)) != XST_SUCCESS)) {
        sd_deselect(card);
        return XST_FAILURE;
    }
    sd_deselect(card);

    if ((csd[0] >> 6U) == 1U) {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16U) |
                          ((uint32_t)csd[8] << 8U) | csd[9];
        card->sector_count = (c_size + 1U) * 1024U;
    } else {
        uint32_t read_block_length = csd[5] & 0x0FU;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03U) << 10U) |
                          ((uint32_t)csd[7] << 2U) |
                          ((uint32_t)(csd[8] & 0xC0U) >> 6U);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03U) << 1U) |
                               ((uint32_t)(csd[10] & 0x80U) >> 7U);
        uint64_t capacity = ((uint64_t)c_size + 1ULL) <<
                            (c_size_mult + 2U + read_block_length);
        card->sector_count = (uint32_t)(capacity / SD_SPI_SECTOR_SIZE);
    }

    return card->sector_count != 0U ? XST_SUCCESS : XST_FAILURE;
}

void sd_spi_bind(sd_spi_t *card, XSpiPs *spi)
{
    if (card == NULL) {
        return;
    }
    card->spi = spi;
    card->sector_count = 0U;
    card->detail = 0U;
    card->card_type = 0U;
    card->last_rx = 0xFFU;
    card->diagnostic = SD_SPI_DIAG_NONE;
    card->initialized = false;
}

int sd_spi_initialize(sd_spi_t *card)
{
    uint8_t response = 0xFFU;
    uint8_t register_value[4];
    uint8_t ignored;
    uint32_t index;
    TickType_t start;
    bool version2 = false;

    if ((card == NULL) || (card->spi == NULL)) {
        if (card != NULL) {
            card->diagnostic = SD_SPI_DIAG_INVALID_ARGUMENT;
        }
        return XST_INVALID_PARAM;
    }

    card->initialized = false;
    card->sector_count = 0U;
    card->detail = 0U;
    card->card_type = 0U;
    card->last_rx = 0xFFU;
    card->diagnostic = SD_SPI_DIAG_NONE;
    if (spi_prepare_controller(card) != XST_SUCCESS) {
        card->diagnostic = SD_SPI_DIAG_SET_INIT_CLOCK;
        return XST_FAILURE;
    }
    XSpiPs_Disable(card->spi);
    if (XSpiPs_SetClkPrescaler(card->spi,
                               APP_SD_SPI_INIT_PRESCALER) != XST_SUCCESS) {
        card->diagnostic = SD_SPI_DIAG_SET_INIT_CLOCK;
        return XST_FAILURE;
    }

    spi_set_selected(card, false);
    for (index = 0U; index < 10U; ++index) {
        if (spi_exchange_byte(card, 0xFFU, &ignored) != XST_SUCCESS) {
            card->diagnostic = SD_SPI_DIAG_IDLE_CLOCKS;
            return XST_FAILURE;
        }
    }

    start = xTaskGetTickCount();
    do {
        if ((sd_send_command(card, SD_CMD0, 0U, &response) == XST_SUCCESS) &&
            (response == SD_R1_IDLE)) {
            break;
        }
        sd_deselect(card);
        vTaskDelay(pdMS_TO_TICKS(10U));
    } while (!deadline_expired(start, APP_SD_COMMAND_TIMEOUT_MS));
    if (response != SD_R1_IDLE) {
        spi_capture_detail(card, IPC_SD_SPI_DETAIL_BAD_RESPONSE, response,
                           XSpiPs_ReadReg(card->spi->Config.BaseAddress,
                                         XSPIPS_SR_OFFSET));
        sd_deselect(card);
        card->diagnostic = SD_SPI_DIAG_CMD0;
        return XST_FAILURE;
    }
    sd_deselect(card);

    if (sd_send_command(card, SD_CMD8, 0x1AAU, &response) != XST_SUCCESS) {
        card->diagnostic = SD_SPI_DIAG_CMD8;
        return XST_FAILURE;
    }
    if (response == SD_R1_IDLE) {
        if (spi_receive(card, register_value, sizeof(register_value)) != XST_SUCCESS) {
            sd_deselect(card);
            card->diagnostic = SD_SPI_DIAG_CMD8;
            return XST_FAILURE;
        }
        version2 = (register_value[2] == 0x01U) &&
                   (register_value[3] == 0xAAU);
    } else if ((response & SD_R1_ILLEGAL_COMMAND) == 0U) {
        sd_deselect(card);
        card->diagnostic = SD_SPI_DIAG_CMD8;
        return XST_FAILURE;
    }
    sd_deselect(card);

    start = xTaskGetTickCount();
    do {
        uint32_t argument = version2 ? 0x40000000U : 0U;
        if ((sd_send_application_command(card, SD_ACMD41, argument,
                                         &response) == XST_SUCCESS) &&
            (response == 0U)) {
            break;
        }
        sd_deselect(card);
        vTaskDelay(pdMS_TO_TICKS(10U));
    } while (!deadline_expired(start, APP_SD_COMMAND_TIMEOUT_MS));
    if (response != 0U) {
        sd_deselect(card);
        card->diagnostic = SD_SPI_DIAG_ACMD41;
        return XST_FAILURE;
    }
    sd_deselect(card);

    if ((sd_send_command(card, SD_CMD58, 0U, &response) != XST_SUCCESS) ||
        (response != 0U) ||
        (spi_receive(card, register_value, sizeof(register_value)) != XST_SUCCESS)) {
        sd_deselect(card);
        card->diagnostic = SD_SPI_DIAG_CMD58;
        return XST_FAILURE;
    }
    if (version2) {
        card->card_type |= SD_TYPE_V2;
    }
    if ((register_value[0] & 0x40U) != 0U) {
        card->card_type |= SD_TYPE_BLOCK_ADDRESS;
    }
    sd_deselect(card);

    if ((card->card_type & SD_TYPE_BLOCK_ADDRESS) == 0U) {
        if ((sd_send_command(card, SD_CMD16, SD_SPI_SECTOR_SIZE,
                             &response) != XST_SUCCESS) ||
            (response != 0U)) {
            sd_deselect(card);
            card->diagnostic = SD_SPI_DIAG_CMD16;
            return XST_FAILURE;
        }
        sd_deselect(card);
    }

    XSpiPs_Disable(card->spi);
    if (XSpiPs_SetClkPrescaler(card->spi,
                               APP_SD_SPI_DATA_PRESCALER) != XST_SUCCESS) {
        card->diagnostic = SD_SPI_DIAG_SET_DATA_CLOCK;
        return XST_FAILURE;
    }

    card->initialized = true;
    if (sd_read_csd(card) != XST_SUCCESS) {
        card->initialized = false;
        card->diagnostic = SD_SPI_DIAG_CMD9;
        return XST_FAILURE;
    }
    card->diagnostic = SD_SPI_DIAG_NONE;
    return XST_SUCCESS;
}

int sd_spi_read(sd_spi_t *card, uint32_t sector, uint8_t *buffer,
                uint32_t count)
{
    uint32_t block;

    if ((card == NULL) || !card->initialized || (buffer == NULL) ||
        (count == 0U) || (count > card->sector_count) ||
        (sector > card->sector_count - count)) {
        return XST_INVALID_PARAM;
    }

    for (block = 0U; block < count; ++block) {
        uint8_t response;
        uint8_t crc[2];
        uint32_t address = ((card->card_type & SD_TYPE_BLOCK_ADDRESS) != 0U) ?
                           (sector + block) :
                           ((sector + block) * SD_SPI_SECTOR_SIZE);

        if ((sd_send_command(card, SD_CMD17, address, &response) != XST_SUCCESS) ||
            (response != 0U) ||
            (sd_wait_data_token(card, SD_DATA_TOKEN,
                                APP_SD_DATA_TIMEOUT_MS) != XST_SUCCESS) ||
            (spi_receive(card, buffer + block * SD_SPI_SECTOR_SIZE,
                         SD_SPI_SECTOR_SIZE) != XST_SUCCESS) ||
            (spi_receive(card, crc, sizeof(crc)) != XST_SUCCESS)) {
            sd_deselect(card);
            card->diagnostic = SD_SPI_DIAG_READ;
            card->initialized = false;
            return XST_FAILURE;
        }
        sd_deselect(card);
    }
    return XST_SUCCESS;
}

int sd_spi_write(sd_spi_t *card, uint32_t sector, const uint8_t *buffer,
                 uint32_t count)
{
    uint32_t block;

    if ((card == NULL) || !card->initialized || (buffer == NULL) ||
        (count == 0U) || (count > card->sector_count) ||
        (sector > card->sector_count - count)) {
        return XST_INVALID_PARAM;
    }

    for (block = 0U; block < count; ++block) {
        uint8_t response;
        uint8_t token;
        uint32_t address = ((card->card_type & SD_TYPE_BLOCK_ADDRESS) != 0U) ?
                           (sector + block) :
                           ((sector + block) * SD_SPI_SECTOR_SIZE);

        if ((sd_send_command(card, SD_CMD24, address, &response) != XST_SUCCESS) ||
            (response != 0U) ||
            (spi_exchange_byte(card, 0xFFU, NULL) != XST_SUCCESS) ||
            (spi_exchange_byte(card, SD_DATA_TOKEN, NULL) != XST_SUCCESS) ||
            (spi_transmit(card, buffer + block * SD_SPI_SECTOR_SIZE,
                          SD_SPI_SECTOR_SIZE) != XST_SUCCESS) ||
            (spi_exchange_byte(card, 0xFFU, NULL) != XST_SUCCESS) ||
            (spi_exchange_byte(card, 0xFFU, NULL) != XST_SUCCESS) ||
            (spi_exchange_byte(card, 0xFFU, &token) != XST_SUCCESS) ||
            ((token & 0x1FU) != SD_DATA_ACCEPTED) ||
            (sd_wait_ready(card, APP_SD_DATA_TIMEOUT_MS) != XST_SUCCESS)) {
            sd_deselect(card);
            card->diagnostic = SD_SPI_DIAG_WRITE;
            card->initialized = false;
            return XST_FAILURE;
        }
        sd_deselect(card);
    }
    return XST_SUCCESS;
}

int sd_spi_sync(sd_spi_t *card)
{
    int status;

    if ((card == NULL) || !card->initialized) {
        return XST_FAILURE;
    }
    if (sd_select(card) != XST_SUCCESS) {
        card->diagnostic = SD_SPI_DIAG_SYNC;
        card->initialized = false;
        return XST_FAILURE;
    }
    status = sd_wait_ready(card, APP_SD_DATA_TIMEOUT_MS);
    sd_deselect(card);
    if (status != XST_SUCCESS) {
        card->diagnostic = SD_SPI_DIAG_SYNC;
        card->initialized = false;
    }
    return status;
}

uint32_t sd_spi_sector_count(const sd_spi_t *card)
{
    return card != NULL ? card->sector_count : 0U;
}

bool sd_spi_is_initialized(const sd_spi_t *card)
{
    return (card != NULL) && card->initialized;
}

uint32_t sd_spi_detail(const sd_spi_t *card)
{
    return card != NULL ? card->detail : 0U;
}

const char *sd_spi_diagnostic_string(const sd_spi_t *card)
{
    static const char *const messages[] = {
        "none",
        "invalid-argument",
        "set-init-clock",
        "idle-clocks",
        "CMD0",
        "CMD8",
        "ACMD41",
        "CMD58",
        "CMD16",
        "set-data-clock",
        "CMD9/CSD",
        "read",
        "write",
        "sync"
    };

    if ((card == NULL) ||
        ((unsigned int)card->diagnostic >=
         (sizeof(messages) / sizeof(messages[0])))) {
        return "unknown";
    }
    return messages[card->diagnostic];
}
