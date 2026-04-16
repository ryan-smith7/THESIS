#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/util.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/ens160.h>
 
#include "sensor.h"
#include "bluetooth.h"
#include "cur_ble.h"
 
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <string.h>

/* ── Shared across all nodes ─────────────────────────────────────────────── */
K_THREAD_DEFINE(bluetooth_init_tid, BLUETOOTH_CONTROL_STACK_SIZE, bluetooth_init_thread,
                NULL, NULL, NULL,
                BLUETOOTH_CONTROL_PRIORITY, 0, 0);

                /* Sensor acquisition thread */
K_THREAD_DEFINE(current_tid, 2 * CUR_BLE_STACK_SIZE, current_thread,
                NULL, NULL, NULL,
                CUR_BLE_PRIORITY, 0, 0);
 
/* BLE modality thread */
K_THREAD_DEFINE(cur_ble_tid, CUR_BLE_STACK_SIZE, cur_ble_thread,
                NULL, NULL, NULL,
                CUR_BLE_PRIORITY, 0, 0);