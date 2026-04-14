#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>

/* Default I2C address (check your wiring / ADDR pin) */
#define AS7343_I2C_ADDR 0x39

/* Data registers */
#define AS7343_CH0_DATA_L   0x95
#define AS7343_STATUS       0x93
#define AS7343_ENABLE       0x80

/* Status bits */
#define AS7343_STATUS_AVALID   BIT(0)

/* Number of spectral channels */
#define AS7343_NUM_CHANNELS 18

/* {wavelength,value} pair */
struct as7343_point {
    uint16_t wavelength_nm;
    uint16_t value;
};

/* Driver data */
struct as7343_data {
    uint16_t channel_data[AS7343_NUM_CHANNELS];
    uint16_t wavelengths[AS7343_NUM_CHANNELS];   /* store λ for each channel */
};

/* Driver config */
struct as7343_config {
    struct i2c_dt_spec i2c;
};

/* Public helper */
extern int as7343_get_spectrum(const struct device *dev,
                        struct as7343_point spectrum[AS7343_NUM_CHANNELS]);

