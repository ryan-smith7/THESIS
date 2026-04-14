#ifndef MST_BLE_H
#define MST_BLE_H

#include "sensor.h"

#define MST_BLE_STACK_SIZE  2048
#define MST_BLE_PRIORITY    6

extern void mst_ble_thread(void);

extern void mst_ble_notify_offline(const struct moisture_msg *msg);

#endif /* MST_BLE_H */
