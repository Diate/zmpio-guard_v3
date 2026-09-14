#include "diskio_sd.h"

#include <stddef.h>

#include "diskio.h"
#include "cpu1_log.h"
#include "ff.h"
#include "xstatus.h"

static sd_spi_t *bound_card;

void sd_disk_bind(sd_spi_t *card)
{
    bound_card = card;
}

uint32_t sd_disk_diagnostic(void)
{
    return bound_card != NULL ? (uint32_t)bound_card->diagnostic : 0U;
}

uint32_t sd_disk_diagnostic_detail(void)
{
    return bound_card != NULL ? sd_spi_detail(bound_card) : 0U;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    int status;

    if ((pdrv != 0U) || (bound_card == NULL)) {
        return STA_NOINIT;
    }
    status = sd_spi_initialize(bound_card);
    if (status != XST_SUCCESS) {
        CPU1_LOG("CPU1: SD SPI init failed stage=%s\r\n",
                   sd_spi_diagnostic_string(bound_card));
        return STA_NOINIT;
    }

    CPU1_LOG("CPU1: SD SPI init OK sectors=%lu capacity=%lu MiB\r\n",
               (unsigned long)sd_spi_sector_count(bound_card),
               (unsigned long)(sd_spi_sector_count(bound_card) / 2048U));
    return 0U;
}

DSTATUS disk_status(BYTE pdrv)
{
    if ((pdrv != 0U) || (bound_card == NULL) ||
        !sd_spi_is_initialized(bound_card)) {
        return STA_NOINIT;
    }
    return 0U;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if ((pdrv != 0U) || (bound_card == NULL) || (buff == NULL) ||
        (count == 0U) || (sector > UINT32_MAX)) {
        return RES_PARERR;
    }
    return sd_spi_read(bound_card, (uint32_t)sector, buff,
                       (uint32_t)count) == XST_SUCCESS ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if ((pdrv != 0U) || (bound_card == NULL) || (buff == NULL) ||
        (count == 0U) || (sector > UINT32_MAX)) {
        return RES_PARERR;
    }
    return sd_spi_write(bound_card, (uint32_t)sector, buff,
                        (uint32_t)count) == XST_SUCCESS ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    if ((pdrv != 0U) || (bound_card == NULL)) {
        return RES_PARERR;
    }

    switch (command) {
    case CTRL_SYNC:
        return sd_spi_sync(bound_card) == XST_SUCCESS ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(LBA_t *)buffer = (LBA_t)sd_spi_sector_count(bound_card);
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(WORD *)buffer = SD_SPI_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buffer = 1U;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    /* No RTC is currently shared with CPU1: use a deterministic valid date. */
    return ((DWORD)(2026U - 1980U) << 25U) |
           ((DWORD)1U << 21U) |
           ((DWORD)1U << 16U);
}
