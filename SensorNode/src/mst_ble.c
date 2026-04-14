/**
 * @file mst_ble.c
 * @brief BLE GATT characteristic for capacitive soil moisture sensor.
 *
 * Payload (8 bytes, big-endian):     ← was 6, +2 for utc_ms
 *   [0-3] utc_sec   uint32   UTC seconds at measurement
 *   [4-5] utc_ms    uint16   UTC milliseconds (0-999) — NEW
 *   [6-7] vwc_x100  uint16
 *
 * Service UUID:  C1000001-0000-ABCD-9078-F6E5D4C3B2C1
 * Char UUID:     C1000002-0000-ABCD-9078-F6E5D4C3B2C1
 */

#include "mst_ble.h"
#include "modality_ble.h"
#include "sd_log.h"

LOG_MODULE_REGISTER(mst_ble, LOG_LEVEL_INF);

#define MST_SVC_UUID_BYTES \
    0xC1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xC1

#define MST_CHR_UUID_BYTES \
    0xC1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xC1

#define MST_BUF_LEN 8  /* was 6 — added 2 bytes for utc_ms */

static bool            mst_notify_enabled = false;
static struct bt_conn *mst_conn           = NULL;
static uint8_t         mst_buf[MST_BUF_LEN];

MODALITY_CCC_CHANGED(mst, mst_notify_enabled);
MODALITY_READ_HANDLER(mst, mst_buf, MST_BUF_LEN);
MODALITY_GATT_SERVICE(mst, MST_SVC_UUID_BYTES, MST_CHR_UUID_BYTES);

static void mst_connected(struct bt_conn *conn, uint8_t err)
{
    if (!err) mst_conn = bt_conn_ref(conn);
}

static void mst_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (mst_conn) { bt_conn_unref(mst_conn); mst_conn = NULL; }
    mst_notify_enabled = false;
}

static struct bt_conn_cb mst_conn_cb = {
    .connected    = mst_connected,
    .disconnected = mst_disconnected,
};

static void mst_pack_and_notify(const struct moisture_msg *msg)
{
    struct bt_conn *conn = mst_conn;
    if (!conn || !mst_notify_enabled) return;

    mst_buf[0] = (msg->utc_sec   >>  24) & 0xFF;  /* utc_sec [0-3] */
    mst_buf[1] = (msg->utc_sec   >>  16) & 0xFF;
    mst_buf[2] = (msg->utc_sec   >>   8) & 0xFF;
    mst_buf[3] =  msg->utc_sec           & 0xFF;
    mst_buf[4] = (msg->utc_ms    >>   8) & 0xFF;  /* utc_ms  [4-5] — NEW */
    mst_buf[5] =  msg->utc_ms            & 0xFF;
    mst_buf[6] = (msg->vwc_x100  >>   8) & 0xFF;  /* vwc     [6-7] */
    mst_buf[7] =  msg->vwc_x100          & 0xFF;

    MODALITY_NOTIFY(mst, conn, mst_notify_enabled, mst_buf, MST_BUF_LEN);
}

void mst_ble_notify_offline(const struct moisture_msg *msg)
{
    mst_pack_and_notify(msg);
}

void mst_ble_thread(void)
{
    bt_conn_cb_register(&mst_conn_cb);
    LOG_INF("mst_ble thread ready");
 
    while (1) {
        struct moisture_msg msg;
        if (k_msgq_get(&moisture_q, &msg, K_FOREVER) != 0) {
            continue;
        }
 
#if defined(CONFIG_SD_LOGGING)
        if (sd_log_is_draining()) {
            continue;
        }
#endif
 
        if (mst_notify_enabled && mst_conn) {
            if (msg.utc_sec > SD_LOG_UTC_MIN) {
                mst_pack_and_notify(&msg);
            } else {
#if defined(CONFIG_SD_LOGGING)
                /* Connected but no UTC sync — log to SD uptime file */
                sd_log_mst(&msg);
#endif
            }
        }
#if defined(CONFIG_SD_LOGGING)
        else {
            sd_log_mst(&msg);
        }
#endif
    }
}