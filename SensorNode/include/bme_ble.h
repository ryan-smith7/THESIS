#ifndef BME_BLE_H
#define BME_BLE_H

#include "sensor.h"

#define BME_BLE_STACK_SIZE  2048
#define BME_BLE_PRIORITY    6

extern void bme_ble_thread(void);

extern void bme_ble_notify_offline(const struct bme280_msg *msg);

#endif /* BME_BLE_H */
