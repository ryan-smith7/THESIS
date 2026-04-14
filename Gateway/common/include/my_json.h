/**
 * @file my_json.h
 * @brief Per-modality JSON encoders for gateway → Azure IoT Hub.
 *
 * Each encoder writes directly into a caller-supplied buffer and returns
 * the number of bytes written (>0) or a negative error code.
 *
 * JSON format per modality:
 *   {
 *     "deviceId": "dev-N",
 *     "utc_sec": NNNN,        ← UTC seconds at measurement on sensor node
 *     "<modality>": { ... }
 *   }
 *
 * utc_sec is stamped by the sensor node at the moment of measurement via
 * time_sync_get_utc(). The Azure Function uses this value as the SQL
 * timestamp rather than datetime.utcnow(), giving accurate measurement
 * time even after SD card drain replay.
 *
 * Value is 0 if the sensor node has not yet received a time sync — the
 * Azure Function falls back to server time in that case.
 */

#ifndef MY_JSON_H
#define MY_JSON_H

#include <stdint.h>
#include <stddef.h>

/* ── Buffer sizes ────────────────────────────────────────── */
#define JSON_ENV_BUF_SIZE    256
#define JSON_SPEC_BUF_SIZE   512
#define JSON_MST_BUF_SIZE    128
#define JSON_BAT_BUF_SIZE    128
#define JSON_SND_BUF_SIZE   3072

/* ── Channel / bin counts ────────────────────────────────── */
#define AS7343_NUM_CH  13
#define SOUND_NUM_BINS 348

/* ═══════════════════════════════════════════════════════════
 * Raw decoded structs — populated by bluetooth.c notify
 * handlers directly from BLE payload bytes (big-endian).
 * ═══════════════════════════════════════════════════════════ */

/** BME280 + ENS160 environment */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;           /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;            /* UTC milliseconds 0-999                   */
    int16_t  temp_c_x100;
    int16_t  rh_x100;
    int32_t  press_hPa_x1000;
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;
} mod_env_t;

/** AS7343 spectrum */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;           /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;            /* UTC milliseconds 0-999                   */
    uint16_t ch[AS7343_NUM_CH];
} mod_spec_t;

/** Soil moisture */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;           /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;            /* UTC milliseconds 0-999                   */
    uint16_t vwc_x100;
} mod_mst_t;

/** Battery */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;           /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;            /* UTC milliseconds 0-999                   */
    uint16_t mV;
    uint8_t  pct;
    int16_t  rate_x10;
} mod_bat_t;

/** Sound summary */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;           /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;            /* UTC milliseconds 0-999                   */
    int16_t  rms_dbfs_x100;
    uint16_t peak_freq_hz;
    uint16_t peak_mag_x10;
    uint16_t bins[SOUND_NUM_BINS];
} mod_snd_t;

/* ═══════════════════════════════════════════════════════════
 * Encoders
 * ═══════════════════════════════════════════════════════════ */

int json_encode_env(const mod_env_t *m, char *buf, size_t buf_size);
int json_encode_spec(const mod_spec_t *m, char *buf, size_t buf_size);
int json_encode_mst(const mod_mst_t *m, char *buf, size_t buf_size);
int json_encode_bat(const mod_bat_t *m, char *buf, size_t buf_size);
int json_encode_snd(const mod_snd_t *m, char *buf, size_t buf_size);

#endif /* MY_JSON_H */