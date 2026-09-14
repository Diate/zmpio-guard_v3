#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>

#include "xiic.h"
#include "zmpio_protocol.h"

typedef ipc_mpu6050_sample_t mpu6050_sample_t;

typedef enum {
    MPU6050_DIAG_NONE = 0U,
    MPU6050_DIAG_WHO_AM_I_READ,
    MPU6050_DIAG_WHO_AM_I_VALUE,
    MPU6050_DIAG_DEVICE_RESET_WRITE,
    MPU6050_DIAG_CONFIG_WRITE,
    MPU6050_DIAG_CONFIG_READ,
    MPU6050_DIAG_CONFIG_VALUE
} mpu6050_diagnostic_t;

typedef struct {
    XIic *iic;
    uint8_t address;
    uint8_t initialized;
    /* Last values read back from the device after a successful configure. */
    uint8_t who_am_i;
    uint8_t pwr_mgmt_1;
    uint8_t pwr_mgmt_2;
    uint8_t config;
    uint8_t sample_rate_divider;
    uint8_t gyro_config;
    uint8_t accel_config;
    mpu6050_diagnostic_t last_diagnostic;
} mpu6050_t;

void mpu6050_bind(mpu6050_t *device, XIic *iic, uint8_t address);
int mpu6050_configure(mpu6050_t *device);
int mpu6050_read_sample(mpu6050_t *device, mpu6050_sample_t *sample);

#endif
