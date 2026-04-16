#define DT_DRV_COMPAT ams_as7343

#include "as7343.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(AS7343, CONFIG_SENSOR_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/* Wavelength mapping (nm) per channel index                                  */

static const uint16_t wavelength_map[18] = {
    450,   // DATA0 = FZ
    555,   // DATA1 = FY
    600,   // DATA2 = FXL
    855,   // DATA3 = NIR
    999,     // DATA4 = VIS clear (TL)
    0,     // DATA5 = Flicker detect
    425,   // DATA6 = F2
    475,   // DATA7 = F3
    515,   // DATA8 = F4
    640,   // DATA9 = F6
    0,     // DATA10 = VIS clear (BR)
    0,     // DATA11 = Flicker detect
    405,   // DATA12 = F1
    690,   // DATA13 = F7
    745,   // DATA14 = F8
    550,   // DATA15 = F5
    0,     // DATA16 = VIS clear (RB)
    0      // DATA17 = Flicker detect
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

static int as7343_wait_ready(const struct device *dev) {
    const struct as7343_config *cfg = dev->config;
    uint8_t st2;
    for (int i = 0; i < 400; i++) {             // ~2 s @ 5 ms
        if (i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &st2) < 0)
            return -EIO;
        if (st2 & BIT(6))                       // AVALID (STATUS2 bit6)
            return 0;
        k_msleep(5);
    }
    return -ETIMEDOUT;
}


static int as7343_sample_fetch(const struct device *dev,
                               enum sensor_channel chan) {
    ARG_UNUSED(chan);
    const struct as7343_config *cfg = dev->config;
    struct as7343_data *data = dev->data;
    int ret;

    /* Wait for 3 integration cycles (to fill autorun buffer) */
    for (int cycle = 0; cycle < 3; cycle++) {
        ret = as7343_wait_ready(dev);
        if (ret < 0) return ret;

        uint8_t dummy;
        i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, 0x90, &dummy); // clear AVALID
    }

    /* DEBUG: read all 18 raw registers */
    for (int i = 0; i < 18; i++) {
        uint8_t lo=0, hi=0;
        uint8_t reg = as7343_data_lsb[i];

        i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, reg, &lo);
        i2c_reg_read_byte(cfg->i2c.bus, cfg->i2c.addr, reg+1, &hi);

        uint16_t v = ((uint16_t)hi << 8) | lo;
        data->channel_data[i] = v;
        data->wavelengths[i]  = wavelength_map[i];

        // LOG_INF("Ch%02d @0x%02X λ=%d nm -> %u counts",
        //         i, reg, data->wavelengths[i], v);
    }

    return 0;
}

static int as7343_channel_get(const struct device *dev,
                              enum sensor_channel chan,
                              struct sensor_value *val) {
    struct as7343_data *data = dev->data;

    if (chan >= SENSOR_CHAN_PRIV_START) {
        int idx = chan - SENSOR_CHAN_PRIV_START;
        if (idx >= 0 && idx < AS7343_NUM_CHANNELS) {
            val->val1 = data->wavelengths[idx];     // λ from struct
            val->val2 = data->channel_data[idx];    // counts
            return 0;
        }
    }
    return -ENOTSUP;
}

static const struct sensor_driver_api as7343_driver_api = {
    .sample_fetch = as7343_sample_fetch,
    .channel_get = as7343_channel_get,
};

/* -------------------------------------------------------------------------- */
/* Spectrum helper                                                            */

int as7343_get_spectrum(const struct device *dev,
                        struct as7343_point spectrum[AS7343_NUM_CHANNELS]) {
    struct as7343_data *data = dev->data;
    int ret = as7343_sample_fetch(dev, SENSOR_CHAN_ALL);
    
    if (ret < 0) {
        return ret;
    }

    for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
        spectrum[i].wavelength_nm = data->wavelengths[i];
        spectrum[i].value = data->channel_data[i];
    }
    return 0;
}

static int as7343_init(const struct device *dev) {

    const struct as7343_config *cfg = dev->config;
    int ret;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    /* Step 1: Power ON only */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x01);
    if (ret < 0) {
        return ret;
    }
    /* Step 2: ATIME, ASTEP, AGAIN */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x81, 0x00); // ATIME
    if (ret < 0) {
        return ret;
    }
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD4, 0xE7); // ASTEP LSB
    if (ret < 0) {
        return ret;
    }
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD5, 0x03); // ASTEP MSB
    if (ret < 0) {
        return ret;
    }
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xC6, 0x2A); // AGAIN=128x
    if (ret < 0) {
        return ret;
    }

    /* Step 3: Switch to bank 1 */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x01);
    if (ret < 0) {
        return ret;
    }

    /* Step 4: Set CFG20 for autorun=3 (18 channels) */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xD6, 0x60);
    if (ret < 0) {
        return ret;
    }

    /* Step 5: Back to bank 0 */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0xBF, 0x00);
    if (ret < 0) {
        return ret;
    }

    i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x11);
    k_msleep(5); // wait for SMUXEN bit to auto-clear
    /* Step 6: Enable ALS (PON+ALS_EN) */
    ret = i2c_reg_write_byte(cfg->i2c.bus, cfg->i2c.addr, 0x80, 0x03);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("AS7343 init: 18-channel autorun, ATIME=0, ASTEP=999, GAIN=128x");
    return 0;
}

#define AS7343_DEFINE(inst)                                           \
    static struct as7343_data as7343_data_##inst;                     \
    static const struct as7343_config as7343_config_##inst = {        \
        .i2c = I2C_DT_SPEC_INST_GET(inst)                             \
    };                                                                \
    DEVICE_DT_INST_DEFINE(inst, as7343_init, NULL,                    \
                          &as7343_data_##inst, &as7343_config_##inst, \
                          POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,   \
                          &as7343_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AS7343_DEFINE)
