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
#define DEVICE_ID 2


// sensor.h
#pragma once
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#define AS7343_NUM_CH 13  // [405,425,450,475,515,550,555,600,640,690,745,855, VISIBLE]

// Final, full sample (matches your BLE packing layout)
// Packed binary size: 59 bytes (was 57 — added uint16_t time_ms)
struct sensor_blk {
    uint32_t time;          // UTC seconds since epoch (0 if not synced)
    uint16_t time_ms;       // UTC sub-second milliseconds (0–999)
    uint32_t uptime_ms;     // k_uptime_get_32()
    uint8_t  proto_ver;
    uint8_t  dev_id;

    // BME280 (scaled for BLE packer)
    int16_t  temp_c_x100;   // °C*100
    int16_t  rh_x100;       // %RH*100
    int32_t  press_hPa_x1000; // hPa*10

    // ENS160
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;

    // AS7343
    uint16_t as7343[AS7343_NUM_CH]; // 12 bands + VISIBLE

    // Battery (MAX17048 fuel gauge via fuel_gauge subsystem)
    uint16_t batt_mV;       /* cell voltage in mV, e.g. 3850         */
    uint8_t  batt_pct;      /* state of charge 0–100%                */
    int16_t  batt_rate_x10; /* charge rate ×10 %/hr, + = charging    */

    // SPH0645 microphone (1-second averaged FFT summary)
    int16_t  snd_rms_dbfs_x100;  /* dBFS × 100, e.g. -3820 = -38.20 dBFS */
    uint16_t snd_peak_freq_hz;   /* dominant frequency, 50–15000 Hz        */
    uint16_t snd_peak_mag_x10;   /* peak bin magnitude × 10                */

    // Capacitive soil moisture sensor v2
    uint16_t soil_vwc_x100;      /* VWC % × 100, e.g. 4567 = 45.67%       */
};

/* -------- Partial messages (from each sensor thread) -------- */
struct bme280_msg { double temp_c, rh_pct, press_hPa; };
struct ens160_msg { int eco2_ppm, tvoc_ppb, aqi; };
struct as7343_msg { uint16_t ch[AS7343_NUM_CH]; /*405..855*/ };
struct batt_msg   { uint16_t mV; uint8_t pct; int16_t rate_x10; };
struct moisture_msg { uint16_t vwc_x100; };  /* VWC % × 100 */

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

/**
 * @brief Thread function to intialise ring buffer utilsied for sensor and relevent shell command
 */
extern void sensor_control_thread(void);

#endif /* SENSOR_H */