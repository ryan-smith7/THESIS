/**
 * @file bme_ble.c
 * @brief BLE GATT characteristic for BME280 (temperature, humidity, pressure).
 *
 * Payload (14 bytes, big-endian):    ← was 12, +2 for utc_ms
 *   [0-3]  utc_sec          uint32   UTC seconds at measurement
 *   [4-5]  utc_ms           uint16   UTC milliseconds (0-999)
 *   [6-7]  temp_c_x100      int16
 *   [8-9]  rh_x100          int16
 *   [10-13] press_hPa_x1000 int32
 *
 * Service UUID:  B0000001-0000-ABCD-9078-F6E5D4C3B2B0
 * Char UUID:     B0000002-0000-ABCD-9078-F6E5D4C3B2B0
 */

#include "bme_ble.h"
#include "modality_ble.h"
#include "sd_log.h"

LOG_MODULE_REGISTER(bme_ble, LOG_LEVEL_INF);

#define BME_SVC_UUID_BYTES \
    0xB0, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xB0

#define BME_CHR_UUID_BYTES \
    0xB0, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xB0

#define BME_BUF_LEN 14  /* was 12 — added 2 bytes for utc_ms */

static bool            bme_notify_enabled = false;
static struct bt_conn *bme_conn           = NULL;
static uint8_t         bme_buf[BME_BUF_LEN];

MODALITY_CCC_CHANGED(bme, bme_notify_enabled);
MODALITY_READ_HANDLER(bme, bme_buf, BME_BUF_LEN);
MODALITY_GATT_SERVICE(bme, BME_SVC_UUID_BYTES, BME_CHR_UUID_BYTES);

static void bme_connected(struct bt_conn *conn, uint8_t err)
{
    if (!err) bme_conn = bt_conn_ref(conn);
}

static void bme_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (bme_conn) { bt_conn_unref(bme_conn); bme_conn = NULL; }
    bme_notify_enabled = false;
}

static struct bt_conn_cb bme_conn_cb = {
    .connected    = bme_connected,
    .disconnected = bme_disconnected,
};

static void bme_pack_and_notify(const struct bme280_msg *msg)
{
    struct bt_conn *conn = bme_conn;
    if (!conn || !bme_notify_enabled) return;

    int16_t temp  = (int16_t)(msg->temp_c    * 100.0);
    int16_t rh    = (int16_t)(msg->rh_pct    * 100.0);
    int32_t press = (int32_t)(msg->press_hPa * 1000.0);

    bme_buf[0]  = (msg->utc_sec >>  24) & 0xFF;  /* utc_sec [0-3] */
    bme_buf[1]  = (msg->utc_sec >>  16) & 0xFF;
    bme_buf[2]  = (msg->utc_sec >>   8) & 0xFF;
    bme_buf[3]  =  msg->utc_sec         & 0xFF;
    bme_buf[4]  = (msg->utc_ms  >>   8) & 0xFF;  /* utc_ms  [4-5] — NEW */
    bme_buf[5]  =  msg->utc_ms          & 0xFF;
    bme_buf[6]  = (temp  >>  8) & 0xFF;           /* temp    [6-7] */
    bme_buf[7]  =  temp         & 0xFF;
    bme_buf[8]  = (rh    >>  8) & 0xFF;           /* rh      [8-9] */
    bme_buf[9]  =  rh           & 0xFF;
    bme_buf[10] = (press >> 24) & 0xFF;           /* press   [10-13] */
    bme_buf[11] = (press >> 16) & 0xFF;
    bme_buf[12] = (press >>  8) & 0xFF;
    bme_buf[13] =  press        & 0xFF;

    MODALITY_NOTIFY(bme, conn, bme_notify_enabled, bme_buf, BME_BUF_LEN);
}

void bme_ble_notify_offline(const struct bme280_msg *msg)
{
    bme_pack_and_notify(msg);
}

void bme_ble_thread(void)
{
    bt_conn_cb_register(&bme_conn_cb);
    LOG_INF("bme_ble thread ready");
 
    while (1) {
        struct bme280_msg msg;
        if (k_msgq_get(&bme_q, &msg, K_FOREVER) != 0) {
            continue;
        }
 
#if defined(CONFIG_SD_LOGGING)
        if (sd_log_is_draining()) {
            continue;
        }
#endif
 
        if (bme_notify_enabled && bme_conn) {
            if (msg.utc_sec > SD_LOG_UTC_MIN) {
                bme_pack_and_notify(&msg);
            } else {
#if defined(CONFIG_SD_LOGGING)
                /* Connected but no UTC sync — log to SD uptime file */
                sd_log_bme(&msg);
#endif
            }
        }
#if defined(CONFIG_SD_LOGGING)
        else {
            sd_log_bme(&msg);
        }
#endif
    }
}