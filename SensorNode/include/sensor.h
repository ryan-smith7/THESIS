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
#define SAMP_THREAD_PRIORITY 6

#define OFF 0
#define ON 1

#if defined(CONFIG_SENSOR_NODE_1)
#define DEVICE_ID 1
#else
#define DEVICE_ID 2
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
struct bme280_msg { double temp_c, rh_pct, press_hPa; uint32_t utc_sec; uint16_t utc_ms; };
struct ens160_msg { int eco2_ppm, tvoc_ppb, aqi;       uint32_t utc_sec; uint16_t utc_ms; };
struct as7343_msg { uint16_t ch[AS7343_NUM_CH];         uint32_t utc_sec; uint16_t utc_ms; };
struct batt_msg   { uint16_t mV; uint8_t pct; int16_t rate_x10; uint32_t utc_sec; uint16_t utc_ms; };
struct moisture_msg { uint16_t vwc_x100;                uint32_t utc_sec; uint16_t utc_ms; };

/* -------- Message queues -------- */
#define Q_DEPTH 8

extern struct k_msgq bme_q;
extern struct k_msgq ens_q;
extern struct k_msgq as7_q;
extern struct k_msgq batt_q;
extern struct k_msgq full_q;
extern struct k_msgq sound_q;
extern struct k_msgq moisture_q;

extern void set_logging_state(uint8_t new_value);
extern void set_logging_file(char* file_abs_path);

extern void bme280_thread(void);
extern void ens160_thread(void);
extern void as7343_thread(void);
extern void max17048_thread(void);
extern void combiner_thread(void);
extern void moisture_thread(void);
extern void sensor_control_thread(void);

#endif /* SENSOR_H */