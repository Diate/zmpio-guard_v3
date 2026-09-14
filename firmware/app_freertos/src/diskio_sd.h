#ifndef DISKIO_SD_H
#define DISKIO_SD_H

#include <stdint.h>

#include "sd_spi.h"

void sd_disk_bind(sd_spi_t *card);
uint32_t sd_disk_diagnostic(void);
uint32_t sd_disk_diagnostic_detail(void);

#endif
