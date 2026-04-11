#ifndef TRACKER_SERVICE_H
#define TRACKER_SERVICE_H

#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>

#include <string.h>

#include "sensor.h"

#define TRACKER_CONTROL_STACK_SIZE 2048
#define TRACKER_CONTROL_PRIORITY 5

// ---------- Sensor payload (ONLY what we need) ----------
#define PROTO_VER 1
#define AS7343_NUM_CH 13  // 405,425,450,475,515,550,555,600,640,690,745,855,VISIBLE

// Scaling (same as before so Grafana/server stays simple)
#define BME_TEMP_SCALE   100   // °C *100
#define BME_HUM_SCALE    100   // %RH *100
#define BME_PRESS_SCALE  10    // hPa *10

// struct sensor_blk {
//     uint32_t time;          // epoch s
//     uint32_t uptime_ms;     // ms
//     uint8_t  proto_ver;     // = PROTO_VER
//     uint8_t  dev_id;        // node id

//     // BME280
//     int16_t  temp_c_x100;   // e.g., 23.50°C -> 2350
//     int16_t  rh_x100;       // 45.67% -> 4567
//     int16_t  press_hPa_x10; // 1013.2 hPa -> 10132

//     // ENS160
//     uint16_t eco2_ppm;      // ppm
//     uint16_t tvoc_ppb;      // ppb
//     uint8_t  aqi;           // 0..5 (or 1..5, pass-through)

//     // AS7343 in fixed order
//     uint16_t as7343[AS7343_NUM_CH]; // raw/scaled counts

//     uint16_t batt_mV;       // battery in mV (0 if N/A)
// };

extern uint8_t get_ble_tick(void);

extern void set_ble_tick(uint8_t value);

extern void pack_sensor_data(const struct sensor_blk *sensor);

extern int init_bluetooth(void);

extern int start_advertising(void);

extern int stop_advertising(void);

extern int stop_advertising_and_disconnect();

extern void tracker_thread(void);

#endif // TRACKER_SERVICE_H