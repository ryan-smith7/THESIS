#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>

/* -------------------------------------------------------------------------- */
/* I2C address                                                                 */

#define AS7343_I2C_ADDR     0x39

/* -------------------------------------------------------------------------- */
/* Register addresses                                                          */

#define AS7343_REG_ENABLE   0x80
#define AS7343_REG_ATIME    0x81
#define AS7343_REG_STATUS2  0x90
#define AS7343_REG_STATUS   0x93
#define AS7343_REG_CH0_L    0x95
#define AS7343_REG_AGAIN    0xC6
#define AS7343_REG_ASTEP_L  0xD4
#define AS7343_REG_ASTEP_H  0xD5
#define AS7343_REG_CFG20    0xD6
#define AS7343_REG_BANK     0xBF

/* -------------------------------------------------------------------------- */
/* Status bits                                                                 */

#define AS7343_STATUS2_AVALID   BIT(6)  /* STATUS2 reg 0x90 — data ready     */

/* -------------------------------------------------------------------------- */
/* Channel counts                                                              */

#define AS7343_NUM_CHANNELS  18   /* total DATA registers (spectral+clear+FD) */
#define AS7343_GAIN_STEPS    12   /* 0.5x through 1024x                        */

/* -------------------------------------------------------------------------- */
/* Wavelength sentinels                                                        */
/* 0   = flicker detect channel  — excluded from all calculations             */
/* 999 = VIS broadband clear channel — exposed separately as vis_irradiance_mW */

#define AS7343_WL_FLICKER   0
#define AS7343_WL_VIS_CLEAR 999

/* -------------------------------------------------------------------------- */
/* {wavelength_nm, irradiance_mW} pair — used by as7343_get_spectrum()        */
/* value field holds irradiance in mW/m² clamped to uint16 (max ~65 W/m²)    */

struct as7343_point {
    uint16_t wavelength_nm;
    uint16_t value;
};

/* -------------------------------------------------------------------------- */
/* Driver data — internal state                                                */

struct as7343_data {
    /* Raw 16-bit ADC counts, DATA0..DATA17 */
    uint16_t channel_data[AS7343_NUM_CHANNELS];

    /* Wavelength per channel (nm). 0=flicker, 999=VIS clear */
    uint16_t wavelengths[AS7343_NUM_CHANNELS];

    /* Per-channel irradiance in mW/m²                                        */
    /* Spectral channels (r_typ > 0): computed from datasheet Fig.8           */
    /* VIS clear (wavelength=999): computed from datasheet Fig.9, R_typ=999   */
    /* Flicker/unused (wavelength=0): always 0.0                              */
    float irradiance_mW[AS7343_NUM_CHANNELS];

    /* Summed irradiance of the VIS broadband channel (DATA4 + DATA10 + DATA16) */
    /* Units: mW/m². Use for a single total-visible irradiance number.         */
    float vis_irradiance_mW;

    /* Current AGAIN gain step index (0=0.5x ... 11=1024x)                   */
    /* Updated automatically by auto-ranging in sample_fetch                  */
    uint8_t again_idx;
};

/* -------------------------------------------------------------------------- */
/* Driver config                                                               */

struct as7343_config {
    struct i2c_dt_spec i2c;
};

/* -------------------------------------------------------------------------- */
/* sensor_channel_get encoding                                                 */
/*                                                                             */
/* val->val1 = wavelength_nm  (0, 999, or 405..855)                          */
/* val->val2 = irradiance in units of 0.001 mW/m²  (i.e. µW/m²)             */
/*                                                                             */
/* To recover mW/m²:  float e = val.val2 / 1000.0f;                          */
/* To recover W/m²:   float e = val.val2 / 1000000.0f;                       */
/*                                                                             */
/* Raw counts remain accessible via data->channel_data[idx] directly.        */

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */

/*
 * as7343_get_spectrum() — fetch + fill spectrum array.
 * spectrum[i].wavelength_nm = channel wavelength (skip where == 0 or 999)
 * spectrum[i].value         = irradiance in mW/m², clamped to uint16
 *
 * After this call, dev->data->vis_irradiance_mW holds the summed VIS
 * broadband irradiance from the three clear photodiodes.
 */
extern int as7343_get_spectrum(const struct device *dev,
                               struct as7343_point spectrum[AS7343_NUM_CHANNELS]);


// #pragma once

// #include <zephyr/device.h>
// #include <zephyr/drivers/sensor.h>
// #include <zephyr/drivers/i2c.h>

// /* Default I2C address (check your wiring / ADDR pin) */
// #define AS7343_I2C_ADDR 0x39

// /* Data registers */
// #define AS7343_CH0_DATA_L   0x95
// #define AS7343_STATUS       0x93
// #define AS7343_ENABLE       0x80

// /* Status bits */
// #define AS7343_STATUS_AVALID   BIT(0)

// /* Number of spectral channels */
// #define AS7343_NUM_CHANNELS 18

// // Add gain step count
// #define AS7343_GAIN_STEPS 12

// /* {wavelength,value} pair */
// struct as7343_point {
//     uint16_t wavelength_nm;
//     uint16_t value;
// };

// /* Driver data */
// struct as7343_data {
//     uint16_t channel_data[AS7343_NUM_CHANNELS];
//     uint16_t wavelengths[AS7343_NUM_CHANNELS];   /* store λ for each channel */
// };

// // Add to struct as7343_data:
// struct as7343_data {
//     uint16_t channel_data[AS7343_NUM_CHANNELS];
//     uint16_t wavelengths[AS7343_NUM_CHANNELS];
//     uint8_t  again_idx;          // current gain step index (0–11)
//     float    irradiance_mW[AS7343_NUM_CHANNELS]; // computed E_i per channel
// };

// /* Driver config */
// struct as7343_config {
//     struct i2c_dt_spec i2c;
// };

// /* Public helper */
// extern int as7343_get_spectrum(const struct device *dev,
//                         struct as7343_point spectrum[AS7343_NUM_CHANNELS]);

