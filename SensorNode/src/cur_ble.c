/**
 * @file cur_ble.c
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

#include "cur_ble.h"
#include "modality_ble.h"
#include "sd_log.h"

LOG_MODULE_REGISTER(cur_ble, LOG_LEVEL_INF);

#define CUR_SVC_UUID_BYTES \
    0xCC, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xCC

#define CUR_CHR_UUID_BYTES \
    0xCC, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xCC

#define CUR_BUF_LEN 10  /* utc_sec(4) + utc_ms(2) + current_uA(2) + voltage_mV(2) */

static bool            cur_notify_enabled = false;
static struct bt_conn *cur_conn           = NULL;
static uint8_t         cur_buf[CUR_BUF_LEN];

MODALITY_CCC_CHANGED(cur, cur_notify_enabled);
MODALITY_READ_HANDLER(cur, cur_buf, CUR_BUF_LEN);
MODALITY_GATT_SERVICE(cur, CUR_SVC_UUID_BYTES, CUR_CHR_UUID_BYTES);

static void cur_connected(struct bt_conn *conn, uint8_t err) {
    
    if (!err) {
        cur_conn = bt_conn_ref(conn);
    }
}

static void cur_disconnected(struct bt_conn *conn, uint8_t reason) {

    if (cur_conn) {
        bt_conn_unref(cur_conn);
        cur_conn = NULL;
    }
    cur_notify_enabled = false;
}

static struct bt_conn_cb cur_conn_cb = {
    .connected    = cur_connected,
    .disconnected = cur_disconnected,
};

void cur_pack_and_notify(const struct current_msg *msg) {
    struct bt_conn *conn = cur_conn;

    if (!conn || !cur_notify_enabled) {
        return;
    }

    cur_buf[0] = (msg->utc_sec    >> 24) & 0xFF;  /* utc_sec [0-3] */
    cur_buf[1] = (msg->utc_sec    >> 16) & 0xFF;
    cur_buf[2] = (msg->utc_sec    >>  8) & 0xFF;
    cur_buf[3] =  msg->utc_sec           & 0xFF;
    cur_buf[4] = (msg->utc_ms     >>  8) & 0xFF;  /* utc_ms  [4-5] */
    cur_buf[5] =  msg->utc_ms            & 0xFF;
    cur_buf[6] = (msg->current_uA >>  8) & 0xFF;  /* current [6-7] */
    cur_buf[7] =  msg->current_uA        & 0xFF;
    cur_buf[8] = (msg->voltage_mV >>  8) & 0xFF;  /* voltage [8-9] */
    cur_buf[9] =  msg->voltage_mV        & 0xFF;

    MODALITY_NOTIFY(cur, conn, cur_notify_enabled, cur_buf, CUR_BUF_LEN);
}

void cur_ble_thread(void) {

    bt_conn_cb_register(&cur_conn_cb);
    LOG_INF("cur_ble thread ready");

    while (1) {
        struct current_msg msg;
        if (k_msgq_get(&current_q, &msg, K_FOREVER) != 0) {
            continue;
        }

        if (cur_notify_enabled && cur_conn) {
            if (msg.utc_sec > SD_LOG_UTC_MIN) {
                cur_pack_and_notify(&msg);
            } else {
                /* Connected but no UTC sync yet — drop rather than
                 * send untimestampable data. No SD logging for this
                 * modality (not yet implemented). */
                LOG_DBG("CUR: dropping live sample — no UTC sync");
            }
        }
        /* No SD logging for current modality — not yet implemented */
    }
}
