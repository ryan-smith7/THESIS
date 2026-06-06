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
#include "ds18b20_direct.h"
#include "sensor.h"
#include "time_sync.h"
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/i2c.h>

static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

LOG_MODULE_REGISTER(sensor_module, LOG_LEVEL_INF);

#define SAMPLE_PERIOD_MS 10000

/* ── Moisture Sensor Polynomial Calibration ────────────────────────────────
 * θ_g (%) = a2·V² + a1·V + a0
 * where V = raw ADC count (0–4095)
 * Coefficients from moisture_calibration.xlsx → Coefficients sheet
 * ───────────────────────────────────────────────────────────────────────── */
#define CONFIG_MOISTURE_DRY_ADC       2371       /* clamp floor — above = 0% */
#define CONFIG_MOISTURE_COEFF_A3  -0.0000001095f
#define CONFIG_MOISTURE_COEFF_A2  0.00051806f
#define CONFIG_MOISTURE_COEFF_A1  -0.83401290f
#define CONFIG_MOISTURE_COEFF_A0  532.626437f

/* ── Moisture Sensor Two Point Calibration -- */
#define CONFIG_MOISTURE_DRY_COUNTS  2371
#define CONFIG_MOISTURE_WET_COUNTS  1053

#define SAMPLE_PERIOD_ENV_MS            500U
#define SAMPLE_PERIOD_SPECTRUM_MS       250U
#define SAMPLE_PERIOD_MOISTURE_MS       5000U
#define SAMPLE_PERIOD_BATTERY_MS        60000U
#define SAMPLE_PERIOD_SOIL_MS       5000U
#define LOW_BATT_SLEEP_MV  3000

static const struct adc_dt_spec moisture_adc =
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/*  item_size, depth, align */
K_MSGQ_DEFINE(bme_q,      sizeof(struct bme280_msg),    Q_DEPTH, 4);
K_MSGQ_DEFINE(ens_q,      sizeof(struct ens160_msg),    Q_DEPTH, 4);
K_MSGQ_DEFINE(as7_q,      sizeof(struct as7343_msg),    Q_DEPTH, 4);
K_MSGQ_DEFINE(batt_q,     sizeof(struct batt_msg),      Q_DEPTH, 4);
K_MSGQ_DEFINE(moisture_q, sizeof(struct moisture_msg),  Q_DEPTH, 4);
K_MSGQ_DEFINE(ds18b20_q, sizeof(struct ds18b20_msg), Q_DEPTH, 4);

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

        struct bme280_msg m = {0};  /* zero entire struct first */
        m.temp_c    = sensor_value_to_double(&temp);
        m.rh_pct    = sensor_value_to_double(&hum);
        m.press_hPa = sensor_value_to_double(&press);
        m.utc_sec = time_sync_get_utc_ms(&m.utc_ms);
        m.uptime_ms = (uint64_t)k_uptime_get();

        printk("BME put: T=%.2fC RH=%.2f%% P=%.3f(kPa?) UTC=%u.%03u\n",
            m.temp_c, m.rh_pct, m.press_hPa, m.utc_sec, m.utc_ms);

        if (k_msgq_put(&bme_q, &m, K_NO_WAIT) != 0) {
            struct bme280_msg dump;
            (void)k_msgq_get(&bme_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&bme_q, &m, K_NO_WAIT);
        }
        k_msleep(SAMPLE_PERIOD_ENV_MS);
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

        struct ens160_msg m = {0};  /* zero entire struct first */
        m.eco2_ppm = eco2.val1;
        m.tvoc_ppb = tvoc.val1;
        m.aqi      = aqi.val1;
        m.utc_sec = time_sync_get_utc_ms(&m.utc_ms);
        m.uptime_ms = (uint64_t)k_uptime_get();

        printk("ENS put: eCO2=%d TVOC=%d AQI=%d UTC=%u.%03u\n",
            m.eco2_ppm, m.tvoc_ppb, m.aqi, m.utc_sec, m.utc_ms);

        if (k_msgq_put(&ens_q, &m, K_NO_WAIT) != 0) {
            struct ens160_msg dump;
            (void)k_msgq_get(&ens_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&ens_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_ENV_MS);
    }
}

/* ── combined BME280 + ENS160 thread ───────────────────────────────────── */
void env_thread(void) {
    const struct device *bme_dev = DEVICE_DT_GET_ONE(bosch_bme280);
    const struct device *ens_dev = DEVICE_DT_GET_ONE(sciosense_ens160);

    if (!device_is_ready(bme_dev)) {
        LOG_ERR("BME280 not ready");
        return;
    }
    if (!device_is_ready(ens_dev)) {
        LOG_ERR("ENS160 not ready");
        return;
    }
    LOG_INF("ENV thread: BME280 + ENS160 ready");

    struct sensor_value temp, press, hum, eco2, tvoc, aqi;

    while (1) {
        /* ── 1. Fetch BME280 ───────────────────────────────────────────── */
        if (sensor_sample_fetch(bme_dev) < 0) {
            LOG_ERR("BME280: fetch failed");
            k_msleep(SAMPLE_PERIOD_ENV_MS);
            continue;
        }

        sensor_channel_get(bme_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
        sensor_channel_get(bme_dev, SENSOR_CHAN_PRESS,        &press);
        sensor_channel_get(bme_dev, SENSOR_CHAN_HUMIDITY,     &hum);

        struct bme280_msg bme = {0};
        bme.temp_c    = sensor_value_to_double(&temp);
        bme.rh_pct    = sensor_value_to_double(&hum);
        bme.press_hPa = sensor_value_to_double(&press);
        bme.utc_sec   = time_sync_get_utc_ms(&bme.utc_ms);
        bme.uptime_ms = (uint64_t)k_uptime_get();

        /* ── 2. Push compensation to ENS160 ────────────────────────────── */
        /*
         * sensor_attr_set() writes TEMP_IN (0x13) and RH_IN (0x15).
         * The Zephyr ENS160 driver converts internally:
         *   TEMP_IN = (T_celsius + 273.15) * 64   [1/64 K, little-endian]
         *   RH_IN   = RH_percent * 512             [1/512 %RH, little-endian]
         * We must write BEFORE sensor_sample_fetch(ens_dev) so the ENS160
         * bakes these values into the very next measurement cycle.
         */
        struct sensor_value temp_clamped = hum;
        temp_clamped.val1 = CLAMP(temp_clamped.val1, -5, 60);

        struct sensor_value hum_clamped = hum;
        hum_clamped.val1 = CLAMP(hum_clamped.val1, 20, 80);

        if (sensor_attr_set(ens_dev, SENSOR_CHAN_ALL,
                            SENSOR_ATTR_ENS160_TEMP, &temp_clamped) < 0) {
            LOG_WRN("ENS160: TEMP_IN write failed — uncompensated");
        }
        if (sensor_attr_set(ens_dev, SENSOR_CHAN_ALL,
                            SENSOR_ATTR_ENS160_RH, &hum_clamped) < 0) {
            LOG_WRN("ENS160: RH_IN write failed — uncompensated");
        }

        /* ── 3. Fetch ENS160 (now uses the T/RH we just wrote) ─────────── */
        if (sensor_sample_fetch(ens_dev) < 0) {
            LOG_ERR("ENS160: fetch failed");
            k_msleep(SAMPLE_PERIOD_ENV_MS);
            continue;
        }

        sensor_channel_get(ens_dev, SENSOR_CHAN_CO2,          &eco2);
        sensor_channel_get(ens_dev, SENSOR_CHAN_VOC,          &tvoc);
        sensor_channel_get(ens_dev, SENSOR_CHAN_ENS160_AQI,   &aqi);

        struct ens160_msg ens = {0};
        ens.eco2_ppm  = eco2.val1;
        ens.tvoc_ppb  = tvoc.val1;
        ens.aqi       = aqi.val1;
        ens.utc_sec   = time_sync_get_utc_ms(&ens.utc_ms);
        ens.uptime_ms = (uint64_t)k_uptime_get();

        /* ── 4. Publish BME280 message ──────────────────────────────────── */

        printk("BME: T=%.2fC RH=%.2f%% P=%.3fhPa UTC=%u.%03u\n",
               bme.temp_c, bme.rh_pct, bme.press_hPa,
               bme.utc_sec, bme.utc_ms);

        if (k_msgq_put(&bme_q, &bme, K_NO_WAIT) != 0) {
            struct bme280_msg dump;
            k_msgq_get(&bme_q, &dump, K_NO_WAIT);
            k_msgq_put(&bme_q, &bme, K_NO_WAIT);
        }

        /* ── 5. Publish ENS160 message ──────────────────────────────────── */

        printk("ENS: eCO2=%d TVOC=%d AQI=%d UTC=%u.%03u (comp T=%.1fC RH=%.1f%%)\n",
               ens.eco2_ppm, ens.tvoc_ppb, ens.aqi,
               ens.utc_sec, ens.utc_ms,
               sensor_value_to_double(&temp),
               sensor_value_to_double(&hum));

        if (k_msgq_put(&ens_q, &ens, K_NO_WAIT) != 0) {
            struct ens160_msg dump;
            k_msgq_get(&ens_q, &dump, K_NO_WAIT);
            k_msgq_put(&ens_q, &ens, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_ENV_MS);
    }
}

/* -------------------------------------------------------------------------- */
/* AS7343 thread                                                               */
/* -------------------------------------------------------------------------- */
 
static const int wl_order[13] = {
    405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855, 999
};
 
static int wl_index(int nm) {
    
    for (int i = 0; i < 13; ++i) {
        if (wl_order[i] == nm) return i;
    }
    return -1;
}
 
void as7343_thread(void) {

    k_msleep(10000);
    const struct device *dev = DEVICE_DT_GET_ONE(ams_as7343);

        // In your sensor init, after AS7343 readiness check fails:
    if (!device_is_ready(dev)) {
        LOG_ERR("AS7343 device not ready");
        int rc = i2c_recover_bus(i2c_dev);
        if (rc) {
            LOG_WRN("I2C bus recovery failed (%d)", rc);
        }
    }

    LOG_INF("AS7343 device ready");
 
    /* Dark offset calibration — sensor must be covered at this point.        */
    /* Averages 10 samples in darkness to establish per-channel BasicCounts   */
    /* offset. Call before any light measurement. Safe to skip in testing     */
    /* (dark_calibrated remains false, Step 2 is bypassed automatically).     */
    if (as7343_calibrate_dark(dev) < 0) {
        LOG_WRN("AS7343: dark calibration failed — proceeding without offset");
    }
 
    while (1) {
 
        if (sensor_sample_fetch(dev) < 0) {
            LOG_ERR("AS7343: fetch failed");
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }
 
        uint32_t ch12[13] = {0};
        struct sensor_value val;
 
        /* val.val1 = wavelength nm  (0=flicker, 999=VIS clear, else spectral) */
        /* val.val2 = irradiance in µW/m²  (mW/m² * 1000, int32)              */
        /* wl_index() maps wavelength → ch12[] slot (0..11 spectral, 12=VIS)  */
        for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
            if (sensor_channel_get(dev, SENSOR_CHAN_PRIV_START + i, &val) == 0) {
                int nm  = val.val1;
                int idx = wl_index(nm);
                if (idx >= 0 && idx < 12) {
                    ch12[idx] = (uint32_t)val.val2;
                } else {
                    LOG_DBG("AS7343: skip nm=%d (chan=%d)", nm, i);
                }
            }
        }
 
        /* ch12[12] = VIS broadband from dedicated clear PD (DATA4/10/16 avg) */
        /* Uses Fig.9 R_typ=999 responsivity, not a sum of spectral channels.  */
        const struct as7343_data *drv =
            (const struct as7343_data *)dev->data;
        ch12[12] = (uint32_t)(drv->vis_irradiance_mW * 1000.0f);
 
        /* Pack into message ------------------------------------------------- */
        struct as7343_msg msg = {0};
        for (int i = 0; i < 13; ++i) {
            msg.ch[i] = ch12[i];
        }
        msg.utc_sec   = time_sync_get_utc_ms(&msg.utc_ms);
        msg.uptime_ms = (uint64_t)k_uptime_get();
 
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
#if defined(CONFIG_MST_POLY)
/* Polynomial calibration: θ_g (%) = a3·V³ + a2·V² + a1·V + a0
 * Input:  raw 12-bit ADC count (0–4095)
 * Output: gravimetric moisture x100 fixed-point (e.g. 4250 = 42.50%)
 * Clamped to [0, 10000] (0–100%)                                      */
static uint16_t compute_moisture_x100_poly(uint16_t raw) {

    if (raw >= CONFIG_MOISTURE_DRY_ADC) {
        return 0;
    }
    float v   = (float)raw;
    float pct = (CONFIG_MOISTURE_COEFF_A3 * v * v * v)
              + (CONFIG_MOISTURE_COEFF_A2 * v * v)
              + (CONFIG_MOISTURE_COEFF_A1 * v)
              +  CONFIG_MOISTURE_COEFF_A0;

    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    return (uint16_t)(pct * 100.0f);
}
#else 
static uint16_t compute_vwc_x100_two_point(uint16_t raw) {

    if (raw >= CONFIG_MOISTURE_DRY_COUNTS) {
        return 0;
    }
    if (raw <= CONFIG_MOISTURE_WET_COUNTS) {
        return 10000;
    }

    uint32_t num = (uint32_t)(CONFIG_MOISTURE_DRY_COUNTS - raw) * 10000U;
    return (uint16_t)(num / (CONFIG_MOISTURE_DRY_COUNTS - CONFIG_MOISTURE_WET_COUNTS));
}
#endif

void moisture_thread(void) {

    if (!adc_is_ready_dt(&moisture_adc)) {
        LOG_ERR("Moisture: ADC not ready");
        return;
    }

    if (adc_channel_setup_dt(&moisture_adc) < 0) {
        LOG_ERR("Moisture: channel setup failed");
        return;
    }

    LOG_INF("Moisture: ADC ready");

    uint16_t buf;
    struct adc_sequence seq = {
        .buffer      = &buf,
        .buffer_size = sizeof(buf),
    };

    while (1) {
        (void)adc_sequence_init_dt(&moisture_adc, &seq);

        if (adc_read_dt(&moisture_adc, &seq) < 0) {
            LOG_ERR("Moisture: read failed");
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        int32_t val_mv = (int32_t)buf;
        adc_raw_to_millivolts_dt(&moisture_adc, &val_mv);

        LOG_INF("Moisture: raw=%u  mV=%d", buf, (int)val_mv);

        // uint16_t vwc = compute_vwc_x100_two_point(val_mv);

        // /* Use raw count for polynomial — calibrated against ADC counts, not mV */
        uint16_t vwc = compute_moisture_x100_poly(buf);

        struct moisture_msg m = {0};
        m.vwc_x100 = vwc;
        m.utc_sec = time_sync_get_utc_ms(&m.utc_ms);
        m.uptime_ms = (uint64_t)k_uptime_get();

        printk("MOIST put: raw=%u  mV=%d  VWC=%.2f%%  UTC=%u.%03u\n",
            buf, (int)val_mv, (double)vwc / 100.0,
            m.utc_sec, m.utc_ms);

        if (k_msgq_put(&moisture_q, &m, K_NO_WAIT) != 0) {
            struct moisture_msg dump;
            (void)k_msgq_get(&moisture_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&moisture_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MOISTURE_MS);
    }
}

/* --- DS18B20 thread --- */
void ds18b20_thread(void)
{
    int ret = ds18b20_direct_init();
    if (ret != 0) {
        LOG_ERR("DS18B20 direct init failed: %d", ret);
        return;
    }
    LOG_INF("DS18B20 direct driver ready");

    while (1) {
        struct sensor_value temp;

        ret = ds18b20_direct_read_sensor_value(&temp);
        if (ret != 0) {
            LOG_ERR("DS18B20: read failed: %d", ret);
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        struct ds18b20_msg m = {0};
        m.temp_val1  = temp.val1;
        m.temp_val2  = temp.val2;
        m.utc_sec    = time_sync_get_utc_ms(&m.utc_ms);
        m.uptime_ms  = (uint64_t)k_uptime_get();

        printk("DS18B20 put: Temp=%d.%06d °C UTC=%u.%03u\n",
               m.temp_val1, m.temp_val2, m.utc_sec, m.utc_ms);

        if (k_msgq_put(&ds18b20_q, &m, K_NO_WAIT) != 0) {
            struct ds18b20_msg dump;
            (void)k_msgq_get(&ds18b20_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&ds18b20_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_SOIL_MS);   /* 30000ms — soil temp changes slowly */
    }
}

#if defined(CONFIG_FUEL_GAUGE)

void max17048_thread(void) {
    uint8_t low_count = 0;
    const struct device *dev = DEVICE_DT_GET_ONE(maxim_max17048);

    if (!device_is_ready(dev)) {
        LOG_ERR("MAX17048 fuel gauge not ready");
        return;
    }

    /* Quick-start: forces MAX17048 to re-measure OCV and re-estimate SoC.
     * Write 0x4000 to COMMAND register (0x06) via raw I2C.
     * Required after battery swap or first boot with new pack. */
    const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    uint8_t qs_buf[3] = { 0x06, 0x40, 0x00 };

    int qsr = i2c_write(i2c, qs_buf, sizeof(qs_buf), 0x36);
    if (qsr != 0) {
        LOG_WRN("MAX17048 quick-start failed (%d) — SoC may be inaccurate", qsr);
    } else {
        LOG_INF("MAX17048 quick-start triggered — settling 175ms");
        k_msleep(175); /* one ADC conversion cycle */
    }

    LOG_INF("MAX17048 ready");

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

        /* Firmware low-voltage cutoff */
        if (m.mV < LOW_BATT_SLEEP_MV) {
            low_count++;
        } else {
            low_count = 0;
        }

        if (low_count >= 3) {
            LOG_WRN("Battery below threshold: %u mV", m.mV);

            printk("LOW BATTERY — ENTERING DEEP SLEEP\n");

            sys_poweroff();
            CODE_UNREACHABLE;
        }

        if (k_msgq_put(&batt_q, &m, K_NO_WAIT) != 0) {
            struct batt_msg dump;
            (void)k_msgq_get(&batt_q, &dump, K_NO_WAIT);
            (void)k_msgq_put(&batt_q, &m, K_NO_WAIT);
        }

        k_msleep(SAMPLE_PERIOD_MS);
    }
}
#endif