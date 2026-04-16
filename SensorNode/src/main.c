#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/util.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/ens160.h>
 
#include "sensor.h"
#include "as7343.h"
#include "bluetooth.h"
#include "sound.h"
#include "sound_ble.h"
 
/* ── New modality BLE headers ──────────────────────────── */
#include "bme_ble.h"
#include "ens_ble.h"
#include "as7_ble.h"
#include "mst_ble.h"
#include "bat_ble.h"
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

#if defined(CONFIG_SD_LOGGING)
#include "sd_log.h"
#endif


/* ── Sensor node selection ───────────────────────────────────────────────────
 * Set exactly one of these in prj.conf:
 *   CONFIG_SENSOR_NODE_1=y
 *   CONFIG_SENSOR_NODE_2=y
 */

/* ── Shared across all nodes ─────────────────────────────────────────────── */
K_THREAD_DEFINE(sensornode_tid, BLUETOOTH_CONTROL_STACK_SIZE, sensornode_thread,
                NULL, NULL, NULL,
                BLUETOOTH_CONTROL_PRIORITY, 0, 0);

#if defined(CONFIG_SD_LOGGING)
K_THREAD_DEFINE(sd_drain_tid, 4096, sd_drain_thread,
                NULL, NULL, NULL, 5, 0, 0);
#endif

#if defined(CONFIG_FUEL_GAUGE)
// Battery — uncomment later
K_THREAD_DEFINE(max17048_tid, STACK_SIZE_SENSOR, max17048_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);
K_THREAD_DEFINE(bat_ble_tid, BAT_BLE_STACK_SIZE, bat_ble_thread,
                NULL, NULL, NULL,
                BAT_BLE_PRIORITY, 0, 0);
#endif

/* ── Sensor Node 1: BME280 + ENS160 + Sound ─────────────────────────────── */
#if defined(CONFIG_SENSOR_NODE_1)

/* Sensor acquisition threads */
K_THREAD_DEFINE(bme280_tid, STACK_SIZE_SENSOR, bme280_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(ens160_tid, STACK_SIZE_SENSOR, ens160_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(sound_tid, SOUND_STACK_SIZE, sound_thread,
                NULL, NULL, NULL,
                SOUND_PRIORITY, 0, 0);

/* BLE modality threads */
K_THREAD_DEFINE(bme_ble_tid, BME_BLE_STACK_SIZE, bme_ble_thread,
                NULL, NULL, NULL,
                BME_BLE_PRIORITY, 0, 0);

K_THREAD_DEFINE(ens_ble_tid, ENS_BLE_STACK_SIZE, ens_ble_thread,
                NULL, NULL, NULL,
                ENS_BLE_PRIORITY, 0, 0);

K_THREAD_DEFINE(sound_ble_tid, SOUND_BLE_STACK_SIZE, sound_ble_thread,
                NULL, NULL, NULL,
                SOUND_BLE_PRIORITY, 0, 0);

/* ── Sensor Node 2: AS7343 + Moisture ───────────────────────────────────── */
#elif defined(CONFIG_SENSOR_NODE_2)

/* Sensor acquisition threads */
K_THREAD_DEFINE(as7343_tid, STACK_SIZE_SENSOR, as7343_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(moisture_tid, STACK_SIZE_SENSOR, moisture_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

/* BLE modality threads */
K_THREAD_DEFINE(as7_ble_tid, AS7_BLE_STACK_SIZE, as7_ble_thread,
                NULL, NULL, NULL,
                AS7_BLE_PRIORITY, 0, 0);

K_THREAD_DEFINE(mst_ble_tid, MST_BLE_STACK_SIZE, mst_ble_thread,
                NULL, NULL, NULL,
                MST_BLE_PRIORITY, 0, 0);

#elif defined(CONFIG_SENSOR_NODE_3)
                /* Sensor acquisition thread */
K_THREAD_DEFINE(current_tid, 2 * CUR_BLE_STACK_SIZE, current_thread,
                NULL, NULL, NULL,
                CUR_BLE_PRIORITY, 0, 0);
 
/* BLE modality thread */
K_THREAD_DEFINE(cur_ble_tid, CUR_BLE_STACK_SIZE, cur_ble_thread,
                NULL, NULL, NULL,
                CUR_BLE_PRIORITY, 0, 0);

#else
#error "No sensor node selected. Set CONFIG_SENSOR_NODE_1=y or CONFIG_SENSOR_NODE_2=y,  CONFIG_SENSOR_NODE_3=y in prj.conf"

#endif

// K_THREAD_DEFINE(combiner_tid, STACK_SIZE_SENSOR, combiner_thread,
//                 NULL, NULL, NULL,
//                 BME280_THREAD_PRIORITY, 0, 0);
