#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>

/* I2C address*/
#define AS7343_I2C_ADDR     0x39

/* Register addresses*/
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

/* Status bits*/
#define AS7343_STATUS2_AVALID   BIT(6)

/* Channel counts*/
#define AS7343_NUM_CHANNELS  18
#define AS7343_GAIN_STEPS    12

/* Wavelength sentinels */
/* 0   = flicker detect — excluded from all calculations*/
/* 999 = VIS broadband clear — exposed via vis_irradiance_mW */
#define AS7343_WL_FLICKER   0
#define AS7343_WL_VIS_CLEAR 999

/* {wavelength_nm, irradiance_mW} pair — used by as7343_get_spectrum()*/
struct as7343_point {
    uint16_t wavelength_nm;
    uint16_t value;
};

/* Driver data*/
struct as7343_data {

    /* Raw 16-bit ADC counts, DATA0..DATA17 */
    uint16_t channel_data[AS7343_NUM_CHANNELS];

    /* Wavelength per channel (nm). 0=flicker, 999=VIS clear */
    uint16_t wavelengths[AS7343_NUM_CHANNELS];

    /* BasicCounts after dark subtraction, gain correction, and spectral*/
    /* Units: counts / (gain_ratio * tint_ms),*/
    float basic_counts[AS7343_NUM_CHANNELS];

    /* Per-channel irradiance in mW/m² after full calibration chain.*/
    float irradiance_mW[AS7343_NUM_CHANNELS];

    /* VIS broadband irradiance (mW/m²) — direct access without wl_index()*/
    float vis_irradiance_mW;

    /* Dark offset BasicCounts per channel, measured at startup in darkness.*/
    float dark_basic[AS7343_NUM_CHANNELS];

    /* True once as7343_calibrate_dark() has been called successfully.*/
    bool dark_calibrated;

    /* Current AGAIN gain step index (0=0.5x ... 11=1024x). */
    /* Updated automatically by auto-ranging in sample_fetch.*/
    uint8_t again_idx;
};

/* Driver config*/
struct as7343_config {
    struct i2c_dt_spec i2c;
};

/**
 * @brief One-time dark offset calibration — call at startup with sensor covered.
 *
 * Averages 10 samples to compute a BasicCounts dark offset per channel.
 * Offset is gain-independent and remains valid across auto-ranger steps.
 * If not called, dark subtraction is skipped and readings include the
 * dark current floor. NOT ACTUALLY USING CURRENTLY
 *
 * @param dev  AS7343 device handle.
 * @return     0 on success, negative errno on failure.
 */
extern int as7343_calibrate_dark(const struct device *dev);

/**
 * @brief Fetch a calibrated spectrum from the AS7343.
 *
 * Fills spectrum[i].wavelength_nm and spectrum[i].value (irradiance in
 * mW/m², clamped to uint16). Also updates dev->data->vis_irradiance_mW
 * with the broadband VIS channel value.
 *
 * @param dev       AS7343 device handle.
 * @param spectrum  Output array of AS7343_NUM_CHANNELS points.
 * @return          0 on success, negative errno on failure.
 */
extern int as7343_get_spectrum(const struct device *dev,
                               struct as7343_point spectrum[AS7343_NUM_CHANNELS]);