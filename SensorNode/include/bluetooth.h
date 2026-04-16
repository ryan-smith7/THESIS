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

#define BLUETOOTH_CONTROL_STACK_SIZE 2048
#define BLUETOOTH_CONTROL_PRIORITY 5

// ---------- Sensor payload (ONLY what we need) ---------- DOUBLE CHECK THIS MACRO IS NEEDED HERE (THINK REDUNDANT)
#define AS7343_NUM_CH 13  // 405,425,450,475,515,550,555,600,640,690,745,855,VISIBLE

extern void pack_sensor_data(const struct sensor_blk *sensor);

extern int init_bluetooth(void);

extern int start_advertising(void);

extern int stop_advertising(void);

extern int stop_advertising_and_disconnect();

extern void sensornode_thread(void);

#endif // TRACKER_SERVICE_H