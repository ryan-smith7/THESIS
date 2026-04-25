/**
 * @file modality_ble.h
 * @brief Shared macros for per-modality BLE GATT characteristics.
 *
 * Only covers the parts that are truly identical across all modalities:
 *   - CCC changed callback
 *   - Read handler
 *   - GATT service definition
 *   - Notify helper
 *
 * Connection tracking (connected/disconnected callbacks and
 * bt_conn_cb_register) is written as plain functions in each .c file
 * to avoid macro-expanded function definitions at file scope.
 */

#ifndef MODALITY_BLE_H
#define MODALITY_BLE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

/* ── CCC changed callback ─────────────────────────────────────────────── */
#define MODALITY_CCC_CHANGED(name, flag, sem)                               \
    static void name##_ccc_changed(const struct bt_gatt_attr *attr,         \
                                   uint16_t value)                          \
    {                                                                       \
        flag = (value == BT_GATT_CCC_NOTIFY);                              \
        if (flag) { k_sem_give(&sem); }                                     \
        LOG_INF(#name " notifications %s", flag ? "on" : "off");           \
    }

/* Without semaphore — for non-drainable modalities (bat, cur) */
#define MODALITY_CCC_CHANGED_NOSEM(name, flag)                              \
    static void name##_ccc_changed(const struct bt_gatt_attr *attr,         \
                                   uint16_t value)                          \
    {                                                                       \
        flag = (value == BT_GATT_CCC_NOTIFY);                              \
        LOG_INF(#name " notifications %s", flag ? "on" : "off");           \
    }

/* ── Read handler ─────────────────────────────────────────────────────── */
#define MODALITY_READ_HANDLER(name, buf, bufsz)                             \
    static ssize_t name##_read(struct bt_conn *conn,                        \
                               const struct bt_gatt_attr *attr,             \
                               void *out, uint16_t len, uint16_t offset)    \
    {                                                                       \
        return bt_gatt_attr_read(conn, attr, out, len, offset, buf, bufsz); \
    }

/* ── GATT service definition ──────────────────────────────────────────── */
#define MODALITY_GATT_SERVICE(name, svc_uuid_bytes, chr_uuid_bytes)         \
    BT_GATT_SERVICE_DEFINE(name##_svc,                                      \
        BT_GATT_PRIMARY_SERVICE(                                            \
            BT_UUID_DECLARE_128(svc_uuid_bytes)),                           \
        BT_GATT_CHARACTERISTIC(                                             \
            BT_UUID_DECLARE_128(chr_uuid_bytes),                            \
            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,                       \
            BT_GATT_PERM_READ,                                              \
            name##_read, NULL, NULL),                                       \
        BT_GATT_CCC(name##_ccc_changed,                                     \
                    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),               \
    )

/* ── Notify helper ────────────────────────────────────────────────────── */
#define MODALITY_NOTIFY(name, conn_var, flag, buf, len)                     \
    do {                                                                    \
        if ((flag) && (conn_var)) {                                         \
            int _err = bt_gatt_notify((conn_var),                           \
                                      &name##_svc.attrs[1],                 \
                                      (buf), (len));                        \
            if (_err) {                                                     \
                LOG_ERR(#name " notify failed: %d", _err);                  \
            }                                                               \
        }                                                                   \
    } while (0)

#endif /* MODALITY_BLE_H */