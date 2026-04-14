#ifndef AS7_BLE_H
#define AS7_BLE_H

#include "sensor.h"

#define AS7_BLE_STACK_SIZE  2048
#define AS7_BLE_PRIORITY    6

extern void as7_ble_thread(void);

extern void as7_ble_notify_offline(const struct as7343_msg *msg);

#endif /* AS7_BLE_H */
