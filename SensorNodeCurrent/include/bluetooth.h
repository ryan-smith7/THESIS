#ifndef BLUETOOTH_SERVICE_H
#define BLUETOOTH_SERVICE_H

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

extern int init_bluetooth(void);

extern int start_advertising(void);

extern int stop_advertising(void);

extern int stop_advertising_and_disconnect();

extern void bluetooth_init_thread(void);

#endif