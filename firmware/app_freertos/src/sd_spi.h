#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "xspips.h"

#define SD_SPI_SECTOR_SIZE 512U

typedef enum {
    SD_SPI_DIAG_NONE = 0,
    SD_SPI_DIAG_INVALID_ARGUMENT,
    SD_SPI_DIAG_SET_INIT_CLOCK,
    SD_SPI_DIAG_IDLE_CLOCKS,
    SD_SPI_DIAG_CMD0,
    SD_SPI_DIAG_CMD8,
    SD_SPI_DIAG_ACMD41,
    SD_SPI_DIAG_CMD58,
    SD_SPI_DIAG_CMD16,
    SD_SPI_DIAG_SET_DATA_CLOCK,
    SD_SPI_DIAG_CMD9,
    SD_SPI_DIAG_READ,
    SD_SPI_DIAG_WRITE,
    SD_SPI_DIAG_SYNC
} sd_spi_diagnostic_t;

typedef struct {
    XSpiPs *spi;
    uint32_t sector_count;
    uint32_t detail;
    uint8_t card_type;
    uint8_t last_rx;
    sd_spi_diagnostic_t diagnostic;
    bool initialized;
} sd_spi_t;

void sd_spi_bind(sd_spi_t *card, XSpiPs *spi);
int sd_spi_initialize(sd_spi_t *card);
int sd_spi_read(sd_spi_t *card, uint32_t sector, uint8_t *buffer,
                uint32_t count);
int sd_spi_write(sd_spi_t *card, uint32_t sector, const uint8_t *buffer,
                 uint32_t count);
int sd_spi_sync(sd_spi_t *card);
uint32_t sd_spi_sector_count(const sd_spi_t *card);
bool sd_spi_is_initialized(const sd_spi_t *card);
uint32_t sd_spi_detail(const sd_spi_t *card);
const char *sd_spi_diagnostic_string(const sd_spi_t *card);

#endif
