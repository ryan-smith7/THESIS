/**
 * @file ens_ble.c
 * @brief BLE GATT characteristic for ENS160 (eCO2, TVOC, AQI).
 *
 * Payload (11 bytes, big-endian):    ← was 9, +2 for utc_ms
 *   [0-3] utc_sec   uint32   UTC seconds at measurement
 *   [4-5] utc_ms    uint16   UTC milliseconds (0-999)
 *   [6-7] eco2_ppm  uint16
 *   [8-9] tvoc_ppb  uint16
 *   [10]  aqi       uint8
 *
 * Service UUID:  E1000001-0000-ABCD-9078-F6E5D4C3B2E1
 * Char UUID:     E1000002-0000-ABCD-9078-F6E5D4C3B2E1
 */

#include "ens_ble.h"
#include "modality_ble.h"
#include "sd_log.h"

LOG_MODULE_REGISTER(ens_ble, LOG_LEVEL_INF);

#define ENS_SVC_UUID_BYTES \
    0xE1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xE1

#define ENS_CHR_UUID_BYTES \
    0xE1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xE1

#define ENS_BUF_LEN 11  /* was 9 — added 2 bytes for utc_ms */

static bool            ens_notify_enabled = false;
static struct bt_conn *ens_conn           = NULL;
static uint8_t         ens_buf[ENS_BUF_LEN];

MODALITY_CCC_CHANGED(ens, ens_notify_enabled);
MODALITY_READ_HANDLER(ens, ens_buf, ENS_BUF_LEN);
MODALITY_GATT_SERVICE(ens, ENS_SVC_UUID_BYTES, ENS_CHR_UUID_BYTES);

static void ens_connected(struct bt_conn *conn, uint8_t err)
{
    if (!err) ens_conn = bt_conn_ref(conn);
}

static void ens_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (ens_conn) { bt_conn_unref(ens_conn); ens_conn = NULL; }
    ens_notify_enabled = false;
}

static struct bt_conn_cb ens_conn_cb = {
    .connected    = ens_connected,
    .disconnected = ens_disconnected,
};

static void ens_pack_and_notify(const struct ens160_msg *msg)
{
    struct bt_conn *conn = ens_conn;
    if (!conn || !ens_notify_enabled) return;

    uint16_t eco2 = (uint16_t)msg->eco2_ppm;
    uint16_t tvoc = (uint16_t)msg->tvoc_ppb;

    ens_buf[0]  = (msg->utc_sec >>  24) & 0xFF;  /* utc_sec [0-3] */
    ens_buf[1]  = (msg->utc_sec >>  16) & 0xFF;
    ens_buf[2]  = (msg->utc_sec >>   8) & 0xFF;
    ens_buf[3]  =  msg->utc_sec         & 0xFF;
    ens_buf[4]  = (msg->utc_ms  >>   8) & 0xFF;  /* utc_ms  [4-5] — NEW */
    ens_buf[5]  =  msg->utc_ms          & 0xFF;
    ens_buf[6]  = (eco2 >>  8) & 0xFF;            /* eco2    [6-7] */
    ens_buf[7]  =  eco2        & 0xFF;
    ens_buf[8]  = (tvoc >>  8) & 0xFF;            /* tvoc    [8-9] */
    ens_buf[9]  =  tvoc        & 0xFF;
    ens_buf[10] = (uint8_t)msg->aqi;              /* aqi     [10]  */

    MODALITY_NOTIFY(ens, conn, ens_notify_enabled, ens_buf, ENS_BUF_LEN);
}

void ens_ble_notify_offline(const struct ens160_msg *msg)
{
    ens_pack_and_notify(msg);
}

void ens_ble_thread(void)
{
    bt_conn_cb_register(&ens_conn_cb);
    LOG_INF("ens_ble thread ready");
 
    while (1) {
        struct ens160_msg msg;
        if (k_msgq_get(&ens_q, &msg, K_FOREVER) != 0) {
            continue;
        }
 
#if defined(CONFIG_SD_LOGGING)
        if (sd_log_is_draining()) {
            continue;
        }
#endif
 
        if (ens_notify_enabled && ens_conn) {
            if (msg.utc_sec > SD_LOG_UTC_MIN) {
                ens_pack_and_notify(&msg);
            } else {
#if defined(CONFIG_SD_LOGGING)
                /* Connected but no UTC sync — log to SD uptime file */
                sd_log_ens(&msg);
#endif
            }
        }
#if defined(CONFIG_SD_LOGGING)
        else {
            sd_log_ens(&msg);
        }
#endif
    }
}