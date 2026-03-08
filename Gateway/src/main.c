/*
 * main.c — BLE Board (ESP32 #1)
 *
 * Starts two threads:
 *   base_thread        — BLE init, scan, connect to tracker peripherals
 *   process_data_thread — dequeue BLE packets, encode JSON, TX over UART2
 *
 * No WiFi, no MQTT, no Azure on this board.
 * All data goes to the WiFi board (ESP32 #2) via UART2 GPIO17 (TX).
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "bluetooth.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

K_THREAD_DEFINE(process_data_tid,
                BASE_PROCESS_STACK_SIZE,
                process_data_thread,
                NULL, NULL, NULL,
                BASE_PROCESS_PRIORITY, 0, 0);

K_THREAD_DEFINE(base_tid,
                BASE_CONTROL_STACK_SIZE,
                base_thread,
                NULL, NULL, NULL,
                BASE_CONTROL_PRIORITY, 0, 0);