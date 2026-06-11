#ifndef AS7_BLE_H
#define AS7_BLE_H

#include "sensor.h"

#define AS7_BLE_STACK_SIZE  2048
#define AS7_BLE_PRIORITY    6

extern struct k_sem as7_notify_sem;

/**
 * @brief AS7343 BLE thread — dequeues messages from as7_q and either
 * notifies the gateway or logs to SD card depending on connection state
 * and UTC validity.
 */
extern void as7_ble_thread(void);

/**
 * @brief Pack an AS7343 message into the BLE buffer and notify the gateway.
 *
 * Encodes utc_sec (4 bytes), utc_ms (2 bytes), 13 spectral channels as
 * big-endian uint32 (52 bytes), and dev_id (1 byte) into as7_buf, then
 * sends a GATT notification.
 *
 * @param msg  Spectral message to encode.
 * @return     true if notification was sent, false if not connected or
 *             notifications not enabled.
 */
extern bool as7_pack_and_notify(const struct as7343_msg *msg);

#endif /* AS7_BLE_H */
