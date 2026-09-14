#include "mpu6050.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "app_config.h"
#include "iic_bus_recovery.h"
#include "iic_polled.h"
#include "task.h"
#include "xiic_l.h"
#include "xstatus.h"

#define MPU6050_REG_SMPLRT_DIV     0x19U
#define MPU6050_REG_CONFIG         0x1AU
#define MPU6050_REG_GYRO_CONFIG    0x1BU
#define MPU6050_REG_ACCEL_CONFIG   0x1CU
#define MPU6050_REG_ACCEL_XOUT_H   0x3BU
#define MPU6050_REG_PWR_MGMT_1     0x6BU
#define MPU6050_REG_PWR_MGMT_2     0x6CU
#define MPU6050_REG_WHO_AM_I       0x75U

#define MPU6050_WHO_AM_I_VALUE     0x68U

/*
 * Both accessors go through iic_polled.c rather than XIic_Send()/XIic_Recv():
 * the BSP transport can spin forever on a wedged bus, which hangs
 * sensor_task inside the driver and so prevents its own recovery path from
 * ever running.  See iic_polled.h.
 *
 * XIic_Reset() is issued ONLY after a failed transfer, never on every
 * transaction: a controller reset in the middle of a transfer is precisely
 * what strands the slave holding SDA low, forcing the recovery path to run
 * unnecessarily.  A completed transfer already leaves REPEATED_START clear,
 * so nothing needs resetting on the success path.
 */
static int mpu6050_write_register(mpu6050_t *device, uint8_t reg,
                                  uint8_t value)
{
    uint8_t transaction[2] = {reg, value};
    unsigned sent;

    sent = iic_polled_send(device->iic->BaseAddress, device->address,
                           transaction, sizeof(transaction), XIIC_STOP,
                           APP_MPU6050_I2C_TIMEOUT_US);
    if (sent != sizeof(transaction)) {
        XIic_Reset(device->iic);
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int mpu6050_read_burst(mpu6050_t *device, uint8_t reg,
                              uint8_t *buffer, unsigned length)
{
    unsigned transferred;

    transferred = iic_polled_send(device->iic->BaseAddress, device->address,
                                  &reg, 1U, XIIC_REPEATED_START,
                                  APP_MPU6050_I2C_TIMEOUT_US);
    if (transferred != 1U) {
        XIic_Reset(device->iic);
        return XST_FAILURE;
    }

    transferred = iic_polled_recv(device->iic->BaseAddress, device->address,
                                  buffer, length, XIIC_STOP,
                                  APP_MPU6050_I2C_TIMEOUT_US);
    if (transferred != length) {
        XIic_Reset(device->iic);
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int mpu6050_read_registers(mpu6050_t *device, uint8_t reg,
                                  uint8_t *buffer, unsigned length)
{
    if ((buffer == NULL) || (length == 0U)) {
        return XST_INVALID_PARAM;
    }

#if APP_MPU6050_BURST_READ_ENABLED
    /* One repeated-start burst. The old per-register loop existed because the
     * BSP's multi-byte receive could return success with an all-zero buffer;
     * iic_polled.c handles the last-two-byte NO-ACK/MSMS sequencing itself,
     * which is where that path went wrong. See APP_MPU6050_BURST_READ_ENABLED
     * for the fallback. */
    return mpu6050_read_burst(device, reg, buffer, length);
#else
    {
    unsigned index;

    for (index = 0U; index < length; ++index) {
        if (mpu6050_read_burst(device, (uint8_t)(reg + index),
                               &buffer[index], 1U) != XST_SUCCESS) {
            return XST_FAILURE;
        }
    }

    return XST_SUCCESS;
    }
#endif
}

static int16_t decode_be_i16(const uint8_t *bytes)
{
    return (int16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

void mpu6050_bind(mpu6050_t *device, XIic *iic, uint8_t address)
{
    if (device == NULL) {
        return;
    }

    device->iic = iic;
    device->address = address;
    device->initialized = 0U;
    device->who_am_i = 0U;
    device->pwr_mgmt_1 = 0U;
    device->pwr_mgmt_2 = 0U;
    device->config = 0U;
    device->sample_rate_divider = 0U;
    device->gyro_config = 0U;
    device->accel_config = 0U;
    device->last_diagnostic = MPU6050_DIAG_NONE;
}

int mpu6050_configure(mpu6050_t *device)
{
    uint8_t identity = 0U;
    uint8_t pwr_mgmt_1 = 0U;
    uint8_t config = 0U;
    uint8_t sample_rate_divider = 0U;
    uint8_t gyro_config = 0U;
    uint8_t accel_config = 0U;
    uint8_t pwr_mgmt_2 = 0U;

    if ((device == NULL) || (device->iic == NULL)) {
        return XST_INVALID_PARAM;
    }

    device->initialized = 0U;
    device->last_diagnostic = MPU6050_DIAG_NONE;

    /* Best-effort: force the core to drive real SCL/SDA activity even if
     * its Bus Busy status is stuck from an interrupted prior transaction
     * (JTAG reload/reboot). XIic_Reset() alone does not do this -- see
     * iic_bus_recovery.h. */
    (void)iic_bus_force_recover(device->iic->BaseAddress, device->address,
                                APP_MPU6050_BUS_RECOVERY_ATTEMPTS);

    XIic_Reset(device->iic);

    if (mpu6050_read_registers(device, MPU6050_REG_WHO_AM_I,
                               &identity, 1U) != XST_SUCCESS) {
        device->last_diagnostic = MPU6050_DIAG_WHO_AM_I_READ;
        return XST_FAILURE;
    }
    device->who_am_i = identity;
    if ((identity & 0x7EU) != MPU6050_WHO_AM_I_VALUE) {
        device->last_diagnostic = MPU6050_DIAG_WHO_AM_I_VALUE;
        return XST_FAILURE;
    }

    if (mpu6050_write_register(device, MPU6050_REG_PWR_MGMT_1,
                               0x80U) != XST_SUCCESS) {
        device->last_diagnostic = MPU6050_DIAG_DEVICE_RESET_WRITE;
        return XST_FAILURE;
    }
    vTaskDelay(pdMS_TO_TICKS(100U));

    /* Mirror the Arduino sketch: +/-8 g, +/-500 deg/s and 21 Hz DLPF.
     * The 1 kHz internal rate with divider 9 still gives our 100 Hz output.
     * Give it 100 ms after configuration to start fresh conversions. */
    if ((mpu6050_write_register(device, MPU6050_REG_PWR_MGMT_1,
                                0x01U) != XST_SUCCESS) ||
        (mpu6050_write_register(device, MPU6050_REG_CONFIG, 0x04U) != XST_SUCCESS) ||
        (mpu6050_write_register(device, MPU6050_REG_SMPLRT_DIV, 9U) != XST_SUCCESS) ||
        (mpu6050_write_register(device, MPU6050_REG_GYRO_CONFIG, 0x08U) != XST_SUCCESS) ||
        (mpu6050_write_register(device, MPU6050_REG_ACCEL_CONFIG, 0x10U) != XST_SUCCESS)) {
        device->last_diagnostic = MPU6050_DIAG_CONFIG_WRITE;
        return XST_FAILURE;
    }

    vTaskDelay(pdMS_TO_TICKS(100U));

    /* Read configuration back: a valid WHO_AM_I alone proves only that one
     * read worked.  These checks prove writes reached the MPU and that it is
     * awake before sensor_task accepts samples. */
    if ((mpu6050_read_registers(device, MPU6050_REG_PWR_MGMT_1,
                                &pwr_mgmt_1, 1U) != XST_SUCCESS) ||
        (mpu6050_read_registers(device, MPU6050_REG_CONFIG,
                                &config, 1U) != XST_SUCCESS) ||
        (mpu6050_read_registers(device, MPU6050_REG_SMPLRT_DIV,
                                &sample_rate_divider, 1U) != XST_SUCCESS) ||
        (mpu6050_read_registers(device, MPU6050_REG_GYRO_CONFIG,
                                &gyro_config, 1U) != XST_SUCCESS) ||
        (mpu6050_read_registers(device, MPU6050_REG_ACCEL_CONFIG,
                                &accel_config, 1U) != XST_SUCCESS) ||
        (mpu6050_read_registers(device, MPU6050_REG_PWR_MGMT_2,
                                &pwr_mgmt_2, 1U) != XST_SUCCESS)) {
        device->last_diagnostic = MPU6050_DIAG_CONFIG_READ;
        return XST_FAILURE;
    }
    device->pwr_mgmt_1 = pwr_mgmt_1;
    device->pwr_mgmt_2 = pwr_mgmt_2;
    device->config = config;
    device->sample_rate_divider = sample_rate_divider;
    device->gyro_config = gyro_config;
    device->accel_config = accel_config;
    if (((pwr_mgmt_1 & 0x40U) != 0U) ||
        ((pwr_mgmt_1 & 0x07U) != 0x01U) ||
        (config != 0x04U) ||
        (sample_rate_divider != 9U) ||
        (gyro_config != 0x08U) ||
        (accel_config != 0x10U)) {
        device->last_diagnostic = MPU6050_DIAG_CONFIG_VALUE;
        return XST_FAILURE;
    }

    device->initialized = 1U;
    return XST_SUCCESS;
}

int mpu6050_read_sample(mpu6050_t *device, mpu6050_sample_t *sample)
{
    uint8_t raw[14];

    if ((device == NULL) || (sample == NULL) ||
        (device->initialized == 0U)) {
        return XST_INVALID_PARAM;
    }

    if (mpu6050_read_registers(device, MPU6050_REG_ACCEL_XOUT_H,
                               raw, sizeof(raw)) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    sample->accel_x = decode_be_i16(&raw[0]);
    sample->accel_y = decode_be_i16(&raw[2]);
    sample->accel_z = decode_be_i16(&raw[4]);
    sample->temperature = decode_be_i16(&raw[6]);
    sample->gyro_x = decode_be_i16(&raw[8]);
    sample->gyro_y = decode_be_i16(&raw[10]);
    sample->gyro_z = decode_be_i16(&raw[12]);
    return XST_SUCCESS;
}
