#ifndef DS18B20_BLE_H
#define DS18B20_BLE_H

#include "sensor.h"

#define DS18B20_BLE_STACK_SIZE  2048
#define DS18B20_BLE_PRIORITY    6

extern struct k_sem ds18b20_notify_sem;

extern void ds18b20_ble_thread(void);

extern bool ds18b20_pack_and_notify(const struct ds18b20_msg *msg);

#endif /* DS18B20_BLE_H */
