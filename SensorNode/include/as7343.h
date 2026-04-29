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
#define AS7343_STATUS2_AVALID   BIT(6)

/* -------------------------------------------------------------------------- */
/* Channel counts                                                              */
#define AS7343_NUM_CHANNELS  18
#define AS7343_GAIN_STEPS    12

/* -------------------------------------------------------------------------- */
/* Wavelength sentinels                                                        */
/* 0   = flicker detect — excluded from all calculations                      */
/* 999 = VIS broadband clear — exposed via vis_irradiance_mW                  */
#define AS7343_WL_FLICKER   0
#define AS7343_WL_VIS_CLEAR 999

/* -------------------------------------------------------------------------- */
/* {wavelength_nm, irradiance_mW} pair — used by as7343_get_spectrum()        */
struct as7343_point {
    uint16_t wavelength_nm;
    uint16_t value;
};

/* -------------------------------------------------------------------------- */
/* Driver data                                                                 */
struct as7343_data {

    /* Raw 16-bit ADC counts, DATA0..DATA17 */
    uint16_t channel_data[AS7343_NUM_CHANNELS];

    /* Wavelength per channel (nm). 0=flicker, 999=VIS clear */
    uint16_t wavelengths[AS7343_NUM_CHANNELS];

    /* BasicCounts after dark subtraction, gain correction, and spectral      */
    /* correction — intermediate value, exposed for diagnostics.              */
    /* Units: counts / (gain_ratio * tint_ms), dimensionless.                 */
    float basic_counts[AS7343_NUM_CHANNELS];

    /* Per-channel irradiance in mW/m² after full calibration chain.          */
    /* Spectral channels: Steps 1-5 applied.                                  */
    /* VIS clear (wavelength=999): averaged DATA4/10/16 + Fig.9 R_typ=999.   */
    /* Flicker/unused (wavelength=0): always 0.0                              */
    float irradiance_mW[AS7343_NUM_CHANNELS];

    /* VIS broadband irradiance (mW/m²) — direct access without wl_index()   */
    float vis_irradiance_mW;

    /* Dark offset BasicCounts per channel, measured at startup in darkness.  */
    /* Subtracted from every sample in Step 2. Gain-independent (BasicCounts) */
    float dark_basic[AS7343_NUM_CHANNELS];

    /* True once as7343_calibrate_dark() has been called successfully.        */
    bool dark_calibrated;

    /* Current AGAIN gain step index (0=0.5x ... 11=1024x).                  */
    /* Updated automatically by auto-ranging in sample_fetch.                 */
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
/* val->val2 = irradiance in µW/m² (mW/m² * 1000, as int32)                 */
/*                                                                             */
/* To recover mW/m²:  float e = val.val2 / 1000.0f;                          */
/* To recover W/m²:   float e = val.val2 / 1000000.0f;                       */

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */

/*
 * as7343_calibrate_dark() — one-time dark offset calibration.
 * Call at startup with the sensor FULLY COVERED (no light).
 * Averages 10 samples to compute BasicCounts offset per channel.
 * Offset is gain-independent and remains valid after auto-ranger steps.
 * If not called, dark subtraction is skipped (readings are still valid
 * but will include the dark current floor).
 */
extern int as7343_calibrate_dark(const struct device *dev);

/*
 * as7343_get_spectrum() — fetch and fill spectrum array.
 * spectrum[i].wavelength_nm = channel wavelength (0=flicker, 999=VIS)
 * spectrum[i].value         = irradiance in mW/m², clamped to uint16
 * After call, dev->data->vis_irradiance_mW has the broadband VIS value.
 */
extern int as7343_get_spectrum(const struct device *dev,
                               struct as7343_point spectrum[AS7343_NUM_CHANNELS]);