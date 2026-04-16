#ifndef BAT_BLE_H
#define BAT_BLE_H

#include "sensor.h"

#define BAT_BLE_STACK_SIZE  2048
#define BAT_BLE_PRIORITY    6

extern void bat_ble_thread(void);

extern void bat_pack_and_notify(const struct batt_msg *msg);

#endif /* BAT_BLE_H */
