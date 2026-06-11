#ifndef CUR_BLE_H
#define CUR_BLE_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include "sensor.h"
#include "sd_log.h"

/* ── Thread config ──────────────────────────────────────── */
#define CUR_BLE_STACK_SIZE  1024
#define CUR_BLE_PRIORITY    6

/* ── Public API ─────────────────────────────────────────── */
extern void cur_ble_thread(void);
extern void cur_pack_and_notify(const struct current_msg *msg);

#endif /* CUR_BLE_H */
