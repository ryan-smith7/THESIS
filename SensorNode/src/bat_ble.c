/**
 * @file bat_ble.c
 * @brief BLE GATT characteristic for MAX17048 battery fuel gauge.
 *
 * Payload (12 bytes, big-endian):    ← was 11, +1 for dev_id
 *   [0-3]  utc_sec   uint32   UTC seconds at measurement
 *   [4-5]  utc_ms    uint16   UTC milliseconds (0-999)
 *   [6-7]  mV        uint16   cell voltage in mV
 *   [8]    pct       uint8    state of charge 0-100%
 *   [9-10] rate_x10  int16    charge rate ×10 %/hr
 *   [11]   dev_id    uint8    sensor node device ID (from DEVICE_ID)
 *
 * dev_id allows Grafana to plot battery data from multiple sensor nodes
 * on the same panel, distinguished by device.
 *
 * Service UUID:  BA000001-0000-ABCD-9078-F6E5D4C3B2BA
 * Char UUID:     BA000002-0000-ABCD-9078-F6E5D4C3B2BA
 */

#include "bat_ble.h"
#include "modality_ble.h"
#include "sensor.h"

LOG_MODULE_REGISTER(bat_ble, LOG_LEVEL_INF);

#define BAT_SVC_UUID_BYTES \
    0xBA, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xBA

#define BAT_CHR_UUID_BYTES \
    0xBA, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xBA

#define BAT_BUF_LEN 12  /* was 11 — added 1 byte for dev_id */

static bool            bat_notify_enabled = false;
static struct bt_conn *bat_conn           = NULL;
static uint8_t         bat_buf[BAT_BUF_LEN];

MODALITY_CCC_CHANGED(bat, bat_notify_enabled);
MODALITY_READ_HANDLER(bat, bat_buf, BAT_BUF_LEN);
MODALITY_GATT_SERVICE(bat, BAT_SVC_UUID_BYTES, BAT_CHR_UUID_BYTES);

static void bat_connected(struct bt_conn *conn, uint8_t err){

    if (!err) {
        bat_conn = bt_conn_ref(conn);
    }
}

static void bat_disconnected(struct bt_conn *conn, uint8_t reason) {

    if (bat_conn) {
        bt_conn_unref(bat_conn);
        bat_conn = NULL;
    }
    bat_notify_enabled = false;
}

static struct bt_conn_cb bat_conn_cb = {
    .connected    = bat_connected,
    .disconnected = bat_disconnected,
};

void bat_pack_and_notify(const struct batt_msg *msg) {

    struct bt_conn *conn = bat_conn;

    if (!conn || !bat_notify_enabled) {
        return;
    }

    bat_buf[0]  = (msg->utc_sec  >>  24) & 0xFF;  /* utc_sec [0-3]  */
    bat_buf[1]  = (msg->utc_sec  >>  16) & 0xFF;
    bat_buf[2]  = (msg->utc_sec  >>   8) & 0xFF;
    bat_buf[3]  =  msg->utc_sec          & 0xFF;
    bat_buf[4]  = (msg->utc_ms   >>   8) & 0xFF;  /* utc_ms  [4-5]  */
    bat_buf[5]  =  msg->utc_ms           & 0xFF;
    bat_buf[6]  = (msg->mV       >>   8) & 0xFF;  /* mV      [6-7]  */
    bat_buf[7]  =  msg->mV               & 0xFF;
    bat_buf[8]  =  msg->pct;                       /* pct     [8]    */
    bat_buf[9]  = (msg->rate_x10 >>   8) & 0xFF;  /* rate    [9-10] */
    bat_buf[10] =  msg->rate_x10         & 0xFF;
    bat_buf[11] =  DEVICE_ID;                      /* dev_id  [11]   */

    MODALITY_NOTIFY(bat, conn, bat_notify_enabled, bat_buf, BAT_BUF_LEN);
}

void bat_ble_thread(void) {

    bt_conn_cb_register(&bat_conn_cb);
    LOG_INF("bat_ble thread ready");

    while (1) {
        struct batt_msg msg;
        if (k_msgq_get(&batt_q, &msg, K_FOREVER) != 0) {
            continue;
        }

        if (bat_notify_enabled && bat_conn) {
            bat_pack_and_notify(&msg);
        }
        /* Battery is live-only — not logged to SD */
    }
}