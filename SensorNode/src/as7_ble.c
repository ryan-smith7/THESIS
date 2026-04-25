/**
 * @file as7_ble.c
 * @brief BLE GATT characteristic for AS7343 spectral sensor (13 channels).
 *
 * Payload (32 bytes, big-endian):    ← was 30, +2 for utc_ms
 *   [0-3]   utc_sec          uint32
 *   [4-5]   utc_ms           uint16   UTC milliseconds (0-999) — NEW
 *   [6-31]  ch[0..12] uint16  405,425,450,475,515,550,555,600,640,690,745,855,VIS
 *
 * Service UUID:  A7000001-0000-ABCD-9078-F6E5D4C3B2A7
 * Char UUID:     A7000002-0000-ABCD-9078-F6E5D4C3B2A7
 */

#include "as7_ble.h"
#include "modality_ble.h"
#include "sd_log.h"

LOG_MODULE_REGISTER(as7_ble, LOG_LEVEL_INF);

#define AS7_SVC_UUID_BYTES \
    0xA7, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xA7

#define AS7_CHR_UUID_BYTES \
    0xA7, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xA7

#define AS7_BUF_LEN (6 + AS7343_NUM_CH * 2 + 1)  /* was 4+26=30, now 6+26=32 +1 FOR DEVICE*/

static bool            as7_notify_enabled = false;
static struct bt_conn *as7_conn           = NULL;
static uint8_t         as7_buf[AS7_BUF_LEN];

K_SEM_DEFINE(as7_notify_sem, 0, 1);

MODALITY_CCC_CHANGED(as7, as7_notify_enabled, as7_notify_sem);
MODALITY_READ_HANDLER(as7, as7_buf, AS7_BUF_LEN);
MODALITY_GATT_SERVICE(as7, AS7_SVC_UUID_BYTES, AS7_CHR_UUID_BYTES);

static void as7_connected(struct bt_conn *conn, uint8_t err)
{
    if (!err) as7_conn = bt_conn_ref(conn);
}

static void as7_disconnected(struct bt_conn *conn, uint8_t reason) {

    if (as7_conn) {
        bt_conn_unref(as7_conn);
        as7_conn = NULL;
    }
    
    as7_notify_enabled = false;
    k_sem_reset(&as7_notify_sem);
}

static struct bt_conn_cb as7_conn_cb = {
    .connected    = as7_connected,
    .disconnected = as7_disconnected,
};

bool as7_pack_and_notify(const struct as7343_msg *msg) {

    struct bt_conn *conn = as7_conn;
    if (!conn || !as7_notify_enabled) {
        return false;
    }

    as7_buf[0] = (msg->utc_sec >>  24) & 0xFF;  /* utc_sec [0-3] */
    as7_buf[1] = (msg->utc_sec >>  16) & 0xFF;
    as7_buf[2] = (msg->utc_sec >>   8) & 0xFF;
    as7_buf[3] =  msg->utc_sec         & 0xFF;
    as7_buf[4] = (msg->utc_ms  >>   8) & 0xFF;  /* utc_ms  [4-5] — NEW */
    as7_buf[5] =  msg->utc_ms          & 0xFF;

    for (int i = 0; i < AS7343_NUM_CH; i++) {   /* bins    [6-31] */
        as7_buf[6 + i * 2]     = (msg->ch[i] >> 8) & 0xFF;
        as7_buf[6 + i * 2 + 1] =  msg->ch[i]       & 0xFF;
    }
    as7_buf[32] =  DEVICE_ID;                      /* dev_id  [32]   */

    MODALITY_NOTIFY(as7, conn, as7_notify_enabled, as7_buf, AS7_BUF_LEN);
    return true;
}

void as7_ble_thread(void) {
    bt_conn_cb_register(&as7_conn_cb);
    LOG_INF("as7_ble thread ready");

    while (1) {
        struct as7343_msg msg;
        if (k_msgq_get(&as7_q, &msg, K_FOREVER) != 0) {
            continue;
        }

        bool utc_valid = (msg.utc_sec > SD_LOG_UTC_MIN);

        if (as7_notify_enabled && as7_conn && utc_valid) {
            as7_pack_and_notify(&msg);
#if defined(CONFIG_SD_LOGGING)
#if defined(CONFIG_SD_LOG_ALWAYS_WRITE)
            SD_LOG_BOOT(sd_log_boot_path_as7(), &msg);
#endif
#endif
        } else {
#if defined(CONFIG_SD_LOGGING)
            if (utc_valid) {
                SD_LOG_UTC(SD_LOG_AS7343, &msg);
#if defined(CONFIG_SD_LOG_ALWAYS_WRITE)
                SD_LOG_BOOT(sd_log_boot_path_as7(), &msg);
#endif
            } else {
                SD_LOG_BOOT(sd_log_boot_path_as7(), &msg);
            }
#endif
        }
    }
}