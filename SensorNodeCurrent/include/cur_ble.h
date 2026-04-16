/**
 * @file cur_ble.h
 * @brief BLE GATT characteristic for electrical current sensor.
 *
 * Payload (14 bytes, big-endian):
 *   [0-3]  utc_sec    uint32   UTC seconds at measurement (0 = unsynced)
 *   [4-5]  utc_ms     uint16   UTC milliseconds 0-999
 *   [6-7]  current_uA int16    microamps signed, e.g. 1500 = 1.500 mA
 *   [8-9]  voltage_mV uint16   millivolts, e.g. 3300 = 3.300 V
 *
 * Service UUID:  CC000001-0000-ABCD-9078-F6E5D4C3B2CC
 * Char UUID:     CC000002-0000-ABCD-9078-F6E5D4C3B2CC
 */

#ifndef CUR_BLE_H
#define CUR_BLE_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include "sensor.h"

/* ── Thread config ──────────────────────────────────────── */
#define CUR_BLE_STACK_SIZE  1024
#define CUR_BLE_PRIORITY    6

/* ── Public API ─────────────────────────────────────────── */
extern void cur_ble_thread(void);
extern void cur_ble_notify_offline(const struct current_msg *msg);

#endif /* CUR_BLE_H */
