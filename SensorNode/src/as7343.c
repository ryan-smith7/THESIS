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
    0x95, 0x97, 0x99, 0x9B, 0x9D, 0x9F,
    0xA1, 0xA3, 0xA5, 0xA7, 0xA9, 0xAB,
    0xAD, 0xAF, 0xB1, 0xB3, 0xB5, 0xB7
};

/* -------------------------------------------------------------------------- */
/* Gain table — ratios from datasheet Fig.10 relative to 64x.                 */
/* 0.5x-16x divided by 1000 per datasheet note (5).                           */

static const uint8_t gain_reg[AS7343_GAIN_STEPS] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C
};

static const float gain_ratio[AS7343_GAIN_STEPS] = {
    0.0079f, 0.0158f, 0.0316f, 0.064f, 0.124f, 0.247f,
    0.5f,    1.0f,    2.0f,    4.1f,   8.6f,   16.9f
};

/* -------------------------------------------------------------------------- */
/* Gain non-linearity correction factors                                       */
/* Source: AN001052 Fig.5, normalised to 128x = 1.0                           */

static const float gain_correction[AS7343_GAIN_STEPS] = {
    1.02f,  /* 0.5x  */
    1.02f,  /* 1x    */
    1.02f,  /* 2x    */
    1.03f,  /* 4x    */
    0.99f,  /* 8x    */
    1.00f,  /* 16x   */
    1.00f,  /* 32x   */
    1.01f,  /* 64x   */
    1.00f,  /* 128x  reference */
    1.02f,  /* 256x  */
    1.05f,  /* 512x  */
    1.02f,  /* 1024x */
};

/* -------------------------------------------------------------------------- */
/* Per-channel spectral correction factors                                     */
/* Source: AN001052 Fig.19, Golden Device vs spectrometer.                    */
/* Index: DATA0..DATA17 order. 0.0 = non-spectral, not applied.               */

static const float ch_correction[18] = {
    1.0296f,  /* DATA0  FZ  450nm  */
    0.9874f,  /* DATA1  FY  555nm  */
    0.9959f,  /* DATA2  FXL 600nm  */
    1.0522f,  /* DATA3  NIR 855nm  */
    0.0f,     /* DATA4  VIS clear  */
    0.0f,     /* DATA5  flicker    */
    1.0435f,  /* DATA6  F2  425nm  */
    1.0175f,  /* DATA7  F3  475nm  */
    1.0044f,  /* DATA8  F4  514nm  */
    1.0146f,  /* DATA9  F6  635nm  */
    0.0f,     /* DATA10 VIS clear  */
    0.0f,     /* DATA11 flicker    */
    1.0554f,  /* DATA12 F1  405nm  */
    0.9965f,  /* DATA13 F7  685nm  */
    0.9331f,  /* DATA14 F8  745nm — reduces out-of-band leakage anomaly */
    0.9576f,  /* DATA15 F5  547nm  */
    0.0f,     /* DATA16 VIS clear  */
    0.0f,     /* DATA17 flicker    */
};

/* -------------------------------------------------------------------------- */
/* Datasheet Fig.8 R_typ values                                               */
/* AGAIN=1024x, t_int=27.8ms, Ee=155 mW/m²                                   */

static const float r_typ[18] = {
    2169.0f, 3747.0f, 4776.0f, 10581.0f, 0.0f, 0.0f,
    1756.0f,  770.0f, 3141.0f,  3336.0f, 0.0f, 0.0f,
    5749.0f, 5435.0f,  864.0f,  1574.0f, 0.0f, 0.0f
};

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */

#define ATIME_VAL       63
#define ASTEP_LSB       0xFF
#define ASTEP_MSB       0x03
#define GAIN_START_IDX  8

#define CLIP_HI  62258U
#define CLIP_LO   3276U

#define REF_RATIO    16.9f
#define REF_TINT_MS  27.8f
#define REF_EE       155.0f
#define R_TYP_VIS    999.0f
#define DARK_SAMPLES 10

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
/* Set AGAIN                                                                   */

static int as7343_set_again(const struct device *dev, uint8_t idx)
{
    const struct as7343_config *cfg = dev->config;
    struct as7343_data *data = dev->data;
    if (idx >= AS7343_GAIN_STEPS) return -EINVAL;
    int ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr,
                                 0xC6, gain_reg[idx]);
    if (ret == 0) data->again_idx = idx;
    return ret;
}

/* -------------------------------------------------------------------------- */
/* Raw register read — 3 cycle flush then read all 18 channels               */

static int as7343_read_raw(const struct device *dev)
{
    const struct as7343_config *cfg = dev->config;
    struct as7343_data *data = dev->data;
    int ret;
    for (int cycle = 0; cycle < 3; cycle++) {
        ret = as7343_wait_ready(dev);
        if (ret < 0) return ret;
        uint8_t dummy;
        i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &dummy);
    }
    for (int i = 0; i < 18; i++) {
        uint8_t lo = 0, hi = 0;
        i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr,
                          as7343_data_lsb[i], &lo);
        i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr,
                          as7343_data_lsb[i] + 1, &hi);
        data->channel_data[i] = ((uint16_t)hi << 8) | lo;
        data->wavelengths[i]  = wavelength_map[i];
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Dark offset calibration                                                     */
/* Call once at startup with sensor fully covered (no light incident).        */
/* Computes BasicCounts offset per channel — remains valid across gain steps. */

int as7343_calibrate_dark(const struct device *dev)
{
    struct as7343_data *data = dev->data;
    int ret;
    float accum[18] = {0};

    LOG_INF("AS7343: dark calibration start — sensor must be covered");

    for (int s = 0; s < DARK_SAMPLES; s++) {
        ret = as7343_read_raw(dev);
        if (ret < 0) return ret;
        float gr = gain_ratio[data->again_idx];
        float ti = as7343_tint_ms();
        for (int i = 0; i < 18; i++) {
            accum[i] += (float)data->channel_data[i] / (gr * ti);
        }
    }
    for (int i = 0; i < 18; i++) {
        data->dark_basic[i] = accum[i] / (float)DARK_SAMPLES;
    }
    data->dark_calibrated = true;
    LOG_INF("AS7343: dark calibration complete");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* S_i_basic — per-channel sensitivity in BasicCounts per mW/m²              */
/*                                                                             */
/* r_typ[i] is the raw ADC count from datasheet Fig.8 at:                    */
/*   AGAIN=1024x (REF_RATIO=16.9), t_int=27.8ms (REF_TINT_MS), Ee=155mW/m²  */
/*                                                                             */
/* To express r_typ as a BasicCount (gain and time normalised):               */
/*   r_typ_basic[i] = r_typ[i] / (REF_RATIO * REF_TINT_MS)                  */
/*                                                                             */
/* S_i_basic = r_typ_basic[i] / REF_EE                                       */
/*           = r_typ[i] / (REF_EE * REF_RATIO * REF_TINT_MS)                */
/*           units: BasicCounts / (mW/m²)                                    */
/*                                                                             */
/* Dimensional proof that E_i = BC_corr / S_i_basic is consistent:           */
/*   BC_corr  has units: counts / (gain_ratio * tint_ms)  [BasicCounts]      */
/*   S_i_basic has units: BasicCounts / (mW/m²)                              */
/*   BC_corr / S_i_basic = mW/m²  ✓                                          */
/*                                                                             */
/* Both numerator (your measurement) and denominator (datasheet reference)    */
/* are in the same BasicCounts space — gain and tint fully cancel.            */
/*                                                                             */
/* Precomputed at compile time: s_i_basic[i] = r_typ[i] / (155 * 16.9 * 27.8) */
/* REF_EE * REF_RATIO * REF_TINT_MS = 155 * 16.9 * 27.8 = 72765.1           */

#define S_DENOM  (REF_EE * REF_RATIO * REF_TINT_MS)   /* 72765.1 */

static const float s_i_basic[18] = {
    2169.0f  / S_DENOM,   /* DATA0  FZ  450nm  = 0.02980 BC/(mW/m²) */
    3747.0f  / S_DENOM,   /* DATA1  FY  555nm  = 0.05149 BC/(mW/m²) */
    4776.0f  / S_DENOM,   /* DATA2  FXL 600nm  = 0.06563 BC/(mW/m²) */
    10581.0f / S_DENOM,   /* DATA3  NIR 855nm  = 0.14541 BC/(mW/m²) */
    0.0f,                 /* DATA4  VIS clear  — handled separately  */
    0.0f,                 /* DATA5  flicker    — excluded            */
    1756.0f  / S_DENOM,   /* DATA6  F2  425nm  = 0.02413 BC/(mW/m²) */
    770.0f   / S_DENOM,   /* DATA7  F3  475nm  = 0.01058 BC/(mW/m²) */
    3141.0f  / S_DENOM,   /* DATA8  F4  515nm  = 0.04316 BC/(mW/m²) */
    3336.0f  / S_DENOM,   /* DATA9  F6  640nm  = 0.04584 BC/(mW/m²) */
    0.0f,                 /* DATA10 VIS clear  — handled separately  */
    0.0f,                 /* DATA11 flicker    — excluded            */
    5749.0f  / S_DENOM,   /* DATA12 F1  405nm  = 0.07902 BC/(mW/m²) */
    5435.0f  / S_DENOM,   /* DATA13 F7  690nm  = 0.07470 BC/(mW/m²) */
    864.0f   / S_DENOM,   /* DATA14 F8  745nm  = 0.01187 BC/(mW/m²) */
    1574.0f  / S_DENOM,   /* DATA15 F5  550nm  = 0.02163 BC/(mW/m²) */
    0.0f,                 /* DATA16 VIS clear  — handled separately  */
    0.0f,                 /* DATA17 flicker    — excluded            */
};

/* VIS channel S_i_basic — Fig.9 R_typ=999, same reference conditions */
static const float s_vis_basic = R_TYP_VIS / S_DENOM;  /* = 0.01373 BC/(mW/m²) */

/* -------------------------------------------------------------------------- */
/* Calibration chain: raw counts → corrected irradiance (mW/m²)              */
/*                                                                             */
/* Step 1  BasicCounts  = raw / (gain_ratio * tint_ms)                        */
/*         Removes AGAIN and t_int dependency. Consistent across gain steps.  */
/*                                                                             */
/* Step 2  BC_offset    = BasicCounts - dark_basic[i]                         */
/*         Removes dark current, DC bias, ambient leakage floor.              */
/*                                                                             */
/* Step 3  BC_gain      = BC_offset * gain_correction[again_idx]              */
/*         Corrects ADC gain non-linearity (AN001052 Fig.5, <1% for <=512x).  */
/*                                                                             */
/* Step 4  BC_corr      = BC_gain * ch_correction[i]                          */
/*         Per-channel spectral correction from Golden Device (AN001052 Fig.19)*/
/*         Partially corrects out-of-band filter leakage, notably F8/745nm.  */
/*                                                                             */
/* Step 5  E_i [mW/m²]  = BC_corr / s_i_basic[i]                             */
/*         s_i_basic[i] = r_typ[i] / (REF_EE * REF_RATIO * REF_TINT_MS)     */
/*         Converts BasicCounts to irradiance — units cancel correctly.       */

static void as7343_compute_irradiance(struct as7343_data *data)
{
    float gr = gain_ratio[data->again_idx];
    float ti = as7343_tint_ms();
    float gc = gain_correction[data->again_idx];

    for (int i = 0; i < 18; i++) {
        if (s_i_basic[i] <= 0.0f) {
            data->basic_counts[i]  = 0.0f;
            data->irradiance_mW[i] = 0.0f;
            continue;
        }

        /* Step 1: BasicCounts = raw / (gain_ratio * tint_ms) */
        float bc = (float)data->channel_data[i] / (gr * ti);

        /* Step 2: subtract dark offset BasicCounts */
        if (data->dark_calibrated) {
            bc -= data->dark_basic[i];
            if (bc < 0.0f) bc = 0.0f;
        }

        /* Step 3: gain non-linearity correction */
        bc *= gc;

        /* Step 4: per-channel spectral correction */
        bc *= ch_correction[i];

        data->basic_counts[i] = bc;

        /* Step 5: E_i = BC_corr / s_i_basic[i]  →  mW/m² */
        data->irradiance_mW[i] = bc / s_i_basic[i];
        // data->irradiance_mW[i] = bc;

    }

    /* VIS broadband (DATA4/10/16) — Steps 1-3 only, s_vis_basic from Fig.9  */
    {
        float vis_sum = 0.0f;
        const int vis_idx[3] = {4, 10, 16};
        for (int j = 0; j < 3; j++) {
            float bc = (float)data->channel_data[vis_idx[j]] / (gr * ti);
            if (data->dark_calibrated) {
                bc -= data->dark_basic[vis_idx[j]];
                if (bc < 0.0f) bc = 0.0f;
            }
            bc *= gc;
            vis_sum += bc;
        }
        data->vis_irradiance_mW = (vis_sum / 3.0f) / s_vis_basic;
        // data->vis_irradiance_mW = (vis_sum / 3.0f);
        data->irradiance_mW[4]  = data->vis_irradiance_mW;
        data->irradiance_mW[10] = data->vis_irradiance_mW;
        data->irradiance_mW[16] = data->vis_irradiance_mW;
    }
}

/* -------------------------------------------------------------------------- */
/* sample_fetch                                                                */

static int as7343_sample_fetch(const struct device *dev,
                               enum sensor_channel chan)
{
    ARG_UNUSED(chan);
    struct as7343_data *data = dev->data;
    int ret;

    for (int attempt = 0; attempt < AS7343_GAIN_STEPS; attempt++) {
        ret = as7343_read_raw(dev);
        if (ret < 0) return ret;

        uint16_t peak = 0;
        for (int i = 0; i < 18; i++) {
            if (r_typ[i] > 0.0f && data->channel_data[i] > peak)
                peak = data->channel_data[i];
        }

        if (peak >= CLIP_HI && data->again_idx > 0) {
            LOG_INF("AS7343: peak=%u clipping, gain %u -> %u",
                    peak, data->again_idx, data->again_idx - 1);
            ret = as7343_set_again(dev, data->again_idx - 1);
            if (ret < 0) return ret;
            continue;
        }
        if (peak < CLIP_LO && data->again_idx < AS7343_GAIN_STEPS - 1) {
            LOG_INF("AS7343: peak=%u too low, gain %u -> %u",
                    peak, data->again_idx, data->again_idx + 1);
            ret = as7343_set_again(dev, data->again_idx + 1);
            if (ret < 0) return ret;
            continue;
        }

        as7343_compute_irradiance(data);
        return 0;
    }

    LOG_ERR("AS7343: auto-range failed after %d attempts", AS7343_GAIN_STEPS);
    return -ETIMEDOUT;
}

/* -------------------------------------------------------------------------- */
/* channel_get                                                                 */
/* val->val1 = wavelength nm                                                  */
/* val->val2 = irradiance in µW/m² (mW/m² * 1000, int32)                    */

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
/* Spectrum helper                                                             */

int as7343_get_spectrum(const struct device *dev,
                        struct as7343_point spectrum[AS7343_NUM_CHANNELS])
{
    struct as7343_data *data = dev->data;
    int ret = as7343_sample_fetch(dev, SENSOR_CHAN_ALL);
    if (ret < 0) return ret;
    for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
        spectrum[i].wavelength_nm = data->wavelengths[i];
        spectrum[i].value = (uint16_t)CLAMP(
                                (int32_t)data->irradiance_mW[i], 0, 0xFFFF);
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

    data->again_idx       = GAIN_START_IDX;
    data->dark_calibrated = false;

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x01);
    if (ret < 0) return ret;

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x81, ATIME_VAL);
    if (ret < 0) return ret;
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD4, ASTEP_LSB);
    if (ret < 0) return ret;
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD5, ASTEP_MSB);
    if (ret < 0) return ret;

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xC6,
                             gain_reg[GAIN_START_IDX]);
    if (ret < 0) return ret;

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x01);
    if (ret < 0) return ret;
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD6, 0x60);
    if (ret < 0) return ret;
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x00);
    if (ret < 0) return ret;

    i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x11);
    k_msleep(5);

    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x03);
    if (ret < 0) return ret;

    LOG_INF("AS7343 init OK — call as7343_calibrate_dark() with sensor covered");
    return 0;
}

/* -------------------------------------------------------------------------- */

#define AS7343_DEFINE(inst)                                            \
    static struct as7343_data as7343_data_##inst;                      \
    static const struct as7343_config as7343_config_##inst = {         \
        .i2c = I2C_DT_SPEC_INST_GET(inst)                             \
    };                                                                 \
    DEVICE_DT_INST_DEFINE(inst, as7343_init, NULL,                     \
                          &as7343_data_##inst, &as7343_config_##inst,  \
                          POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,    \
                          &as7343_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AS7343_DEFINE)