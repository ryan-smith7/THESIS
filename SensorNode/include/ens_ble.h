#ifndef ENS_BLE_H
#define ENS_BLE_H

#include "sensor.h"

#define ENS_BLE_STACK_SIZE  2048
#define ENS_BLE_PRIORITY    6

extern void ens_ble_thread(void);

extern void ens_ble_notify_offline(const struct ens160_msg *msg);

#endif /* ENS_BLE_H */
