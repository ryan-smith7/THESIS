/**
 * @file my_json.c
 * @brief Per-modality JSON encoders for gateway → Azure IoT Hub.
 *
 * utc_sec + utc_ms: millisecond-precision UTC timestamp stamped on the
 * sensor node at measurement time via time_sync_get_utc_ms().
 * The Azure Function reconstructs: timestamp = utc_sec + utc_ms/1000.0
 * utc_sec is 0 if unsynced — Azure Function falls back to datetime.utcnow().
 */

#include "my_json.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(json_module, LOG_LEVEL_INF);

/* ── json_encode_bme ─────────────────────────────────────── */
int json_encode_bme(const mod_bme_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;

    return snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"bme280\":{"
            "\"temperature_c\":%.2f,"
            "\"humidity_percent\":%.2f,"
            "\"pressure_hpa\":%.3f"
          "}"
        "}",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        (double)m->temp_c_x100     / 100.0,
        (double)m->rh_x100         / 100.0,
        (double)m->press_hPa_x1000 / 1000.0
    );
}

/* ── json_encode_ens ─────────────────────────────────────── */
int json_encode_ens(const mod_ens_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;

    return snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"ens160\":{"
            "\"eco2_ppm\":%u,"
            "\"tvoc_ppb\":%u,"
            "\"aqi\":%u"
          "}"
        "}",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        (unsigned)m->eco2_ppm,
        (unsigned)m->tvoc_ppb,
        (unsigned)m->aqi
    );
}

/* ── json_encode_spec ────────────────────────────────────── */
int json_encode_spec(const mod_spec_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;

    return snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"spectrum\":{"
            "\"AS7343_405nm\":%u,"
            "\"AS7343_425nm\":%u,"
            "\"AS7343_450nm\":%u,"
            "\"AS7343_475nm\":%u,"
            "\"AS7343_515nm\":%u,"
            "\"AS7343_550nm\":%u,"
            "\"AS7343_555nm\":%u,"
            "\"AS7343_600nm\":%u,"
            "\"AS7343_640nm\":%u,"
            "\"AS7343_690nm\":%u,"
            "\"AS7343_745nm\":%u,"
            "\"AS7343_855nm\":%u,"
            "\"AS7343_VISIBLE\":%u"
          "}"
        "}",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        (unsigned)m->ch[0],
        (unsigned)m->ch[1],
        (unsigned)m->ch[2],
        (unsigned)m->ch[3],
        (unsigned)m->ch[4],
        (unsigned)m->ch[5],
        (unsigned)m->ch[6],
        (unsigned)m->ch[7],
        (unsigned)m->ch[8],
        (unsigned)m->ch[9],
        (unsigned)m->ch[10],
        (unsigned)m->ch[11],
        (unsigned)m->ch[12]
    );
}

/* ── json_encode_mst ─────────────────────────────────────── */
int json_encode_mst(const mod_mst_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;

    return snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"soil\":{"
            "\"vwc_percent\":%.2f"
          "}"
        "}",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        (double)m->vwc_x100 / 100.0
    );
}

/* ── json_encode_bat ─────────────────────────────────────── */
int json_encode_bat(const mod_bat_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;

    return snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"battery\":{"
            "\"mV\":%u,"
            "\"pct\":%u,"
            "\"rate_pct_hr\":%.1f"
          "}"
        "}",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        (unsigned)m->mV,
        (unsigned)m->pct,
        (double)m->rate_x10 / 10.0
    );
}

int json_encode_snd(const mod_snd_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;


    int pos = snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"sound\":{"
            "\"rms_dbfs\":%.2f,"
            "\"bins\":[",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        ((double)m->rms_dbfs_x100 / 100.0) + 120
    );

    LOG_INF("bin[0] raw=%u decoded=%.2f", m->bins[0],
        (double)m->bins[0] / 100.0); //NOT -120 AS IS 94db at -26dbfs therefore +120 gets to db spl
    

    if (pos <= 0 || (size_t)pos >= buf_size) return -ENOMEM;

    for (int i = 0; i < SOUND_NUM_BINS; i++) {
        double db = (double)m->bins[i] / 100.0;
        int w = snprintk(buf + pos, buf_size - pos,
                         (i < SOUND_NUM_BINS - 1) ? "%.2f," : "%.2f",
                         db);
        if (w <= 0 || (size_t)(pos + w) >= buf_size) {
            LOG_ERR("Sound JSON overflow at bin %d", i);
            return -ENOMEM;
        }
        pos += w;
    }

    int tail = snprintk(buf + pos, buf_size - pos, "]}}");
    if (tail <= 0) return -ENOMEM;
    return pos + tail;
}

int json_encode_cur(const mod_cur_t *m, char *buf, size_t buf_size) {
    if (!m || !buf) return -EINVAL;
 
    return snprintk(buf, buf_size,
        "{"
          "\"deviceId\":\"dev-%u\","
          "\"utc_sec\":%u,"
          "\"utc_ms\":%u,"
          "\"current\":{"
            "\"current_mA\":%.3f,"
            "\"voltage_mV\":%u"
          "}"
        "}",
        (unsigned)m->dev_id,
        (unsigned)m->utc_sec,
        (unsigned)m->utc_ms,
        (double)m->current_uA / 1000.0,
        (unsigned)m->voltage_mV
    );
}