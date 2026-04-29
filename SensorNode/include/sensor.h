/**
 * @file s4744413_sensor.c
 * @brief sensor library header file
 */

#ifndef SENSOR_H
#define SENSOR_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

// Define Thread Macros
#define STACK_SIZE_SENSOR 2048

#define SENSOR_PRIORITY 2
#define HTS221_THREAD_PRIORITY 3
#define LPS22HB_THREAD_PRIORITY 4
#define LIST3MDL_THREAD_PRIORITY 5
#define BME280_THREAD_PRIORITY 3
#define ENV_THREAD_PRIORITY 3
#define SAMP_THREAD_PRIORITY 6

#if defined(CONFIG_SENSOR_NODE_1)
#define DEVICE_ID 1
#elif defined(CONFIG_SENSOR_NODE_2)
#define DEVICE_ID 2
#else 
#define DEVICE_ID 3
#endif

// sensor.h
#pragma once
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#define AS7343_NUM_CH 13  // [405,425,450,475,515,550,555,600,640,690,745,855, VISIBLE]

struct sensor_blk {
    uint32_t time;
    uint16_t time_ms;
    uint32_t uptime_ms;
    uint8_t  proto_ver;
    uint8_t  dev_id;
    int16_t  temp_c_x100;
    int16_t  rh_x100;
    int32_t  press_hPa_x1000;
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;
    uint16_t as7343[AS7343_NUM_CH];
    uint16_t batt_mV;
    uint8_t  batt_pct;
    int16_t  batt_rate_x10;
    int16_t  snd_rms_dbfs_x100;
    uint16_t snd_peak_freq_hz;
    uint16_t snd_peak_mag_x10;
    uint16_t soil_vwc_x100;
};

/* -------- Partial messages (from each sensor thread) --------
 *
 * utc_sec + utc_ms: UTC timestamp stamped at measurement moment via
 * time_sync_get_utc_ms(). utc_sec is 0 and utc_ms is 0 if no time
 * sync has occurred yet. Both fields are sent over BLE so the gateway
 * can forward millisecond-precision timestamps to Azure.
 */
struct bme280_msg {
    double   temp_c, rh_pct, press_hPa; /* offset 0,  size 24 */
    uint32_t utc_sec;                    /* offset 24, size 4  */
    uint16_t utc_ms;                     /* offset 28, size 2  */
    uint16_t _pad;                       /* offset 30, size 2  */
    uint64_t uptime_ms;                  /* offset 32, size 8 ✓ */
};                                       /* total: 40 bytes */

struct ens160_msg {
    int      eco2_ppm, tvoc_ppb, aqi;   /* offset 0,  size 12 */
    uint32_t utc_sec;                    /* offset 12, size 4  */
    uint16_t utc_ms;                     /* offset 16, size 2  */
    uint16_t _pad;                       /* offset 18, size 2  */
    uint32_t _pad2;                      /* offset 20, size 4  */
    uint64_t uptime_ms;                  /* offset 24, size 8 ✓ */
};                                       /* total: 32 bytes */

struct as7343_msg {
    uint32_t ch[AS7343_NUM_CH];  /* offset 0, size 52 bytes (13 × 4) */
    uint16_t utc_ms;             /* offset 52, size 2                  */
    uint16_t _pad;               /* offset 54, size 2                  */
    uint32_t utc_sec;            /* offset 56, size 4                  */
    uint32_t _pad2;              /* offset 60, size 4                  */
    uint64_t uptime_ms;          /* offset 64, size 8                  */
};                               /* total: 72 bytes                    */

struct batt_msg {
    uint16_t mV;                         /* offset 0,  size 2  */
    uint8_t  pct;                        /* offset 2,  size 1  */
    uint8_t  _pad0;                      /* offset 3,  size 1  */
    int16_t  rate_x10;                   /* offset 4,  size 2  */
    uint16_t _pad1;                      /* offset 6,  size 2  */
    uint32_t utc_sec;                    /* offset 8,  size 4  */
    uint16_t utc_ms;                     /* offset 12, size 2  */
    uint16_t _pad2;                      /* offset 14, size 2  */
    uint64_t uptime_ms;                  /* offset 16, size 8 ✓ */
};                                       /* total: 24 bytes */

struct moisture_msg {
    uint16_t vwc_x100;                   /* offset 0,  size 2  */
    uint16_t utc_ms;                     /* offset 2,  size 2  */
    uint32_t utc_sec;                    /* offset 4,  size 4  */
    uint64_t uptime_ms;                  /* offset 8,  size 8 ✓ */
};                                       /* total: 16 bytes, no padding needed */

/* DS18B20 soil temperature message */
struct ds18b20_msg {
    int32_t  temp_val1;                  /* offset 0,  size 4  — integer °C */
    int32_t  temp_val2;                  /* offset 4,  size 4  — fractional millionths */
    uint32_t utc_sec;                    /* offset 8,  size 4  */
    uint16_t utc_ms;                     /* offset 12, size 2  */
    uint16_t _pad;                       /* offset 14, size 2  */
    uint64_t uptime_ms;                  /* offset 16, size 8  */
};                                       /* total: 24 bytes    */


struct current_msg {
    int16_t  current_uA;                 /* offset 0,  size 2  */
    uint16_t voltage_mV;                 /* offset 2,  size 2  */
    uint32_t utc_sec;                    /* offset 4,  size 4  */
    uint16_t utc_ms;                     /* offset 8,  size 2  */
    uint16_t _pad;                       /* offset 10, size 2  */
    uint32_t _pad2;                      /* offset 12, size 4  */
    uint64_t uptime_ms;                  /* offset 16, size 8 ✓ */
};       

/* -------- Message queues -------- */
#define Q_DEPTH 8

extern struct k_msgq bme_q;
extern struct k_msgq ens_q;
extern struct k_msgq as7_q;
extern struct k_msgq batt_q;
extern struct k_msgq full_q;
extern struct k_msgq moisture_q;
extern struct k_msgq current_q;  //CURRENT ADDITION
extern struct k_msgq ds18b20_q;  //soil temp ADDITION


extern void set_logging_state(uint8_t new_value);
extern void set_logging_file(char* file_abs_path);

extern void bme280_thread(void);
extern void ens160_thread(void);
extern void as7343_thread(void);
extern void env_thread(void);
extern void max17048_thread(void);
extern void moisture_thread(void);
extern void sensor_control_thread(void);
extern void current_thread(void); //CURRENT ADDITION
extern void ds18b20_thread(void);

#endif /* SENSOR_H */