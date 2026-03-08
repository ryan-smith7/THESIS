    #include <zephyr/kernel.h>
    #include <zephyr/device.h>
    #include <zephyr/drivers/sensor.h>
    #include <zephyr/logging/log.h>
    #include <zephyr/data/json.h>
    #include <zephyr/sys/ring_buffer.h>
    #include <zephyr/sys/printk.h>
    #include <zephyr/drivers/sensor.h>
    #include <zephyr/drivers/sensor/ens160.h>
    #include "as7343.h"
    #include "sensor.h"

    #include "i2c_gate.h"

    LOG_MODULE_REGISTER(sensor_module, LOG_LEVEL_INF);

    #define RING_BUF_SIZE 256
    #define MAX_JSON_SIZE 512
    #define SAMPLE_PERIOD_MS 10000

    /* --- Sensor types --- */
    #define DEV_ID_BME280  1
    #define DEV_ID_ENS160  2
    #define DEV_ID_AS7343  3

    /*  item_size, depth, align */
    K_MSGQ_DEFINE(bme_q,  sizeof(struct bme280_msg), Q_DEPTH, 4);
    K_MSGQ_DEFINE(ens_q,  sizeof(struct ens160_msg), Q_DEPTH, 4);
    K_MSGQ_DEFINE(as7_q,  sizeof(struct as7343_msg), Q_DEPTH, 4);
    K_MSGQ_DEFINE(batt_q, sizeof(struct batt_msg),   Q_DEPTH, 4);

    /* Final telemetry queue: full samples -> BLE */
    K_MSGQ_DEFINE(full_q, sizeof(struct sensor_blk), Q_DEPTH, 4);

    /* --- Ring buffer + structs --- */
    struct sensor_data {
        double value1;
        double value2;
        double value3;
        uint8_t sensor_type;
    };

    struct json_sensor_output {
        uint8_t dev_id;
        char *rtc_time;
        char *values[20];
        size_t values_len;
    };

    static const struct json_obj_descr json_descr[] = {
        JSON_OBJ_DESCR_PRIM(struct json_sensor_output, dev_id, JSON_TOK_NUMBER),
        JSON_OBJ_DESCR_PRIM(struct json_sensor_output, rtc_time, JSON_TOK_STRING),
        JSON_OBJ_DESCR_ARRAY(struct json_sensor_output, values, 20, values_len, JSON_TOK_STRING),
    };

    /* --- Dummy timestamp --- */
    static char *get_rtc_time(void)
    {
        static char buf[32];
        uint64_t ms = k_uptime_get();
        //in milleseconds currently
        snprintf(buf, sizeof(buf), "%llu", ms);
        return buf;
    }

    /* -------------------------------------------------------------------------- */
    /* --- BME280 thread --- */
    void bme280_thread(void)
    {   
        const struct device *dev = DEVICE_DT_GET_ONE(bosch_bme280);
        if (!device_is_ready(dev)) {
            LOG_ERR("BME280 device not ready");
            return;
        }

        struct sensor_value temp, press, hum;
        // struct json_sensor_output json_out = {0};

        while (1) {

            // i2c_gate_acquire();
            if (sensor_sample_fetch(dev) < 0) {
                LOG_ERR("BME280: fetch failed");
                // i2c_gate_release();
                k_msleep(SAMPLE_PERIOD_MS);
                continue;
            }
            //  i2c_gate_release();

            // json_out.dev_id = DEV_ID_BME280;
            // json_out.rtc_time = get_rtc_time();
            // json_out.values_len = 0;

            // static char temp_str[32], hum_str[32], press_str[32];
            // if (sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp) == 0) {
            //     snprintf(temp_str, sizeof(temp_str), "%.2f", sensor_value_to_double(&temp));
            //     json_out.values[json_out.values_len++] = temp_str;
            // }
            // if (sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press) == 0) {
            //     snprintf(press_str, sizeof(press_str), "%.2f", sensor_value_to_double(&press));
            //     json_out.values[json_out.values_len++] = press_str;
            // }
            // if (sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum) == 0) {
            //     snprintf(hum_str, sizeof(hum_str), "%.2f", sensor_value_to_double(&hum));
            //     json_out.values[json_out.values_len++] = hum_str;
            // }
            sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
            sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);
            sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);

            // static char json_buf[MAX_JSON_SIZE];
            // int ret = json_obj_encode_buf(json_descr, ARRAY_SIZE(json_descr), &json_out,
            //                               json_buf, sizeof(json_buf));
            // if (ret < 0) {
            //     LOG_ERR("BME280 JSON encode error: %d", ret);
            // } else {
            //     printk("%s\n", json_buf);
            // }

            // k_msleep(SAMPLE_PERIOD_MS);

            struct bme280_msg m = {
                .temp_c   = sensor_value_to_double(&temp),
                .rh_pct   = sensor_value_to_double(&hum),
                .press_hPa= sensor_value_to_double(&press),
            };

            printk("BME put: T=%.2fC RH=%.2f%% P=%.3f(kPa?)\n",
                m.temp_c, m.rh_pct, m.press_hPa);
            /* Drop oldest if full to keep freshest */
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
    void ens160_thread(void)
    {   
        const struct device *dev = DEVICE_DT_GET_ONE(sciosense_ens160);
        if (!device_is_ready(dev)) {
            LOG_ERR("ENS160 device not ready");
            return;
        }
        LOG_INF("ENS160 device ready");

        struct sensor_value eco2, tvoc, aqi;
        // struct json_sensor_output json_out = {0};

        while (1) {
            // i2c_gate_acquire();
            if (sensor_sample_fetch(dev) < 0) {
                LOG_ERR("ENS160: fetch failed");
                // i2c_gate_release();
                k_msleep(SAMPLE_PERIOD_MS);
                continue;
            }
            // i2c_gate_release();

            // json_out.dev_id = DEV_ID_ENS160;
            // json_out.rtc_time = get_rtc_time();
            // json_out.values_len = 0;

            // static char eco2_str[32], tvoc_str[32], aqi_str[32];
            // if (sensor_channel_get(dev, SENSOR_CHAN_CO2, &eco2) == 0) {
            //     snprintf(eco2_str, sizeof(eco2_str), "%d", eco2.val1);
            //     json_out.values[json_out.values_len++] = eco2_str;
            // }
            // if (sensor_channel_get(dev, SENSOR_CHAN_VOC, &tvoc) == 0) {
            //     snprintf(tvoc_str, sizeof(tvoc_str), "%d", tvoc.val1);
            //     json_out.values[json_out.values_len++] = tvoc_str;
            // }
            // if (sensor_channel_get(dev, SENSOR_CHAN_ENS160_AQI, &aqi) == 0) {
            //     snprintf(aqi_str, sizeof(aqi_str), "%d", aqi.val1);
            //     json_out.values[json_out.values_len++] = aqi_str;
            // }
            sensor_channel_get(dev, SENSOR_CHAN_CO2, &eco2);
            sensor_channel_get(dev, SENSOR_CHAN_VOC, &tvoc);
            sensor_channel_get(dev, SENSOR_CHAN_ENS160_AQI, &aqi);

            // static char json_buf[MAX_JSON_SIZE];
            // int ret = json_obj_encode_buf(json_descr, ARRAY_SIZE(json_descr), &json_out,
            //                               json_buf, sizeof(json_buf));
            // if (ret < 0) {
            //     LOG_ERR("ENS160 JSON encode error: %d", ret);
            // } else {
            //     printk("%s\n", json_buf);
            // }

            struct ens160_msg m = { .eco2_ppm = eco2.val1, .tvoc_ppb = tvoc.val1, .aqi = aqi.val1 };
            printk("ENS put: eCO2=%d TVOC=%d AQI=%d\n", m.eco2_ppm, m.tvoc_ppb, m.aqi);
            if (k_msgq_put(&ens_q, &m, K_NO_WAIT) != 0) {
                struct ens160_msg dump;
                (void)k_msgq_get(&ens_q, &dump, K_NO_WAIT);
                (void)k_msgq_put(&ens_q, &m, K_NO_WAIT);
            }

            k_msleep(SAMPLE_PERIOD_MS);
        }
    }

    /* Fixed order we want downstream (index 12 = 999 "VISIBLE") */
    static const int wl_order[13] = {
        405,425,450,475,515,550,555,600,640,690,745,855,999
    };

    static int wl_index(int nm)
    {
        /* exact match first (covers 999 visible) */
        for (int i = 0; i < 13; ++i) {
            if (wl_order[i] == nm) return i;
        }
        return -1;
    }

    /* -------------------------------------------------------------------------- */
    /* --- AS7343 thread (aligned -> queue, includes 999nm sum) ----------------- */
    void as7343_thread(void)
    {
        const struct device *dev = DEVICE_DT_GET_ONE(ams_as7343);
        if (!device_is_ready(dev)) {
            LOG_ERR("AS7343 device not ready");
            return;
        }
        LOG_INF("AS7343 device ready");

        while (1) {
            // i2c_gate_acquire();
            if (sensor_sample_fetch(dev) < 0) {
                LOG_ERR("AS7343: fetch failed");
                // i2c_gate_release();
                k_msleep(SAMPLE_PERIOD_MS);
                continue;
            }
            // i2c_gate_release();

            uint16_t ch12[13] = {0};
            struct sensor_value val;

            for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
                if (sensor_channel_get(dev, SENSOR_CHAN_PRIV_START + i, &val) == 0) {
                    int nm  = val.val1;      // wavelength in nm
                    int idx = wl_index(nm);  // index in wl_order
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

            // Build message: 12 bands + VISIBLE=999nm sum at index 12
            struct as7343_msg msg = {0};
            uint32_t vis = 0;
            for (int i = 0; i < 12; ++i) {
                msg.ch[i] = ch12[i];
                vis += ch12[i];
            }
            if (vis > 0xFFFF) vis = 0xFFFF;
            msg.ch[12] = (uint16_t)vis; // index 12 == 999nm summary

            printk("AS7 put: 450nm=%u 600nm=%u VIS=%u\n", msg.ch[2], msg.ch[7], msg.ch[12]);
            // Push (drop-oldest to keep freshest)
            if (k_msgq_put(&as7_q, &msg, K_NO_WAIT) != 0) {
                struct as7343_msg dump;
                (void)k_msgq_get(&as7_q, &dump, K_NO_WAIT);
                (void)k_msgq_put(&as7_q, &msg, K_NO_WAIT);
            }

            k_msleep(SAMPLE_PERIOD_MS);
        }
    }

    // /* -------------------------------------------------------------------------- */
    // /* --- AS7343 thread --- */
    // void as7343_thread(void)
    // {
    //     const struct device *dev = DEVICE_DT_GET_ONE(ams_as7343);
    //     if (!device_is_ready(dev)) {
    //         LOG_ERR("AS7343 device not ready");
    //         return;
    //     }
    //     LOG_INF("AS7343 device ready");

    //     struct sensor_value val;
    //     struct json_sensor_output json_out = {0};

    //     while (1) {
    //         if (sensor_sample_fetch(dev) < 0) {
    //             LOG_ERR("AS7343: fetch failed");
    //             k_msleep(SAMPLE_PERIOD_MS);
    //             continue;
    //         }

    //         // json_out.dev_id = DEV_ID_AS7343;
    //         // json_out.rtc_time = get_rtc_time();
    //         // json_out.values_len = 0;

    //         static char chan_str[AS7343_NUM_CHANNELS][32];

    //         // for (int i = 0; i < AS7343_NUM_CHANNELS; i++) {
    //         //     if (sensor_channel_get(dev, SENSOR_CHAN_PRIV_START + i, &val) == 0) {
    //         //         if (val.val1 > 0) {
    //         //             snprintf(chan_str[i], sizeof(chan_str[i]), "λ=%dnm:%d", val.val1, val.val2);
    //         //             json_out.values[json_out.values_len++] = chan_str[i];
    //         //         }
    //         //     }
    //         // }

    //         static char json_buf[MAX_JSON_SIZE];
    //         int ret = json_obj_encode_buf(json_descr, ARRAY_SIZE(json_descr), &json_out,
    //                                       json_buf, sizeof(json_buf));
    //         if (ret < 0) {
    //             LOG_ERR("AS7343 JSON encode error: %d", ret);
    //         } else {
    //             printk("%s\n", json_buf);
    //         }

    //         k_msleep(SAMPLE_PERIOD_MS);
    //     }
    // }

    /* Combiner retains the latest partials and emits a full frame once per tick */
    void combiner_thread(void)
    {
        /* caches of last-seen values */
        struct bme280_msg bme = {0};
        struct ens160_msg ens = {0};
        struct as7343_msg as7 = {0};
        struct batt_msg   bat = {.mV = 0};

        bool have_bme=false, have_ens=false, have_as7=false, have_bat=false;

        const uint8_t PROTO_VER = 1;
        const uint8_t DEV_ID    = 2;

        while (1) {
            /* Non-blocking harvest of any new partials that arrived in the last cycle */
            if (k_msgq_get(&bme_q,  &bme, K_NO_WAIT) == 0) have_bme = true;
            if (k_msgq_get(&ens_q,  &ens, K_NO_WAIT) == 0) have_ens = true;
            if (k_msgq_get(&as7_q,  &as7, K_NO_WAIT) == 0) have_as7 = true;
            // (void)k_msgq_get(&batt_q, &bat, K_NO_WAIT)  == 0 ? (have_bat=true) : 0;

            /* Build a full sample every SAMPLE_PERIOD_MS (even if one partial is stale) */
            struct sensor_blk s = {0};
            s.time       = 0;                        // wall-clock unused
            s.uptime_ms  = k_uptime_get_32();
            s.proto_ver  = PROTO_VER;
            s.dev_id     = DEV_ID;

            /* BME280 (scale for BLE packer) */
            s.temp_c_x100   = (int16_t)(bme.temp_c   * 100.0);
            s.rh_x100       = (int16_t)(bme.rh_pct   * 100.0);
            s.press_hPa_x1000 = (int32_t)(bme.press_hPa * 1000.0);

            /* ENS160 */
            s.eco2_ppm = (uint16_t)ens.eco2_ppm;
            s.tvoc_ppb = (uint16_t)ens.tvoc_ppb;
            s.aqi      = (uint8_t) ens.aqi;


            for (int i = 0; i < 12; ++i) {
                s.as7343[i] = as7.ch[i];
            }

            /* Battery (optional) */
            // s.batt_mV = bat.mV;
            s.batt_mV = 100;

            /* Push the full sample (drop oldest if queue is full) */
            if (k_msgq_put(&full_q, &s, K_NO_WAIT) != 0) {
                struct sensor_blk dump;
                (void)k_msgq_get(&full_q, &dump, K_NO_WAIT);
                (void)k_msgq_put(&full_q, &s, K_NO_WAIT);
            }

            k_msleep(SAMPLE_PERIOD_MS);  // e.g., 1000 ms tick
        }
    }