/**
 * @file sound_ble.c
 * @brief Dedicated BLE GATT characteristic for SPH0645 FFT spectrum.
 *
 * Registers a separate GATT service with one notify+read characteristic.
 * When a spectrum is ready (published to sound_spec_q by sound.c),
 * this thread sends it as 3 sequential BLE notifications.
 *
 * Packet layout per notification:
 *   Byte 0:     pkt_id        (0, 1, or 2)
 *   Byte 1:     total_pkts    (always 3)
 *   Byte 2-3:   timestamp_ms low 16 bits (big-endian) - for reassembly
 *   Byte 4-N:   bin magnitudes as uint16_t big-endian (116 bins per packet)
 *
 * Last packet (pkt_id=2) also appends:
 *   Byte N+1-N+2: rms_dbfs_x100 as int16_t big-endian
 *
 * SD offline logging: when BLE unavailable, full sound_spec_msg is written
 * as raw binary (700 bytes) to /SD/sound.bin. On reconnect the drain thread
 * replays each record via sound_ble_notify_offline().
 *
 * Service UUID:  A1B2C3D4-E5F6-7890-ABCD-EF1234567890
 * Char UUID:     A1B2C3D4-E5F6-7890-ABCD-EF1234567891
 */

#include "sound_ble.h"
#include "sound.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <string.h>

// #if defined(CONFIG_SD_LOGGING)
#include "sd_log.h"
// #endif

LOG_MODULE_REGISTER(sound_ble, LOG_LEVEL_INF);

/* -- State ---------------------------------------------------------- */
static bool            snd_notify_enabled = false;
static struct bt_conn *snd_conn           = NULL;

/* -- GATT UUIDs ----------------------------------------------------- */
#define SOUND_SVC_UUID_BYTES \
    0x90, 0x78, 0x56, 0x34, 0x12, 0xEF, \
    0xCD, 0xAB, 0x90, 0x78, 0xF6, 0xE5, \
    0xD4, 0xC3, 0xB2, 0xA1

#define SOUND_CHR_UUID_BYTES \
    0x91, 0x78, 0x56, 0x34, 0x12, 0xEF, \
    0xCD, 0xAB, 0x90, 0x78, 0xF6, 0xE5, \
    0xD4, 0xC3, 0xB2, 0xA1

/* -- GATT callbacks ------------------------------------------------- */

static void snd_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    snd_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Sound spectrum notifications %s",
            snd_notify_enabled ? "enabled" : "disabled");
}

static uint8_t last_rms_buf[2] = {0xFF, 0x70};  /* default: -120.00 dBFS */

static ssize_t snd_read_handler(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             last_rms_buf, sizeof(last_rms_buf));
}

/* -- GATT service definition ---------------------------------------- */
BT_GATT_SERVICE_DEFINE(sound_svc,
    BT_GATT_PRIMARY_SERVICE(
        BT_UUID_DECLARE_128(SOUND_SVC_UUID_BYTES)),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(SOUND_CHR_UUID_BYTES),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ,
        snd_read_handler,
        NULL,
        NULL),

    BT_GATT_CCC(snd_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* -- Connection tracking -------------------------------------------- */
static void snd_connected(struct bt_conn *conn, uint8_t err)
{
    if (!err) {
        snd_conn = bt_conn_ref(conn);
    }
}

static void snd_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (snd_conn) {
        bt_conn_unref(snd_conn);
        snd_conn = NULL;
    }
    snd_notify_enabled = false;
}

static struct bt_conn_cb snd_conn_cb = {
    .connected    = snd_connected,
    .disconnected = snd_disconnected,
};

/* -- Packet buffer -------------------------------------------------- */
/* Static - never on stack. Packet 2: header(4) + 116x2 + rms(2) = 238 bytes */
static uint8_t snd_pkt_buf[SOUND_BLE_PKT_SIZE + 2U];  /* PKT_SIZE=(8+232)=240, +2 for last pkt rms */

/* Semaphore to signal when previous notify completed */
static K_SEM_DEFINE(snd_notify_sem, 0, 1);

static void snd_notify_cb(struct bt_conn *conn, void *user_data)
{
    k_sem_give(&snd_notify_sem);
    ARG_UNUSED(user_data);
}

void send_spectrum(const struct sound_spec_msg *spec) {
    struct bt_conn *conn = snd_conn;
    if (!snd_notify_enabled || !conn) return;

    last_rms_buf[0] = (spec->rms_dbfs_x100 >> 8) & 0xFF;
    last_rms_buf[1] =  spec->rms_dbfs_x100        & 0xFF;

    static struct bt_gatt_notify_params notify_params;

    for (uint8_t pkt = 0; pkt < SOUND_BLE_NUM_PKTS; pkt++) {
        uint32_t bin_start = pkt * SOUND_BLE_BINS_PER_PKT;
        uint32_t bin_end   = bin_start + SOUND_BLE_BINS_PER_PKT;
        if (bin_end > SOUND_NUM_BINS) bin_end = SOUND_NUM_BINS;
        uint32_t bin_count = bin_end - bin_start;

        size_t o = 0;
        snd_pkt_buf[o++] = pkt;
        snd_pkt_buf[o++] = SOUND_BLE_NUM_PKTS;
        snd_pkt_buf[o++] = (spec->utc_sec >> 24) & 0xFF;
        snd_pkt_buf[o++] = (spec->utc_sec >> 16) & 0xFF;
        snd_pkt_buf[o++] = (spec->utc_sec >>  8) & 0xFF;
        snd_pkt_buf[o++] =  spec->utc_sec         & 0xFF;
        snd_pkt_buf[o++] = (spec->utc_ms  >>  8) & 0xFF;
        snd_pkt_buf[o++] =  spec->utc_ms          & 0xFF;

        for (uint32_t i = 0; i < bin_count; i++) {
            uint16_t v = spec->bins[bin_start + i];
            snd_pkt_buf[o++] = (v >> 8) & 0xFF;
            snd_pkt_buf[o++] =  v        & 0xFF;
        }

        if (pkt == SOUND_BLE_NUM_PKTS - 1) {
            snd_pkt_buf[o++] = (spec->rms_dbfs_x100 >> 8) & 0xFF;
            snd_pkt_buf[o++] =  spec->rms_dbfs_x100        & 0xFF;
        }

        memset(&notify_params, 0, sizeof(notify_params));
        notify_params.attr = &sound_svc.attrs[1];
        notify_params.data = snd_pkt_buf;
        notify_params.len  = o;
        notify_params.func = snd_notify_cb;

        int err = bt_gatt_notify_cb(conn, &notify_params);
        if (err) {
            LOG_WRN("Sound notify pkt%u failed (%d)", pkt, err);
            break;
        }

        /* Wait for TX completion before sending next packet */
        if (k_sem_take(&snd_notify_sem, K_MSEC(200)) != 0) {
            LOG_WRN("Sound notify pkt%u timeout", pkt);
            break;
        }

        LOG_INF("Sound pkt%u sent (%zu bytes)", pkt, o);
    }
}

/* -- Sound BLE thread ----------------------------------------------- */
void sound_ble_thread(void) {

    bt_conn_cb_register(&snd_conn_cb);
    LOG_INF("Sound BLE thread ready -- waiting for spectrum data");
 
    while (1) {
        struct sound_spec_msg spec;
        if (k_msgq_get(&sound_spec_q, &spec, K_FOREVER) != 0) {
            continue;
        }
 
#if defined(CONFIG_SD_LOGGING)
        if (sd_log_is_draining()) {
            continue;
        }
#endif
 
        if (snd_notify_enabled && snd_conn) {
            if (spec.utc_sec > SD_LOG_UTC_MIN) {
                send_spectrum(&spec);
            } else {
#if defined(CONFIG_SD_LOGGING)
                /* Connected but no UTC sync — log to SD uptime file */
                sd_log_snd(&spec);
#endif
            }
        }
#if defined(CONFIG_SD_LOGGING)
        else {
            sd_log_snd(&spec);
        }
#endif
    }
}