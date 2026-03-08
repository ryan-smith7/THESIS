#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/util.h>
#include <zephyr/shell/shell.h>
// #include "s4744413_rtc.h"
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/ens160.h>
#include "sensor.h"
#include "as7343.h"

// MAIN.C bluetooth test

#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>


#include <string.h>
#include "bluetooth.h"

// #include "s4744413_file.h"

//TESTING TASK1
// K_THREAD_DEFINE(control_id, STACK_SIZE_RTC, control_thread, NULL, NULL, NULL, RTC_PRIORITY, 0, 0);

//TESTING TASK2
// K_THREAD_DEFINE(sensor_control_id, STACK_SIZE_SENSOR, sensor_control_thread, NULL, NULL, NULL, SENSOR_PRIORITY, 0, 0);
// K_THREAD_DEFINE(hts221_tid, STACK_SIZE_SENSOR, hts221_thread, NULL, NULL, NULL, HTS221_THREAD_PRIORITY , 0, 0);
// K_THREAD_DEFINE(lps22hb_tid, STACK_SIZE_SENSOR, lps22hb_thread, NULL, NULL, NULL, LPS22HB_THREAD_PRIORITY, 0, 0);
// K_THREAD_DEFINE(lis3mdl_tid, STACK_SIZE_SENSOR, lis3mdl_thread, NULL, NULL, NULL, LIST3MDL_THREAD_PRIORITY, 0, 0);


                /* Start immediately -> 0 (ms) */
K_THREAD_DEFINE(tracker_tid, TRACKER_CONTROL_STACK_SIZE, tracker_thread,
                NULL, NULL, NULL,
                TRACKER_CONTROL_PRIORITY, 0, 0);

/* Create paused -> SYS_FOREVER_MS */
K_THREAD_DEFINE(bme280_tid,  STACK_SIZE_SENSOR, bme280_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(ens160_tid,  STACK_SIZE_SENSOR, ens160_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(as7343_tid,  STACK_SIZE_SENSOR, as7343_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(combiner_tid, STACK_SIZE_SENSOR, combiner_thread,
                NULL, NULL, NULL,
                BME280_THREAD_PRIORITY, 0, 0);


// //TESTING TASK 5
// K_THREAD_DEFINE(file_control_id, FILE_STACK_SIZE, file_control_thread, NULL, NULL, NULL, FILE_PRIORITY, 0, 0);