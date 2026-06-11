/**
 * @file my_json.h
 * @brief Per-modality JSON encoders for gateway → Oracle Cloud.
 */

#ifndef MY_JSON_H
#define MY_JSON_H

#include <stdint.h>
#include <stddef.h>

/* ── Buffer sizes ────────────────────────────────────────── */
#define JSON_ENS_BUF_SIZE   128
#define JSON_BME_BUF_SIZE   192
#define JSON_SPEC_BUF_SIZE   512
#define JSON_MST_BUF_SIZE    128
#define JSON_BAT_BUF_SIZE    128
#define JSON_SND_BUF_SIZE   3072
#define JSON_CUR_BUF_SIZE    128   /* current sensor                   */
#define JSON_DS18B20_BUF_SIZE  128

/* ── Channel / bin counts ────────────────────────────────── */
#define AS7343_NUM_CH  13
#define SOUND_NUM_BINS 348


/*
 * Raw decoded structs — populated by bluetooth.c notify
 * handlers directly from BLE payload bytes (big-endian).
 */

/** BME280 — temperature, humidity, pressure */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;
    uint16_t utc_ms;
    int16_t  temp_c_x100;        /* °C × 100          */
    int16_t  rh_x100;            /* %RH × 100         */
    int32_t  press_hPa_x1000;    /* hPa × 1000        */
} mod_bme_t;

/** ENS160 — VOC/eCO2/AQI with compensation values logged */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;
    uint16_t utc_ms;
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;
} mod_ens_t;

/** AS7343 spectrum */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;           /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;            /* UTC milliseconds 0-999                    */
    uint32_t ch[AS7343_NUM_CH]; /* µW/m² per channel — was uint16 raw counts */
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

/** Electrical current sensor */
typedef struct {
    uint8_t  dev_id;
    uint32_t utc_sec;        /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;         /* UTC milliseconds 0-999                    */
    int16_t  current_uA;     /* signed microamps, e.g. 1500 = 1.500 mA   */
    uint16_t voltage_mV;     /* millivolts, e.g. 3300 = 3.300 V          */
} mod_cur_t;

typedef struct {
    uint32_t utc_sec;
    uint16_t utc_ms;
    int16_t  temp_val1;   /* integer °C */
    int16_t  temp_val2;   /* centidegrees fractional — e.g. 62 = 0.62°C */
    uint8_t  dev_id;
} mod_ds18b20_t;

/* -------------------Encoders-------------------*/

int json_encode_bme(const mod_bme_t *m, char *buf, size_t buf_size);
int json_encode_ens(const mod_ens_t *m, char *buf, size_t buf_size);
int json_encode_spec(const mod_spec_t *m, char *buf, size_t buf_size);
int json_encode_mst(const mod_mst_t *m, char *buf, size_t buf_size);
int json_encode_bat(const mod_bat_t *m, char *buf, size_t buf_size);
int json_encode_snd(const mod_snd_t *m, char *buf, size_t buf_size);
int json_encode_cur(const mod_cur_t *m, char *buf, size_t buf_size);
int json_encode_ds18b20(const mod_ds18b20_t *m, char *buf, size_t buf_size);

#endif /* MY_JSON_H */