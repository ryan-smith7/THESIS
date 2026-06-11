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

/* AS7_BUF_LEN = 6 (timestamp) + 13*4 (channels) + 1 (dev_id) = 59 bytes    */
#define AS7_BUF_LEN  59

static bool            as7_notify_enabled = false;
static struct bt_conn *as7_conn           = NULL;
static uint8_t         as7_buf[AS7_BUF_LEN];

K_SEM_DEFINE(as7_notify_sem, 0, 1);

MODALITY_CCC_CHANGED(as7, as7_notify_enabled, as7_notify_sem);
MODALITY_READ_HANDLER(as7, as7_buf, AS7_BUF_LEN);
MODALITY_GATT_SERVICE(as7, AS7_SVC_UUID_BYTES, AS7_CHR_UUID_BYTES);

/**
 * @brief Connection callback — stores a referenced conn handle.
 */
static void as7_connected(struct bt_conn *conn, uint8_t err) {
    if (!err) {
        as7_conn = bt_conn_ref(conn);
    }
}

/**
 * @brief Disconnection callback — releases the conn handle and resets
 * notify state.
 */
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
 bool as7_pack_and_notify(const struct as7343_msg *msg) {
    struct bt_conn *conn = as7_conn;
    if (!conn || !as7_notify_enabled) {
        return false;
    }
 
    /* UTC timestamp — big-endian */
    as7_buf[0] = (msg->utc_sec >> 24) & 0xFF;
    as7_buf[1] = (msg->utc_sec >> 16) & 0xFF;
    as7_buf[2] = (msg->utc_sec >>  8) & 0xFF;
    as7_buf[3] =  msg->utc_sec        & 0xFF;
    as7_buf[4] = (msg->utc_ms  >>  8) & 0xFF;
    as7_buf[5] =  msg->utc_ms         & 0xFF;
 
    /* Spectral + VIS channels — big-endian uint32 (4 bytes each)             */
    /* ch[0..11] = spectral irradiance µW/m²                                  */
    /* ch[12]    = VIS broadband irradiance µW/m²                             */
    for (int i = 0; i < AS7343_NUM_CH; i++) {
        as7_buf[6 + i * 4]     = (msg->ch[i] >> 24) & 0xFF;
        as7_buf[6 + i * 4 + 1] = (msg->ch[i] >> 16) & 0xFF;
        as7_buf[6 + i * 4 + 2] = (msg->ch[i] >>  8) & 0xFF;
        as7_buf[6 + i * 4 + 3] =  msg->ch[i]        & 0xFF;
    }
 
    /* Device ID */
    as7_buf[6 + AS7343_NUM_CH * 4] = DEVICE_ID;
 
    MODALITY_NOTIFY(as7, conn, as7_notify_enabled, as7_buf, AS7_BUF_LEN);
    return true;
}

/**
 * @brief AS7343 BLE thread — dequeues messages from as7_q and either
 * notifies the gateway or logs to SD card depending on connection state
 * and UTC validity.
 */
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