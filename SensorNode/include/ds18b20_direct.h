/*
 * ds18b20_direct.h
 *
 * Direct DS18B20 1-Wire driver for ESP32 on Zephyr RTOS.
 * Bypasses Zephyr's w1_serial driver entirely.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DS18B20_DIRECT_H
#define DS18B20_DIRECT_H

#include <zephyr/drivers/sensor.h>

/**
 * @brief Initialise the DS18B20 direct UART driver.
 *        Must be called once before ds18b20_direct_read().
 *
 * @return 0 on success, negative errno on failure
 */
int ds18b20_direct_init(void);

/**
 * @brief Read temperature from DS18B20 in degrees Celsius.
 *
 * @param temp_c  Pointer to float to store temperature
 * @return 0 on success
 *         -ENODEV if no device detected on bus
 *         -EIO    if CRC error or UART bus error
 */
int ds18b20_direct_read(float *temp_c);

/**
 * @brief Read temperature into a Zephyr sensor_value struct.
 *        val1 = integer degrees C
 *        val2 = fractional part in millionths (e.g. 62500 = 0.0625°C)
 *
 * @param val  Pointer to sensor_value to populate
 * @return 0 on success, negative errno on failure
 */
int ds18b20_direct_read_sensor_value(struct sensor_value *val);

#endif /* DS18B20_DIRECT_H */
