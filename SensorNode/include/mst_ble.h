#ifndef MST_BLE_H
#define MST_BLE_H

#include "sensor.h"

#define MST_BLE_STACK_SIZE  2048
#define MST_BLE_PRIORITY    6

extern struct k_sem mst_notify_sem;

extern void mst_ble_thread(void);

extern bool mst_pack_and_notify(const struct moisture_msg *msg);

#endif /* MST_BLE_H */
