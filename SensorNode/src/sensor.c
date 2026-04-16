#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/ens160.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/fuel_gauge.h>
#include "as7343.h"
#include "sound.h"
#include "sensor.h"
#include "time_sync.h"

#include <zephyr/drivers/adc.h>

LOG_MODULE_REGISTER(sensor_module, LOG_LEVEL_INF);

#define SAMPLE_PERIOD_MS 10000

#define CONFIG_MOISTURE_DRY_COUNTS  2900
#define CONFIG_MOISTURE_WET_COUNTS  1200

#define MOISTURE_ADC_SPEC \
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0)

/*  item_size, depth, align */
K_MSGQ_DEFINE(bme_q,      sizeof(struct bme280_msg),    Q_DEPTH, 4);
K_MSGQ_DEFINE(ens_q,      sizeof(struct ens160_msg),    Q_DEPTH, 4);
K_MSGQ_DEFINE(as7_q,      sizeof(struct as7343_msg),    Q_DEPTH, 4);
K_MSGQ_DEFINE(batt_q,     sizeof(struct batt_msg),      Q_DEPTH, 4);
K_MSGQ_DEFINE(moisture_q, sizeof(struct moisture_msg),  Q_DEPTH, 4);

/* Final telemetry queue: full samples -> BLE */
K_MSGQ_DEFINE(full_q, sizeof(struct sensor_blk), Q_DEPTH, 4);

/* -------------------------------------------------------------------------- */
/* --- BME280 thread --- */
void bme280_thread(void) {

    const struct device *dev = DEVICE_DT_GET_ONE(bosch_bme280);

    if (!device_is_ready(dev)) {
        LOG_ERR("BME280 device not ready");
        return;
    }

    struct sensor_value temp, press, hum;

    while (1) {
        if (sensor_sample_fetch(dev) < 0) {
            LOG_ERR("BME280: fetch failed");
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
        sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);
        sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);

        uint16_t bme_utc_ms;
        uint32_t bme_utc_sec = time_sync_get_utc_ms(&bme_utc_ms);

        struct bme280_msg m = {
            .temp_c    = sensor_value_to_double(&temp),
            .rh_pct    = sensor_value_to_double(&hum),
            .press_hPa = sensor_value_to_double(&press),
            .utc_sec   = bme_utc_sec,
            .utc_ms    = bme_utc_ms,
        };

        printk("BME put: T=%.2fC RH=%.2f%% P=%.3f(kPa?) UTC=%u.%03u\n",
            m.temp_c, m.rh_pct, m.press_hPa, m.utc_sec, m.utc_ms);

        if (k_msgq_put(&bme_q, &m, K_NO_WAIT) != 0) {
            struct bme280_msg dump;
            (void)k_msgq_get(&bme_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&bme_q, &m, K_NO_WAIT);
        }
        k_msleep(SAMPLE_PERIOD_MS);
    }
}

/* -------------------------------------------------------------------------- */
/* --- ENS160 thread --- */
void ens160_thread(void) {

    const struct device *dev = DEVICE_DT_GET_ONE(sciosense_ens160);

    if (!device_is_ready(dev)) {
        LOG_ERR("ENS160 device not ready");
        return;
    }
    LOG_INF("ENS160 device ready");

    struct sensor_value eco2, tvoc, aqi;

    while (1) {

        if (sensor_sample_fetch(dev) < 0) {
            LOG_ERR("ENS160: fetch failed");
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        sensor_channel_get(dev, SENSOR_CHAN_CO2, &eco2);
        sensor_channel_get(dev, SENSOR_CHAN_VOC, &tvoc);
        sensor_channel_get(dev, SENSOR_CHAN_ENS160_AQI, &aqi);

        uint16_t ens_utc_ms;
        uint32_t ens_utc_sec = time_sync_get_utc_ms(&ens_utc_ms);

        struct ens160_msg m = {
            .eco2_ppm = eco2.val1,
            .tvoc_ppb = tvoc.val1,
            .aqi      = aqi.val1,
            .utc_sec  = ens_utc_sec,
            .utc_ms   = ens_utc_ms,
        };

        printk("ENS put: eCO2=%d TVOC=%d AQI=%d UTC=%u.%03u\n",
            m.eco2_ppm, m.tvoc_ppb, m.aqi, m.utc_sec, m.utc_ms);

        if (k_msgq_put(&ens_q, &m, K_NO_WAIT) != 0) {
            struct ens160_msg dump;
            (void)k_msgq_get(&ens_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&ens_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MS);
    }
}

/* -------------------------------------------------------------------------- */
/* --- AS7343 thread -------------------------------------------------------- */

static const int wl_order[13] = {
    405,425,450,475,515,550,555,600,640,690,745,855,999
};

static int wl_index(int nm) {

    for (int i = 0; i < 13; ++i) {
        if (wl_order[i] == nm) return i;
    }
    return -1;
}

void as7343_thread(void) {
    const struct device *dev = DEVICE_DT_GET_ONE(ams_as7343);

    if (!device_is_ready(dev)) {
        LOG_ERR("AS7343 device not ready");
        return;
    }

    LOG_INF("AS7343 device ready");

    while (1) {

        if (sensor_sample_fetch(dev) < 0) {
            LOG_ERR("AS7343: fetch failed");
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        uint16_t ch12[13] = {0};
        struct sensor_value val;

        for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
            if (sensor_channel_get(dev, SENSOR_CHAN_PRIV_START + i, &val) == 0) {
                int nm  = val.val1;
                int idx = wl_index(nm);
                if (idx >= 0) {
                    int c = val.val2;
                    if (c < 0)      c = 0;
                    if (c > 0xFFFF) c = 0xFFFF;
                    ch12[idx] = (uint16_t)c;
                } else {
                    LOG_DBG("AS7343: skip unknown nm=%d (chan=%d)", nm, i);
                }
            }
        }

        struct as7343_msg msg = {0};
        uint32_t vis = 0;
        for (int i = 0; i < 12; ++i) {
            msg.ch[i] = ch12[i];
            vis += ch12[i];
        }
        if (vis > 0xFFFF) vis = 0xFFFF;
        msg.ch[12] = (uint16_t)vis;

        /* Stamp UTC at measurement moment */
        msg.utc_sec = time_sync_get_utc_ms(&msg.utc_ms);

        printk("AS7 put: 450nm=%u 600nm=%u VIS=%u UTC=%u.%03u\n",
               msg.ch[2], msg.ch[7], msg.ch[12], msg.utc_sec, msg.utc_ms);

        if (k_msgq_put(&as7_q, &msg, K_NO_WAIT) != 0) {
            struct as7343_msg dump;
            (void)k_msgq_get(&as7_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&as7_q, &msg, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MS);
    }
}

/* -------------------------------------------------------------------------- */
/* --- Capacitive Soil Moisture Sensor thread ------------------------------- */

static uint16_t compute_vwc_x100(uint16_t raw) {

    if (raw >= CONFIG_MOISTURE_DRY_COUNTS) return 0;
    if (raw <= CONFIG_MOISTURE_WET_COUNTS) return 10000;

    uint32_t num = (uint32_t)(CONFIG_MOISTURE_DRY_COUNTS - raw) * 10000U;
    return (uint16_t)(num / (CONFIG_MOISTURE_DRY_COUNTS - CONFIG_MOISTURE_WET_COUNTS));
}

void moisture_thread(void) {

    const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

    if (!device_is_ready(adc_dev)) {
        LOG_ERR("Moisture: ADC device not ready");
        return;
    }

    struct adc_channel_cfg ch_cfg = {
        .gain             = ADC_GAIN_1,
        .reference        = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        .channel_id       = 7,
    };

    if (adc_channel_setup(adc_dev, &ch_cfg) < 0) {
        LOG_ERR("Moisture: ADC channel setup failed");
        return;
    }

    LOG_INF("Moisture: ADC ready");

    int16_t sample_buf;
    struct adc_sequence seq = {
        .channels    = BIT(7),
        .buffer      = &sample_buf,
        .buffer_size = sizeof(sample_buf),
        .resolution  = 12,
    };

    while (1) {
        sample_buf = 0;
        if (adc_read(adc_dev, &seq) < 0) {
            LOG_ERR("Moisture: ADC read failed");
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        uint16_t raw = (uint16_t)CLAMP(sample_buf, 0, 4095);
        LOG_INF("Moisture: raw=%u", raw);
        uint16_t vwc = compute_vwc_x100(raw);

        uint16_t mst_utc_ms;
        uint32_t mst_utc_sec = time_sync_get_utc_ms(&mst_utc_ms);

        struct moisture_msg m = {
            .vwc_x100 = vwc,
            .utc_sec  = mst_utc_sec,
            .utc_ms   = mst_utc_ms,
        };

        printk("MOIST put: raw=%u  VWC=%.2f%%  UTC=%u.%03u\n",
            raw, (double)vwc / 100.0, m.utc_sec, m.utc_ms);

        if (k_msgq_put(&moisture_q, &m, K_NO_WAIT) != 0) {
            struct moisture_msg dump;
            (void)k_msgq_get(&moisture_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&moisture_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MS);
    }
}

#if defined(CONFIG_FUEL_GAUGE)
/* -------------------------------------------------------------------------- */
/* --- MAX17048 Fuel Gauge thread (commented — enable when hardware ready) -- */
void max17048_thread(void) {

    const struct device *dev = DEVICE_DT_GET_ONE(maxim_max17048);

    if (!device_is_ready(dev)) {
        LOG_ERR("MAX17048 fuel gauge not ready");
        return;
    }
    LOG_INF("MAX17048 fuel gauge ready");

    while (1) {
        union fuel_gauge_prop_val voltage, soc, tte, ttf;

        int rv  = fuel_gauge_get_prop(dev, FUEL_GAUGE_VOLTAGE,                  &voltage);
        int rs  = fuel_gauge_get_prop(dev, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &soc);
        int rte = fuel_gauge_get_prop(dev, FUEL_GAUGE_RUNTIME_TO_EMPTY,         &tte);
        int rtf = fuel_gauge_get_prop(dev, FUEL_GAUGE_RUNTIME_TO_FULL,          &ttf);

        if (rv != 0 || rs != 0) {
            LOG_ERR("MAX17048: read failed (rv=%d rs=%d)", rv, rs);
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        int16_t rate_x10 = 0;
        if (rtf == 0 && ttf.runtime_to_full > 0)        rate_x10 = +10;
        else if (rte == 0 && tte.runtime_to_empty > 0)  rate_x10 = -10;

        uint16_t bat_utc_ms;
        uint32_t bat_utc_sec = time_sync_get_utc_ms(&bat_utc_ms);

        struct batt_msg m = {
            .mV       = (uint16_t)(voltage.voltage / 1000),
            .pct      = (uint8_t)CLAMP(soc.relative_state_of_charge, 0, 100),
            .rate_x10 = rate_x10,
            .utc_sec  = bat_utc_sec,
            .utc_ms   = bat_utc_ms,
        };

        printk("BATT: %u mV  %u%%  %s  UTC=%u.%03u\n",
               m.mV, m.pct,
               rate_x10 > 0 ? "charging" : (rate_x10 < 0 ? "discharging" : "unknown"),
               m.utc_sec, m.utc_ms);

        if (k_msgq_put(&batt_q, &m, K_NO_WAIT) != 0) {
            struct batt_msg dump;
            (void)k_msgq_get(&batt_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&batt_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MS);
    }
}
#endif

/* -------------------------------------------------------------------------- */
/* --- Combiner thread (LEGACY) --------------------------------------------- */
/* Retains latest partials and emits a full frame once per tick             */
void combiner_thread(void) {
    struct bme280_msg   bme   = {0};
    struct ens160_msg   ens   = {0};
    struct as7343_msg   as7   = {0};
    struct batt_msg     bat   = {.mV = 0};
    struct sound_msg    snd   = {0};
    struct moisture_msg moist = {0};

    bool have_bme=false, have_ens=false, have_as7=false, have_bat=false;

    const uint8_t PROTO_VER = 1;
    const uint8_t DEV_ID    = DEVICE_ID;

    while (1) {
        if (k_msgq_get(&bme_q,      &bme,   K_NO_WAIT) == 0) have_bme = true;
        if (k_msgq_get(&ens_q,      &ens,   K_NO_WAIT) == 0) have_ens = true;
        if (k_msgq_get(&as7_q,      &as7,   K_NO_WAIT) == 0) have_as7 = true;
        if (k_msgq_get(&batt_q,     &bat,   K_NO_WAIT) == 0) have_bat = true;
        (void)k_msgq_get(&sound_q,    &snd,   K_NO_WAIT);
        (void)k_msgq_get(&moisture_q, &moist, K_NO_WAIT);

        struct sensor_blk s = {0};
        uint16_t blk_utc_ms;
        s.time      = time_sync_get_utc_ms(&blk_utc_ms);
        s.time_ms   = blk_utc_ms;
        s.uptime_ms = k_uptime_get_32();
        s.proto_ver = PROTO_VER;
        s.dev_id    = DEV_ID;

        s.temp_c_x100     = (int16_t)(bme.temp_c    * 100.0);
        s.rh_x100         = (int16_t)(bme.rh_pct    * 100.0);
        s.press_hPa_x1000 = (int32_t)(bme.press_hPa * 1000.0);

        s.eco2_ppm = (uint16_t)ens.eco2_ppm;
        s.tvoc_ppb = (uint16_t)ens.tvoc_ppb;
        s.aqi      = (uint8_t) ens.aqi;

        for (int i = 0; i < 12; ++i) {
            s.as7343[i] = as7.ch[i];
        }

        s.batt_mV       = bat.mV;
        s.batt_pct      = bat.pct;
        s.batt_rate_x10 = bat.rate_x10;

        s.snd_rms_dbfs_x100 = snd.rms_dbfs_x100;
        s.snd_peak_freq_hz  = snd.peak_freq_hz;
        s.snd_peak_mag_x10  = snd.peak_mag_x10;

        s.soil_vwc_x100 = moist.vwc_x100;

        if (k_msgq_put(&full_q, &s, K_NO_WAIT) != 0) {
            struct sensor_blk dump;
            (void)k_msgq_get(&full_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&full_q, &s, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MS);
    }
}