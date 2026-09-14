#ifndef MPU6050_POWER_H
#define MPU6050_POWER_H

/* Configures the MPU6050 power-switch GPIO and forces one unconditional
 * off/on cycle. Call once before the first XIic transaction to the sensor --
 * the sensor may still be powered from a prior firmware load, so "turn on"
 * alone is not enough to clear a bus lockup left over from that run. */
void mpu6050_power_init(void);

/* Cuts and restores MPU6050 VCC/GND (APP_MPU6050_PWR_OFF_MS off, then
 * APP_MPU6050_PWR_STABLE_MS settle) to force a power-on reset of the chip's
 * I2C state machine. Call whenever mpu6050_configure()/read repeatedly fail,
 * i.e. the same recovery a physical unplug/replug currently provides by
 * hand. */
void mpu6050_power_cycle(void);

#endif
