#define DT_DRV_COMPAT ams_as7343

#include "as7343.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(AS7343, CONFIG_SENSOR_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/* Wavelength mapping (nm) per channel index                                  */
/* 999 = VIS clear channel (intentional sentinel, used by application layer)  */
/* 0   = flicker detect channel (excluded from all calculations)              */

static const uint16_t wavelength_map[18] = {
    450,  // DATA0  = FZ
    555,  // DATA1  = FY
    600,  // DATA2  = FXL
    855,  // DATA3  = NIR
    999,  // DATA4  = VIS clear (TL)
    0,    // DATA5  = Flicker detect
    425,  // DATA6  = F2
    475,  // DATA7  = F3
    515,  // DATA8  = F4
    640,  // DATA9  = F6
    0,    // DATA10 = VIS clear (BR)
    0,    // DATA11 = Flicker detect
    405,  // DATA12 = F1
    690,  // DATA13 = F7
    745,  // DATA14 = F8
    550,  // DATA15 = F5
    0,    // DATA16 = VIS clear (RB)
    0     // DATA17 = Flicker detect
};

static const uint8_t as7343_data_lsb[18] = {
    0x95, // DATA0_L
    0x97, // DATA1_L
    0x99, // DATA2_L
    0x9B, // DATA3_L
    0x9D, // DATA4_L
    0x9F, // DATA5_L
    0xA1, // DATA6_L
    0xA3, // DATA7_L
    0xA5, // DATA8_L
    0xA7, // DATA9_L
    0xA9, // DATA10_L
    0xAB, // DATA11_L
    0xAD, // DATA12_L
    0xAF, // DATA13_L
    0xB1, // DATA14_L
    0xB3, // DATA15_L
    0xB5, // DATA16_L
    0xB7  // DATA17_L
};

/* -------------------------------------------------------------------------- */
/* Gain table                                                                  */
/* Ratios from datasheet Fig.10, relative to 64x reference.                   */
/* 0.5x–16x entries are divided by 1000 per datasheet note (5).               */

static const uint8_t gain_reg[AS7343_GAIN_STEPS] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,  /* 0.5x  1x   2x   4x   8x   16x  */
    0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C   /* 32x   64x  128x 256x 512x 1024x */
};

static const float gain_ratio[AS7343_GAIN_STEPS] = {
    0.0079f, 0.0158f, 0.0316f, 0.064f, 0.124f, 0.247f,
    0.5f,    1.0f,    2.0f,    4.1f,   8.6f,   16.9f
};

/* -------------------------------------------------------------------------- */
/* Datasheet Fig.8 R_typ values                                               */
/* Conditions: AGAIN=1024x, t_int=27.8ms, Ee=155 mW/m²                       */
/* Index matches DATA0..DATA17 order. 0 = non-spectral channel.               */

static const float r_typ[18] = {
    2169.0f,   // DATA0  FZ  450nm
    3747.0f,   // DATA1  FY  555nm
    4776.0f,   // DATA2  FXL 600nm
    10581.0f,  // DATA3  NIR 855nm
    0.0f,      // DATA4  VIS clear (999)
    0.0f,      // DATA5  flicker
    1756.0f,   // DATA6  F2  425nm
    770.0f,    // DATA7  F3  475nm
    3141.0f,   // DATA8  F4  515nm
    3336.0f,   // DATA9  F6  640nm
    0.0f,      // DATA10 VIS clear
    0.0f,      // DATA11 flicker
    5749.0f,   // DATA12 F1  405nm
    5435.0f,   // DATA13 F7  690nm
    864.0f,    // DATA14 F8  745nm
    1574.0f,   // DATA15 F5  550nm
    0.0f,      // DATA16 VIS clear
    0.0f       // DATA17 flicker
};

/* -------------------------------------------------------------------------- */
/* Integration time and full-scale configuration                              */
/* full_scale = (ATIME+1) * (ASTEP+1) - 1 = 64 * 1024 - 1 = 65535           */

#define ATIME_VAL       63
#define ASTEP_LSB       0xFF    /* ASTEP=1023 → LSB=0xFF MSB=0x03 */
#define ASTEP_MSB       0x03
#define GAIN_START_IDX  8       /* 128x — safe starting point for indoor light */

/* Auto-range thresholds as fraction of full scale (65535) */
#define CLIP_HI  62258U   /* 95% — reduce gain if any spectral channel exceeds */
#define CLIP_LO   3276U   /*  5% — increase gain if brightest channel is below  */

/* Datasheet reference conditions (Fig.8) */
#define REF_RATIO    16.9f   /* gain_ratio for 1024x */
#define REF_TINT_MS  27.8f   /* integration time used in datasheet table */
#define REF_EE       155.0f  /* irradiance stimulus in mW/m² */

static inline float as7343_tint_ms(void)
{
    return (float)(ATIME_VAL + 1) * (float)(1023 + 1) * 2.78f / 1000.0f;
}

/* -------------------------------------------------------------------------- */
/* Wait for AVALID                                                             */

static int as7343_wait_ready(const struct device *dev)
{
    const struct as7343_config *cfg = dev->config;
    uint8_t st2;

    for (int i = 0; i < 400; i++) {
        if (i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &st2) < 0)
            return -EIO;
        if (st2 & BIT(6))
            return 0;
        k_msleep(5);
    }
    return -ETIMEDOUT;
}

/* -------------------------------------------------------------------------- */
/* Set AGAIN register and update stored index                                 */

static int as7343_set_again(const struct device *dev, uint8_t idx)
{
    const struct as7343_config *cfg = dev->config;
    struct as7343_data *data = dev->data;

    if (idx >= AS7343_GAIN_STEPS) {
        return -EINVAL;
    }
    int ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr,
                                 0xC6, gain_reg[idx]);
    if (ret == 0) {
        data->again_idx = idx;
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/* sample_fetch — read raw counts with auto-ranging, then compute irradiance  */

static int as7343_sample_fetch(const struct device *dev,
                               enum sensor_channel chan)
{
    ARG_UNUSED(chan);
    const struct as7343_config *cfg = dev->config;
    struct as7343_data *data = dev->data;
    int ret;

    for (int attempt = 0; attempt < AS7343_GAIN_STEPS; attempt++) {

        /* Wait for 3 complete integration cycles */
        for (int cycle = 0; cycle < 3; cycle++) {
            ret = as7343_wait_ready(dev);
            if (ret < 0) return ret;
            uint8_t dummy;
            i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &dummy);
        }

        /* Read all 18 channel registers */
        for (int i = 0; i < 18; i++) {
            uint8_t lo = 0, hi = 0;
            i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr,
                              as7343_data_lsb[i], &lo);
            i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr,
                              as7343_data_lsb[i] + 1, &hi);
            data->channel_data[i] = ((uint16_t)hi << 8) | lo;
            data->wavelengths[i]  = wavelength_map[i];
        }

        /* Find peak count across spectral channels only (skip clear/flicker) */
        uint16_t peak = 0;
        for (int i = 0; i < 18; i++) {
            if (r_typ[i] > 0.0f && data->channel_data[i] > peak) {
                peak = data->channel_data[i];
            }
        }

        /* Auto-range: adjust gain and retake if outside window */
        if (peak >= CLIP_HI && data->again_idx > 0) {
            LOG_DBG("AS7343: peak=%u clipping, gain %u -> %u",
                    peak, data->again_idx, data->again_idx - 1);
            ret = as7343_set_again(dev, data->again_idx - 1);
            if (ret < 0) return ret;
            continue;
        }
        if (peak < CLIP_LO && data->again_idx < AS7343_GAIN_STEPS - 1) {
            LOG_DBG("AS7343: peak=%u too low, gain %u -> %u",
                    peak, data->again_idx, data->again_idx + 1);
            ret = as7343_set_again(dev, data->again_idx + 1);
            if (ret < 0) return ret;
            continue;
        }

        /* Good reading — compute irradiance per channel                      */
        /* E_i [mW/m²] = counts_i / S_i_scaled                               */
        /* S_i_scaled  = (r_typ[i] / REF_EE)                                 */
        /*             * (gain_ratio[idx] / REF_RATIO)                        */
        /*             * (tint_ms / REF_TINT_MS)                              */
        float gr = gain_ratio[data->again_idx];
        float ti = as7343_tint_ms();

        for (int i = 0; i < 18; i++) {
            if (r_typ[i] <= 0.0f) {
                data->irradiance_mW[i] = 0.0f;
                continue;
            }
            float s = (r_typ[i] / REF_EE)
                    * (gr / REF_RATIO)
                    * (ti / REF_TINT_MS);
            data->irradiance_mW[i] = (float)data->channel_data[i] / s;
        }

        /* VIS broadband clear channel (DATA4/10/16, wavelength=999)          */
        /* Datasheet Fig.9: R_typ=999 counts at same reference conditions,    */
        /* measured as "2 VIS PDs read-out" simultaneously.                   */
        /* We use DATA4 (the primary TL clear PD) with the Fig.9 responsivity.*/
        /* DATA10 and DATA16 are the same physical measurement repeated;      */
        /* averaging them reduces noise by ~sqrt(3).                          */
        {
            /* Fig.9 R_typ for VIS channel at 1024x, 27.8ms, 155mW/m² */
#define R_TYP_VIS  999.0f
            float s_vis = (R_TYP_VIS / REF_EE)
                        * (gr / REF_RATIO)
                        * (ti / REF_TINT_MS);

            /* Average the three VIS clear PD readings (DATA4, DATA10, DATA16) */
            float vis_avg = ((float)data->channel_data[4]
                           + (float)data->channel_data[10]
                           + (float)data->channel_data[16]) / 3.0f;

            data->vis_irradiance_mW = vis_avg / s_vis;

            /* Store in each VIS slot so channel_get can return it */
            data->irradiance_mW[4]  = data->vis_irradiance_mW;
            data->irradiance_mW[10] = data->vis_irradiance_mW;
            data->irradiance_mW[16] = data->vis_irradiance_mW;
        }

        return 0;
    }

    LOG_ERR("AS7343: auto-range failed to converge after %d attempts",
            AS7343_GAIN_STEPS);
    return -ETIMEDOUT;
}

/* -------------------------------------------------------------------------- */
/* channel_get                                                                 */
/* val->val1 = wavelength in nm  (999 = VIS clear, 0 = flicker/unused)       */
/* val->val2 = irradiance in units of 0.001 mW/m²  (i.e. µW/m²)             */
/*             raw counts still accessible via data->channel_data[idx]        */

static int as7343_channel_get(const struct device *dev,
                              enum sensor_channel chan,
                              struct sensor_value *val)
{
    struct as7343_data *data = dev->data;

    if (chan >= SENSOR_CHAN_PRIV_START) {
        int idx = chan - SENSOR_CHAN_PRIV_START;
        if (idx >= 0 && idx < AS7343_NUM_CHANNELS) {
            val->val1 = data->wavelengths[idx];
            val->val2 = (int32_t)(data->irradiance_mW[idx] * 1000.0f);
            return 0;
        }
    }
    return -ENOTSUP;
}

static const struct sensor_driver_api as7343_driver_api = {
    .sample_fetch = as7343_sample_fetch,
    .channel_get  = as7343_channel_get,
};

/* -------------------------------------------------------------------------- */
/* Spectrum helper — fills array with {wavelength_nm, irradiance_mW} pairs    */

int as7343_get_spectrum(const struct device *dev,
                        struct as7343_point spectrum[AS7343_NUM_CHANNELS])
{
    struct as7343_data *data = dev->data;
    int ret = as7343_sample_fetch(dev, SENSOR_CHAN_ALL);

    if (ret < 0) {
        return ret;
    }

    for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
        spectrum[i].wavelength_nm = data->wavelengths[i];
        spectrum[i].value         = (uint16_t)CLAMP(
                                        (int32_t)data->irradiance_mW[i],
                                        0, 0xFFFF);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Init                                                                        */

static int as7343_init(const struct device *dev)
{
    const struct as7343_config *cfg = dev->config;
    struct as7343_data *data = dev->data;
    int ret;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    data->again_idx = GAIN_START_IDX;

    /* Step 1: Power ON */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x01);
    if (ret < 0) return ret;

    /* Step 2: ATIME=63, ASTEP=1023 → full_scale=65535, t_int=179ms */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x81, ATIME_VAL);
    if (ret < 0) return ret;

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD4, ASTEP_LSB);
    if (ret < 0) return ret;

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD5, ASTEP_MSB);
    if (ret < 0) return ret;

    /* Step 3: AGAIN = 128x starting point */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xC6,
                             gain_reg[GAIN_START_IDX]);
    if (ret < 0) return ret;

    /* Step 4: Switch to bank 1 */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x01);
    if (ret < 0) return ret;

    /* Step 5: CFG20 autorun=3 (18 channels) */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD6, 0x60);
    if (ret < 0) return ret;

    /* Step 6: Back to bank 0 */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x00);
    if (ret < 0) return ret;

    /* Step 7: SMUXEN trigger, wait for auto-clear */
    i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x11);
    k_msleep(5);

    /* Step 8: Enable ALS (PON + AEN) */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x03);
    if (ret < 0) return ret;

    LOG_INF("AS7343 init: ATIME=%d ASTEP=1023 AGAIN=128x "
            "full_scale=65535 t_int=179ms",
            ATIME_VAL);
    return 0;
}

/* -------------------------------------------------------------------------- */

#define AS7343_DEFINE(inst)                                            \
    static struct as7343_data as7343_data_##inst;                      \
    static const struct as7343_config as7343_config_##inst = {         \
        .i2c = I2C_DT_SPEC_INST_GET(inst)                              \
    };                                                                 \
    DEVICE_DT_INST_DEFINE(inst, as7343_init, NULL,                     \
                          &as7343_data_##inst, &as7343_config_##inst,  \
                          POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,    \
                          &as7343_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AS7343_DEFINE)




// #define DT_DRV_COMPAT ams_as7343

// #include "as7343.h"
// #include <zephyr/logging/log.h>
// #include <zephyr/kernel.h>

// LOG_MODULE_REGISTER(AS7343, CONFIG_SENSOR_LOG_LEVEL);

// /* -------------------------------------------------------------------------- */
// /* Wavelength mapping (nm) per channel index                                  */

// static const uint16_t wavelength_map[18] = {
//     450,   // DATA0 = FZ
//     555,   // DATA1 = FY
//     600,   // DATA2 = FXL
//     855,   // DATA3 = NIR
//     999,     // DATA4 = VIS clear (TL)
//     0,     // DATA5 = Flicker detect
//     425,   // DATA6 = F2
//     475,   // DATA7 = F3
//     515,   // DATA8 = F4
//     640,   // DATA9 = F6
//     0,     // DATA10 = VIS clear (BR)
//     0,     // DATA11 = Flicker detect
//     405,   // DATA12 = F1
//     690,   // DATA13 = F7
//     745,   // DATA14 = F8
//     550,   // DATA15 = F5
//     0,     // DATA16 = VIS clear (RB)
//     0      // DATA17 = Flicker detect
// };

// static const uint8_t as7343_data_lsb[18] = {
//     0x95, // DATA0_L
//     0x97, // DATA1_L
//     0x99, // DATA2_L
//     0x9B, // DATA3_L
//     0x9D, // DATA4_L
//     0x9F, // DATA5_L
//     0xA1, // DATA6_L
//     0xA3, // DATA7_L
//     0xA5, // DATA8_L
//     0xA7, // DATA9_L
//     0xA9, // DATA10_L
//     0xAB, // DATA11_L
//     0xAD, // DATA12_L
//     0xAF, // DATA13_L
//     0xB1, // DATA14_L
//     0xB3, // DATA15_L
//     0xB5, // DATA16_L
//     0xB7  // DATA17_L
// };

// static int as7343_wait_ready(const struct device *dev) {
//     const struct as7343_config *cfg = dev->config;
//     uint8_t st2;
//     for (int i = 0; i < 400; i++) {             // ~2 s @ 5 ms
//         if (i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &st2) < 0)
//             return -EIO;
//         if (st2 & BIT(6))                       // AVALID (STATUS2 bit6)
//             return 0;
//         k_msleep(5);
//     }
//     return -ETIMEDOUT;
// }


// static int as7343_sample_fetch(const struct device *dev,
//                                enum sensor_channel chan) {
//     ARG_UNUSED(chan);
//     const struct as7343_config *cfg = dev->config;
//     struct as7343_data *data = dev->data;
//     int ret;

//     /* Wait for 3 integration cycles (to fill autorun buffer) */
//     for (int cycle = 0; cycle < 3; cycle++) {
//         ret = as7343_wait_ready(dev);
//         if (ret < 0) return ret;

//         uint8_t dummy;
//         i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &dummy); // clear AVALID
//     }

//     /* DEBUG: read all 18 raw registers */
//     for (int i = 0; i < 18; i++) {
//         uint8_t lo=0, hi=0;
//         uint8_t reg = as7343_data_lsb[i];

//         i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, reg, &lo);
//         i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, reg+1, &hi);

//         uint16_t v = ((uint16_t)hi << 8) | lo;
//         data->channel_data[i] = v;
//         data->wavelengths[i]  = wavelength_map[i];

//         // LOG_INF("Ch%02d @0x%02X λ=%d nm -> %u counts",
//         //         i, reg, data->wavelengths[i], v);
//     }

//     return 0;
// }

// static int as7343_channel_get(const struct device *dev,
//                               enum sensor_channel chan,
//                               struct sensor_value *val) {
//     struct as7343_data *data = dev->data;

//     if (chan >= SENSOR_CHAN_PRIV_START) {
//         int idx = chan - SENSOR_CHAN_PRIV_START;
//         if (idx >= 0 && idx < AS7343_NUM_CHANNELS) {
//             val->val1 = data->wavelengths[idx];     // λ from struct
//             val->val2 = data->channel_data[idx];    // counts
//             return 0;
//         }
//     }
//     return -ENOTSUP;
// }

// static const struct sensor_driver_api as7343_driver_api = {
//     .sample_fetch = as7343_sample_fetch,
//     .channel_get = as7343_channel_get,
// };

// /* -------------------------------------------------------------------------- */
// /* Spectrum helper                                                            */

// int as7343_get_spectrum(const struct device *dev,
//                         struct as7343_point spectrum[AS7343_NUM_CHANNELS]) {
//     struct as7343_data *data = dev->data;
//     int ret = as7343_sample_fetch(dev, SENSOR_CHAN_ALL);
    
//     if (ret < 0) {
//         return ret;
//     }

//     for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
//         spectrum[i].wavelength_nm = data->wavelengths[i];
//         spectrum[i].value = data->channel_data[i];
//     }
//     return 0;
// }

// static int as7343_init(const struct device *dev) {

//     const struct as7343_config *cfg = dev->config;
//     int ret;

//     if (!device_is_ready(cfg->i2c.bus)) {
//         LOG_ERR("I2C bus not ready");
//         return -ENODEV;
//     }

//     /* Step 1: Power ON only */
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x01);
//     if (ret < 0) {
//         return ret;
//     }
//     /* Step 2: ATIME, ASTEP, AGAIN */
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x81, 0x00); // ATIME
//     if (ret < 0) {
//         return ret;
//     }
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD4, 0xE7); // ASTEP LSB
//     if (ret < 0) {
//         return ret;
//     }
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD5, 0x03); // ASTEP MSB
//     if (ret < 0) {
//         return ret;
//     }
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xC6, 0x2A); // AGAIN=128x
//     if (ret < 0) {
//         return ret;
//     }

//     /* Step 3: Switch to bank 1 */
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x01);
//     if (ret < 0) {
//         return ret;
//     }

//     /* Step 4: Set CFG20 for autorun=3 (18 channels) */
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD6, 0x60);
//     if (ret < 0) {
//         return ret;
//     }

//     /* Step 5: Back to bank 0 */
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x00);
//     if (ret < 0) {
//         return ret;
//     }

//     i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x11);
//     k_msleep(5); // wait for SMUXEN bit to auto-clear
//     /* Step 6: Enable ALS (PON+ALS_EN) */
//     ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x03);
//     if (ret < 0) {
//         return ret;
//     }

//     LOG_INF("AS7343 init: 18-channel autorun, ATIME=0, ASTEP=999, GAIN=128x");
//     return 0;
// }

// #define AS7343_DEFINE(inst)                                           \
//     static struct as7343_data as7343_data_##inst;                     \
//     static const struct as7343_config as7343_config_##inst = {        \
//         .i2c = I2C_DT_SPEC_INST_GET(inst)                             \
//     };                                                                \
//     DEVICE_DT_INST_DEFINE(inst, as7343_init, NULL,                    \
//                           &as7343_data_##inst, &as7343_config_##inst, \
//                           POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,   \
//                           &as7343_driver_api);

// DT_INST_FOREACH_STATUS_OKAY(AS7343_DEFINE)
