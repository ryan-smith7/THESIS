/**
 * @file ds18b20_ble.c
 * @brief BLE GATT characteristic for DS18B20 soil temperature sensor.
 *
 * Payload (11 bytes, big-endian):
 *   [0-3]  utc_sec       uint32   UTC seconds at measurement
 *   [4-5]  utc_ms        uint16   UTC milliseconds (0-999)
 *   [6-7]  temp_val1     int16    integer degrees C
 *   [8-9]  temp_val2     int16    fractional part (scaled to centidegrees)
 *   [10]   device_id     uint8
 *
 * Service UUID:  D5000001-0000-ABCD-9078-F6E5D4C3B2D5
 * Char UUID:     D5000002-0000-ABCD-9078-F6E5D4C3B2D5
 */

#include "ds18b20_ble.h"
#include "modality_ble.h"
#include "sd_log.h"

LOG_MODULE_REGISTER(ds18b20_ble, LOG_LEVEL_INF);

#define DS18B20_SVC_UUID_BYTES \
    0xD5, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x01, 0x00, 0x00, 0xD5

#define DS18B20_CHR_UUID_BYTES \
    0xD5, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, \
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00, \
    0x02, 0x00, 0x00, 0xD5

#define DS18B20_BUF_LEN 11  /* utc_sec(4) + utc_ms(2) + val1(2) + val2(2) + device_id(1) */

static bool            ds18b20_notify_enabled = false;
static struct bt_conn *ds18b20_conn           = NULL;
static uint8_t         ds18b20_buf[DS18B20_BUF_LEN];

K_SEM_DEFINE(ds18b20_notify_sem, 0, 1);

MODALITY_CCC_CHANGED(ds18b20, ds18b20_notify_enabled, ds18b20_notify_sem);
MODALITY_READ_HANDLER(ds18b20, ds18b20_buf, DS18B20_BUF_LEN);
MODALITY_GATT_SERVICE(ds18b20, DS18B20_SVC_UUID_BYTES, DS18B20_CHR_UUID_BYTES);

static void ds18b20_connected(struct bt_conn *conn, uint8_t err) {

    if (!err) {
        ds18b20_conn = bt_conn_ref(conn);
    }
}

static void ds18b20_disconnected(struct bt_conn *conn, uint8_t reason) {

    if (ds18b20_conn) {
        bt_conn_unref(ds18b20_conn);
        ds18b20_conn = NULL;
    }

    ds18b20_notify_enabled = false;
    k_sem_reset(&ds18b20_notify_sem);
}

static struct bt_conn_cb ds18b20_conn_cb = {
    .connected    = ds18b20_connected,
    .disconnected = ds18b20_disconnected,
};

bool ds18b20_pack_and_notify(const struct ds18b20_msg *msg) {

    struct bt_conn *conn = ds18b20_conn;
    if (!conn || !ds18b20_notify_enabled) {
        return false;
    }

    /* Fractional part: scale val2 (millionths) to centidegrees (hundredths) */
    int16_t temp_val1 = (int16_t)msg->temp_val1;
    int16_t temp_val2 = (int16_t)(msg->temp_val2 / 10000);

    ds18b20_buf[0]  = (msg->utc_sec >> 24) & 0xFF;   /* utc_sec [0-3] */
    ds18b20_buf[1]  = (msg->utc_sec >> 16) & 0xFF;
    ds18b20_buf[2]  = (msg->utc_sec >>  8) & 0xFF;
    ds18b20_buf[3]  =  msg->utc_sec        & 0xFF;
    ds18b20_buf[4]  = (msg->utc_ms  >>  8) & 0xFF;   /* utc_ms  [4-5] */
    ds18b20_buf[5]  =  msg->utc_ms         & 0xFF;
    ds18b20_buf[6]  = (temp_val1    >>  8) & 0xFF;   /* temp_val1 [6-7] */
    ds18b20_buf[7]  =  temp_val1           & 0xFF;
    ds18b20_buf[8]  = (temp_val2    >>  8) & 0xFF;   /* temp_val2 [8-9] */
    ds18b20_buf[9]  =  temp_val2           & 0xFF;
    ds18b20_buf[10] =  DEVICE_ID;                     /* device_id [10] */

    MODALITY_NOTIFY(ds18b20, conn, ds18b20_notify_enabled,
                    ds18b20_buf, DS18B20_BUF_LEN);
    return true;
}

void ds18b20_ble_thread(void) {
    bt_conn_cb_register(&ds18b20_conn_cb);
    LOG_INF("ds18b20_ble thread ready");

    while (1) {
        struct ds18b20_msg msg;
        if (k_msgq_get(&ds18b20_q, &msg, K_FOREVER) != 0) {
            continue;
        }

        bool utc_valid = (msg.utc_sec > SD_LOG_UTC_MIN);

        if (ds18b20_notify_enabled && ds18b20_conn && utc_valid) {
            ds18b20_pack_and_notify(&msg);
#if defined(CONFIG_SD_LOGGING)
#if defined(CONFIG_SD_LOG_ALWAYS_WRITE)
            SD_LOG_BOOT(sd_log_boot_path_ds18b20(), &msg);
#endif
#endif
        } else {
#if defined(CONFIG_SD_LOGGING)
            if (utc_valid) {
                SD_LOG_UTC(SD_LOG_DS18B20, &msg);
#if defined(CONFIG_SD_LOG_ALWAYS_WRITE)
                SD_LOG_BOOT(sd_log_boot_path_ds18b20(), &msg);
#endif
            } else {
                SD_LOG_BOOT(sd_log_boot_path_ds18b20(), &msg);
            }
#endif
        }
    }
}
