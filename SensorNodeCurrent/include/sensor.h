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
#define DEVICE_ID 3

// sensor.h
#pragma once
#include <zephyr/kernel.h>
#include <zephyr/types.h>

/* -------- Partial messages (from each sensor thread) --------
 *
 * utc_sec + utc_ms: UTC timestamp stamped at measurement moment via
 * time_sync_get_utc_ms(). utc_sec is 0 and utc_ms is 0 if no time
 * sync has occurred yet. Both fields are sent over BLE so the gateway
 * can forward millisecond-precision timestamps to Azure.
 */

struct current_msg { //CURRENT ADDITION
    int16_t  current_uA;   /* microamps signed — e.g. 1500 = 1.500 mA  */
    uint16_t voltage_mV;   /* millivolts       — e.g. 3300 = 3.300 V   */
    uint32_t utc_sec;      /* UTC seconds at measurement (0 = unsynced) */
    uint16_t utc_ms;       /* UTC milliseconds 0-999                    */
};

/* -------- Message queues -------- */
#define Q_DEPTH 8

extern struct k_msgq current_q;  //CURRENT ADDITION

extern void current_thread(void); //CURRENT ADDITION

#endif /* SENSOR_H */