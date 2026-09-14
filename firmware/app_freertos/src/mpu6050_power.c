#include "mpu6050_power.h"

#include "FreeRTOS.h"
#include "app_config.h"
#include "cpu1_log.h"
#include "task.h"
#include "xgpiops.h"
#include "xparameters.h"
#include "xstatus.h"

#if APP_MPU6050_PWR_GPIO_ENABLED
static XGpioPs gpio;
static uint8_t gpio_ready;

static int ensure_gpio_ready(void)
{
    XGpioPs_Config *config_ptr;
    int status;

    if (gpio_ready) {
        return XST_SUCCESS;
    }

    config_ptr = XGpioPs_LookupConfig(XPAR_XGPIOPS_0_BASEADDR);
    if (config_ptr == NULL) {
        CPU1_LOG("CPU1: WARNING XGpioPs_LookupConfig failed (MPU6050 power)\r\n");
        return XST_FAILURE;
    }

    status = XGpioPs_CfgInitialize(&gpio, config_ptr, config_ptr->BaseAddr);
    if (status != XST_SUCCESS) {
        CPU1_LOG("CPU1: WARNING XGpioPs_CfgInitialize failed %d (MPU6050 power)\r\n",
                 status);
        return XST_FAILURE;
    }

    XGpioPs_SetDirectionPin(&gpio, APP_MPU6050_PWR_EMIO_PIN, 1);
    XGpioPs_SetOutputEnablePin(&gpio, APP_MPU6050_PWR_EMIO_PIN, 1);
    gpio_ready = 1U;
    return XST_SUCCESS;
}
#endif /* APP_MPU6050_PWR_GPIO_ENABLED */

void mpu6050_power_cycle(void)
{
#if APP_MPU6050_PWR_GPIO_ENABLED
    if (ensure_gpio_ready() != XST_SUCCESS) {
        return;
    }

    CPU1_LOG("CPU1: MPU6050 power cycle (off %u ms)...\r\n",
             (unsigned int)APP_MPU6050_PWR_OFF_MS);
    XGpioPs_WritePin(&gpio, APP_MPU6050_PWR_EMIO_PIN, 1); /* active-low: 1 = off */
    vTaskDelay(pdMS_TO_TICKS(APP_MPU6050_PWR_OFF_MS));
    XGpioPs_WritePin(&gpio, APP_MPU6050_PWR_EMIO_PIN, 0); /* active-low: 0 = on */
    vTaskDelay(pdMS_TO_TICKS(APP_MPU6050_PWR_STABLE_MS));
    CPU1_LOG("CPU1: MPU6050 power restored\r\n");
#endif
}

void mpu6050_power_init(void)
{
#if APP_MPU6050_PWR_GPIO_ENABLED
    if (ensure_gpio_ready() != XST_SUCCESS) {
        return;
    }
    mpu6050_power_cycle();
#endif
}
