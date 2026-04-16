/*
 * bluetooth.c — Combined BLE Central Gateway (WROVER WiFi build)
 *
 * Subscribes to per-modality BLE characteristics from sensor nodes.
 * Each notify handler decodes the utc_sec timestamp from bytes [0-3]
 * then the modality payload from bytes [4+].
 *
 * Payload lengths (all include 4-byte uptime_ms prefix):
 *   BME: 12  (4 + temp(2) + rh(2) + press(4))
 *   ENS:  9  (4 + eco2(2) + tvoc(2) + aqi(1))
 *   AS7: 30  (4 + 13×uint16)
 *   MST:  6  (4 + vwc(2))
 *   BAT:  9  (4 + mV(2) + pct(1) + rate(2))
 *
 * Bytes [0-3] now carry utc_sec (UTC seconds at measurement)
 * rather than uptime_ms. The sensor node stamps this via time_sync_get_utc().
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "bluetooth.h"
#include "azure_mqtt.h"
#include "my_json.h"
#include "time_sync_writer.h"

#define DEBUG_PRINT_RAW 0
#define INVALID        -1
#define MSGQ_MAX_MSGS   4

/* ── Payload lengths ─────────────────────────────────────── */
#define CUR_PAYLOAD_LEN   10   /* utc_sec(4)+utc_ms(2)+current_uA(2)+voltage_mV(2) */
#define BME_PAYLOAD_LEN  14  /* +2 for utc_ms */
#define ENS_PAYLOAD_LEN  11  /* +2 for utc_ms */
#define AS7_PAYLOAD_LEN  32  /* +2 for utc_ms */
#define MST_PAYLOAD_LEN   8  /* +2 for utc_ms */
#define BAT_PAYLOAD_LEN  12  /* +2 for utc_ms */

#define SOUND_NUM_BINS     348
#define SOUND_BINS_PER_PKT 116
#define SOUND_NUM_PKTS     3
#define SOUND_HDR_SIZE     8   /* pkt_id(1)+total(1)+utc_sec(4)+utc_ms(2) */

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

/* ═══════════════════════════════════════════════════════════
 * UUIDs
 * ═══════════════════════════════════════════════════════════ */

static struct bt_uuid_128 tracker_service_uuid = BT_UUID_INIT_128(
    0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78,
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);

static struct bt_uuid_128 tracker_char_uuid = BT_UUID_INIT_128(
    0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
    0x11, 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa);

static struct bt_uuid_128 sound_char_uuid = BT_UUID_INIT_128(
    0x91, 0x78, 0x56, 0x34, 0x12, 0xEF,
    0xCD, 0xAB, 0x90, 0x78, 0xF6, 0xE5,
    0xD4, 0xC3, 0xB2, 0xA1);

/* Modality UUIDs — char handles only, match sensor node definitions */
static struct bt_uuid_128 bme_char_uuid = BT_UUID_INIT_128(
    0xB0, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00,
    0x02, 0x00, 0x00, 0xB0);

static struct bt_uuid_128 ens_char_uuid = BT_UUID_INIT_128(
    0xE1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00,
    0x02, 0x00, 0x00, 0xE1);

static struct bt_uuid_128 as7_char_uuid = BT_UUID_INIT_128(
    0xA7, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00,
    0x02, 0x00, 0x00, 0xA7);

static struct bt_uuid_128 mst_char_uuid = BT_UUID_INIT_128(
    0xC1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00,
    0x02, 0x00, 0x00, 0xC1);

static struct bt_uuid_128 bat_char_uuid = BT_UUID_INIT_128(
    0xBA, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00,
    0x02, 0x00, 0x00, 0xBA);

static struct bt_uuid_128 cur_char_uuid = BT_UUID_INIT_128(
    0xCC, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6,
    0x78, 0x90, 0xCD, 0xAB, 0x00, 0x00,
    0x02, 0x00, 0x00, 0xCC);

static struct bt_uuid_16 cccd_uuid = BT_UUID_INIT_16(0x2902);

/* ═══════════════════════════════════════════════════════════
 * Per-connection state
 * ═══════════════════════════════════════════════════════════ */
static struct bt_conn *conns[MAX_CONN];

#define DECL_MOD(name) \
    static struct bt_gatt_subscribe_params name##_subs[MAX_CONN]; \
    static uint16_t                        name##_char_handles[MAX_CONN]; \
    static uint16_t                        name##_ccc_handles[MAX_CONN]; \
    static struct bt_gatt_discover_params  name##_disc[MAX_CONN]; \
    static struct bt_gatt_discover_params  name##_cccd_disc[MAX_CONN]

DECL_MOD(sensor);
DECL_MOD(sound);
DECL_MOD(bme);
DECL_MOD(ens);
DECL_MOD(as7);
DECL_MOD(mst);
DECL_MOD(bat);
DECL_MOD(cur);

/* Environment accumulator — merge BME + ENS before enqueuing */
static mod_env_t env_acc[MAX_CONN];
static bool      env_bme_ready[MAX_CONN];
static bool      env_ens_ready[MAX_CONN];

/* Sound reassembly */
struct sound_reassembly {
    uint16_t bins[SOUND_NUM_BINS];
    int16_t  rms_dbfs_x100;
    uint8_t  received;
    uint32_t utc_sec;      /* reassembly key — UTC seconds from sensor node */
    uint16_t utc_ms;       /* UTC milliseconds 0-999 */
};
static struct sound_reassembly sound_rx[MAX_CONN];

/* ── Message queues ─────────────────────────────────────── */
K_MSGQ_DEFINE(env_msgq, sizeof(mod_env_t),  MSGQ_MAX_MSGS, 4);
K_MSGQ_DEFINE(as7_msgq, sizeof(mod_spec_t), MSGQ_MAX_MSGS, 4);
K_MSGQ_DEFINE(mst_msgq, sizeof(mod_mst_t),  MSGQ_MAX_MSGS, 4);
K_MSGQ_DEFINE(bat_msgq, sizeof(mod_bat_t),  MSGQ_MAX_MSGS, 4);
K_MSGQ_DEFINE(cur_msgq,   sizeof(mod_cur_t),                MSGQ_MAX_MSGS, 4);

struct sound_queue_pkt_t { mod_snd_t sp; };
K_MSGQ_DEFINE(sound_msgq, sizeof(struct sound_queue_pkt_t), 2, 4);

/* ── Forward declarations ───────────────────────────────── */
static int  get_conn_index(struct bt_conn *conn);
static bool is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid);
static void start_scan(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                          struct net_buf_simple *ad);
static void discover_sensor_char(struct bt_conn *conn, int index);
static void discover_sound_char(struct bt_conn *conn, int index);
static void discover_bme_char(struct bt_conn *conn, int index);
static void discover_ens_char(struct bt_conn *conn, int index);
static void discover_as7_char(struct bt_conn *conn, int index);
static void discover_mst_char(struct bt_conn *conn, int index);
static void discover_bat_char(struct bt_conn *conn, int index);
static void discover_cur_char(struct bt_conn *conn, int index);
static void discover_done(struct bt_conn *conn, int index);

/* ═══════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════ */

static int get_conn_index(struct bt_conn *conn)
{
    for (int i = 0; i < MAX_CONN; i++) {
        if (conns[i] == conn) return i;
    }
    return INVALID;
}

static bool is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid)
{
    size_t offset = 0;
    while (offset < ad->len) {
        uint8_t len  = ad->data[offset];
        if (len == 0 || (offset + len) >= ad->len) break;
        uint8_t type = ad->data[offset + 1];
        const uint8_t *data = &ad->data[offset + 2];
        uint8_t data_len = len - 1;
        if (type == BT_DATA_UUID128_ALL || type == BT_DATA_UUID128_SOME) {
            for (size_t i = 0; i + 16 <= data_len; i += 16) {
                struct bt_uuid_128 adv_uuid;
                memcpy(adv_uuid.val, &data[i], 16);
                adv_uuid.uuid.type = BT_UUID_TYPE_128;
                if (!bt_uuid_cmp(uuid, &adv_uuid.uuid)) return true;
            }
        }
        offset += len + 1;
    }
    return false;
}

/* ── Big-endian decode helpers ──────────────────────────── */
static inline uint32_t be32(const uint8_t *b)
{
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
           ((uint32_t)b[2]<<8)| (uint32_t)b[3];
}
static inline uint16_t be16(const uint8_t *b)
{
    return ((uint16_t)b[0] << 8) | b[1];
}
static inline int16_t be16s(const uint8_t *b)
{
    return (int16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static inline int32_t be32s(const uint8_t *b)
{
    return (int32_t)(((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
                     ((uint32_t)b[2]<<8)| (uint32_t)b[3]);
}

/* ═══════════════════════════════════════════════════════════
 * NOTIFY HANDLERS
 *
 * All payloads begin with 4 bytes of utc_sec (big-endian uint32)
 * stamped on the sensor node at measurement via time_sync_get_utc().
 * Value is 0 if the sensor node has not yet received a time sync.
 * ═══════════════════════════════════════════════════════════ */

/* ── BME280 (12 bytes: uptime(4) + temp(2) + rh(2) + press(4)) ──────── */
static uint8_t bme_notify_func(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    if (!data || length != BME_PAYLOAD_LEN) return BT_GATT_ITER_CONTINUE;
    int idx = get_conn_index(conn);
    if (idx < 0) return BT_GATT_ITER_CONTINUE;

    const uint8_t *d = data;
    env_acc[idx].utc_sec          = be32(d + 0);
    env_acc[idx].utc_ms           = be16(d + 4);   /* NEW */
    env_acc[idx].temp_c_x100      = be16s(d + 6);
    env_acc[idx].rh_x100          = be16s(d + 8);
    env_acc[idx].press_hPa_x1000  = be32s(d + 10);
    env_bme_ready[idx] = true;

    if (env_ens_ready[idx]) {
        env_ens_ready[idx] = false;
        env_bme_ready[idx] = false;
        if (k_msgq_put(&env_msgq, &env_acc[idx], K_NO_WAIT) != 0) {
            LOG_WRN("[BME %d] env queue full", idx);
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ── ENS160 (9 bytes: uptime(4) + eco2(2) + tvoc(2) + aqi(1)) ───────── */
static uint8_t ens_notify_func(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    if (!data || length != ENS_PAYLOAD_LEN) return BT_GATT_ITER_CONTINUE;
    int idx = get_conn_index(conn);
    if (idx < 0) return BT_GATT_ITER_CONTINUE;

    const uint8_t *d = data;
    /* Use ENS utc_sec only if BME hasn't arrived yet */
    if (!env_bme_ready[idx]) {
        env_acc[idx].utc_sec = be32(d + 0);
    }
    env_acc[idx].eco2_ppm = be16(d + 4);
    env_acc[idx].tvoc_ppb = be16(d + 6);
    env_acc[idx].aqi      = d[8];
    env_ens_ready[idx] = true;

    if (env_bme_ready[idx]) {
        env_bme_ready[idx] = false;
        env_ens_ready[idx] = false;
        if (k_msgq_put(&env_msgq, &env_acc[idx], K_NO_WAIT) != 0) {
            LOG_WRN("[ENS %d] env queue full", idx);
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ── AS7343 (30 bytes: uptime(4) + 13×uint16(26)) ────────────────────── */
static uint8_t as7_notify_func(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    if (!data || length != AS7_PAYLOAD_LEN) return BT_GATT_ITER_CONTINUE;
    int idx = get_conn_index(conn);
    if (idx < 0) return BT_GATT_ITER_CONTINUE;

    const uint8_t *d = data;
    mod_spec_t msg;
    msg.dev_id  = (uint8_t)idx;
    msg.utc_sec = be32(d + 0);
    msg.utc_ms  = be16(d + 4);   /* NEW */
    for (int i = 0; i < AS7343_NUM_CH; i++) {
        msg.ch[i] = be16(d + 6 + i * 2);
    }

    if (k_msgq_put(&as7_msgq, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("[AS7 %d] queue full", idx);
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ── Moisture (6 bytes: uptime(4) + vwc(2)) ──────────────────────────── */
static uint8_t mst_notify_func(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    if (!data || length != MST_PAYLOAD_LEN) return BT_GATT_ITER_CONTINUE;
    int idx = get_conn_index(conn);
    if (idx < 0) return BT_GATT_ITER_CONTINUE;

    const uint8_t *d = data;
    mod_mst_t msg;
    msg.dev_id   = (uint8_t)idx;
    msg.utc_sec  = be32(d + 0);
    msg.utc_ms   = be16(d + 4);   /* NEW */
    msg.vwc_x100 = be16(d + 6);

    if (k_msgq_put(&mst_msgq, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("[MST %d] queue full", idx);
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ── Battery (9 bytes: uptime(4) + mV(2) + pct(1) + rate(2)) ────────── */
static uint8_t bat_notify_func(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    if (!data || length != BAT_PAYLOAD_LEN) return BT_GATT_ITER_CONTINUE;
    int idx = get_conn_index(conn);
    if (idx < 0) return BT_GATT_ITER_CONTINUE;
 
    const uint8_t *d = data;
    mod_bat_t msg;
    msg.utc_sec  = be32(d + 0);
    msg.utc_ms   = be16(d + 4);
    msg.mV       = be16(d + 6);
    msg.pct      = d[8];
    msg.rate_x10 = be16s(d + 9);
    msg.dev_id   = d[11];          /* dev_id from sensor node — identifies which node */
 
    if (k_msgq_put(&bat_msgq, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("[BAT %d] queue full", idx);
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ── Current sensor (10 bytes: utc_sec(4)+utc_ms(2)+current_uA(2)+voltage_mV(2)) ── */
static uint8_t cur_notify_func(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    if (!data || length != CUR_PAYLOAD_LEN) return BT_GATT_ITER_CONTINUE;
    int idx = get_conn_index(conn);
    if (idx < 0) return BT_GATT_ITER_CONTINUE;
 
    const uint8_t *d = data;
    mod_cur_t msg;
    msg.dev_id      = (uint8_t)idx;
    msg.utc_sec     = be32(d + 0);
    msg.utc_ms      = be16(d + 4);
    msg.current_uA  = be16s(d + 6);
    msg.voltage_mV  = be16(d + 8);
 
    if (k_msgq_put(&cur_msgq, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("[CUR %d] queue full", idx);
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ── Sound spectrum (existing reassembly — unchanged) ────────────────── */
static uint8_t sound_notify_func(struct bt_conn *conn,
                                  struct bt_gatt_subscribe_params *params,
                                  const void *data, uint16_t length)
{
    if (!data || length < SOUND_HDR_SIZE) return BT_GATT_ITER_CONTINUE;
    int index = get_conn_index(conn);
    if (index < 0) return BT_GATT_ITER_CONTINUE;

    const uint8_t *buf = (const uint8_t *)data;
    uint8_t  pkt_id  = buf[0];
    uint8_t  total   = buf[1];
    uint32_t utc_sec = be32(buf + 2);  /* 4 bytes */
    uint16_t utc_ms  = be16(buf + 6);  /* 2 bytes — NEW */

    if (pkt_id >= SOUND_NUM_PKTS || total != SOUND_NUM_PKTS) {
        return BT_GATT_ITER_CONTINUE;
    }

    struct sound_reassembly *rx = &sound_rx[index];
    if (pkt_id == 0 || utc_sec != rx->utc_sec || utc_ms != rx->utc_ms) {
        memset(rx, 0, sizeof(*rx));
        rx->utc_sec = utc_sec;
        rx->utc_ms  = utc_ms;
    }
    if (rx->received & (1u << pkt_id)) return BT_GATT_ITER_CONTINUE;

    uint32_t bin_start     = pkt_id * SOUND_BINS_PER_PKT;
    const uint8_t *payload = buf + SOUND_HDR_SIZE;
    uint16_t payload_len   = length - SOUND_HDR_SIZE;
    uint16_t bin_bytes     = (pkt_id == SOUND_NUM_PKTS - 1)
                             ? payload_len - 2 : payload_len;
    uint32_t bin_count     = bin_bytes / 2;

    for (uint32_t i = 0; i < bin_count && (bin_start + i) < SOUND_NUM_BINS; i++) {
        rx->bins[bin_start + i] = be16(payload + i * 2);
    }
    if (pkt_id == SOUND_NUM_PKTS - 1 && payload_len >= 2) {
        rx->rms_dbfs_x100 = be16s(payload + payload_len - 2);
    }

    rx->received |= (1u << pkt_id);

    if (rx->received == 0x07) {
        struct sound_queue_pkt_t qpkt;
        memset(&qpkt, 0, sizeof(qpkt));
        qpkt.sp.dev_id        = (uint8_t)index;
        qpkt.sp.utc_sec       = rx->utc_sec;   /* UTC from sensor node */
        qpkt.sp.utc_ms        = rx->utc_ms;    /* UTC ms — NEW */
        qpkt.sp.rms_dbfs_x100 = rx->rms_dbfs_x100;
        memcpy(qpkt.sp.bins, rx->bins, sizeof(rx->bins));
        if (k_msgq_put(&sound_msgq, &qpkt, K_NO_WAIT) != 0) {
            LOG_WRN("[SOUND %d] queue full", index);
        }
        rx->received = 0;
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════
 * DISCOVERY CHAIN
 * sensor → sound → bme → ens → as7 → mst → bat → done
 * ═══════════════════════════════════════════════════════════ */

#define MODALITY_DISC_FUNCS(name, next_fn, notify_fn)                        \
static uint8_t name##_cccd_disc_func(struct bt_conn *conn,                   \
                                      const struct bt_gatt_attr *attr,        \
                                      struct bt_gatt_discover_params *params) \
{                                                                             \
    int index = get_conn_index(conn);                                         \
    if (!attr) {                                                              \
        LOG_ERR("[" #name " %d] CCCD not found", index);                     \
        next_fn(conn, index);                                                 \
        return BT_GATT_ITER_STOP;                                             \
    }                                                                         \
    name##_ccc_handles[index]       = attr->handle;                          \
    name##_subs[index].ccc_handle   = name##_ccc_handles[index];             \
    name##_subs[index].value_handle = name##_char_handles[index];            \
    name##_subs[index].notify       = notify_fn;                             \
    name##_subs[index].value        = BT_GATT_CCC_NOTIFY;                   \
    int err = bt_gatt_subscribe(conn, &name##_subs[index]);                  \
    if (err) LOG_ERR("[" #name " %d] subscribe failed (%d)", index, err);    \
    else     LOG_INF("[" #name " %d] subscribed", index);                    \
    next_fn(conn, index);                                                     \
    return BT_GATT_ITER_STOP;                                                 \
}                                                                             \
static uint8_t name##_disc_func(struct bt_conn *conn,                        \
                                 const struct bt_gatt_attr *attr,             \
                                 struct bt_gatt_discover_params *params)      \
{                                                                             \
    int index = get_conn_index(conn);                                         \
    if (!attr) {                                                              \
        LOG_ERR("[" #name " %d] char not found", index);                     \
        next_fn(conn, index);                                                 \
        return BT_GATT_ITER_STOP;                                             \
    }                                                                         \
    const struct bt_gatt_chrc *chrc = attr->user_data;                      \
    name##_char_handles[index] = chrc->value_handle;                         \
    name##_cccd_disc[index].uuid         = &cccd_uuid.uuid;                  \
    name##_cccd_disc[index].start_handle = name##_char_handles[index];       \
    name##_cccd_disc[index].end_handle   = 0xffff;                           \
    name##_cccd_disc[index].type         = BT_GATT_DISCOVER_ATTRIBUTE;       \
    name##_cccd_disc[index].func         = name##_cccd_disc_func;            \
    int err = bt_gatt_discover(conn, &name##_cccd_disc[index]);              \
    if (err) LOG_ERR("[" #name " %d] cccd discover failed (%d)", index, err);\
    return BT_GATT_ITER_STOP;                                                 \
}                                                                             \
static void discover_##name##_char(struct bt_conn *conn, int index)          \
{                                                                             \
    name##_disc[index].uuid         = &name##_char_uuid.uuid;                \
    name##_disc[index].func         = name##_disc_func;                      \
    name##_disc[index].start_handle = 0x0001;                                \
    name##_disc[index].end_handle   = 0xffff;                                \
    name##_disc[index].type         = BT_GATT_DISCOVER_CHARACTERISTIC;       \
    int err = bt_gatt_discover(conn, &name##_disc[index]);                   \
    if (err) LOG_ERR("[" #name " %d] discover failed (%d)", index, err);     \
}

/* ── Sensor (chains to sound, no notify — time sync write target only) ── */
static uint8_t sensor_cccd_disc_func(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (!attr) {
        LOG_ERR("[SENSOR %d] CCCD not found", index);
        return BT_GATT_ITER_STOP;
    }
    sensor_ccc_handles[index]       = attr->handle;
    sensor_subs[index].ccc_handle   = sensor_ccc_handles[index];
    sensor_subs[index].value_handle = sensor_char_handles[index];
    sensor_subs[index].notify       = NULL;
    sensor_subs[index].value        = BT_GATT_CCC_NOTIFY;

    time_sync_writer_send(conn, index, sensor_char_handles[index]);
    discover_sound_char(conn, index);
    return BT_GATT_ITER_STOP;
}

static uint8_t sensor_disc_func(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (!attr) { return BT_GATT_ITER_STOP; }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    sensor_char_handles[index] = chrc->value_handle;
    sensor_cccd_disc[index].uuid         = &cccd_uuid.uuid;
    sensor_cccd_disc[index].start_handle = sensor_char_handles[index];
    sensor_cccd_disc[index].end_handle   = 0xffff;
    sensor_cccd_disc[index].type         = BT_GATT_DISCOVER_ATTRIBUTE;
    sensor_cccd_disc[index].func         = sensor_cccd_disc_func;
    bt_gatt_discover(conn, &sensor_cccd_disc[index]);
    return BT_GATT_ITER_STOP;
}

static void discover_sensor_char(struct bt_conn *conn, int index)
{
    sensor_disc[index].uuid         = &tracker_char_uuid.uuid;
    sensor_disc[index].func         = sensor_disc_func;
    sensor_disc[index].start_handle = 0x0001;
    sensor_disc[index].end_handle   = 0xffff;
    sensor_disc[index].type         = BT_GATT_DISCOVER_CHARACTERISTIC;
    int err = bt_gatt_discover(conn, &sensor_disc[index]);
    if (err) LOG_ERR("[SENSOR %d] discover failed (%d)", index, err);
}

static void discover_done(struct bt_conn *conn, int index)
{
    LOG_INF("[BASE %d] Discovery chain complete", index);
}

/* Expand modality discovery triplets — chain order: sound→bme→ens→as7→mst→bat→done */
MODALITY_DISC_FUNCS(sound, discover_bme_char,  sound_notify_func)
MODALITY_DISC_FUNCS(bme,   discover_ens_char,  bme_notify_func)
MODALITY_DISC_FUNCS(ens,   discover_as7_char,  ens_notify_func)
MODALITY_DISC_FUNCS(as7,   discover_mst_char,  as7_notify_func)
MODALITY_DISC_FUNCS(mst,   discover_bat_char,  mst_notify_func)
MODALITY_DISC_FUNCS(bat,   discover_cur_char,  bat_notify_func)
MODALITY_DISC_FUNCS(cur,   discover_done,      cur_notify_func)

/* ═══════════════════════════════════════════════════════════
 * Scan and connection management
 * ═══════════════════════════════════════════════════════════ */

static void start_scan(void)
{
    struct bt_le_scan_param scan_params = {
        .type     = BT_HCI_LE_SCAN_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window   = BT_GAP_SCAN_FAST_WINDOW,
    };
    int err = bt_le_scan_start(&scan_params, device_found);
    if (err) LOG_ERR("[BASE] Scan failed (%d)", err);
    else     LOG_INF("[BASE] Scanning...");
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                          struct net_buf_simple *ad)
{
    if (!azure_mqtt_is_connected()) {
        return;  /* ignore advertisements until MQTT is back */
    }
    
    if (type != BT_GAP_ADV_TYPE_ADV_IND &&
        type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) return;
    if (!is_uuid_in_ad(ad, &tracker_service_uuid.uuid)) return;

    struct bt_conn *existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
    if (existing) { bt_conn_unref(existing); return; }

    int slot = -1;
    for (int i = 0; i < MAX_CONN; i++) {
        if (!conns[i]) { slot = i; break; }
    }
    if (slot < 0) return;

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INF("[BASE] Found tracker: %s (RSSI %d)", addr_str, rssi);

    bt_le_scan_stop();
    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                                BT_LE_CONN_PARAM_DEFAULT, &conns[slot]);
    if (err) { LOG_ERR("[BASE] Connect failed (%d)", err); start_scan(); }
}

static struct bt_gatt_exchange_params mtu_params[MAX_CONN];

static void exchange_func(struct bt_conn *conn, uint8_t err,
                           struct bt_gatt_exchange_params *params)
{
    if (err) LOG_ERR("MTU exchange failed (%u)", err);
    else     LOG_INF("MTU: %d bytes", bt_gatt_get_mtu(conn));
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        LOG_ERR("[BASE] Connect failed (err %u)", err);
        bt_conn_unref(conn);
        start_scan();
        return;
    }

    /* Don't establish BLE connections if MQTT is not up */
    if (!azure_mqtt_is_connected()) {
        LOG_WRN("[BASE] MQTT not connected — rejecting BLE connection");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        start_scan();
        return;
    }

    int index = get_conn_index(conn);
    if (index < 0) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        start_scan();
        return;
    }

    LOG_INF("[BASE] Connected [%d]: %s", index, addr);

    memset(&sound_rx[index], 0, sizeof(sound_rx[index]));
    memset(&env_acc[index],  0, sizeof(env_acc[index]));
    env_bme_ready[index] = false;
    env_ens_ready[index] = false;
    env_acc[index].dev_id = (uint8_t)index;

    mtu_params[index].func = exchange_func;
    bt_gatt_exchange_mtu(conn, &mtu_params[index]);

    discover_sensor_char(conn, index);
    start_scan();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    int index = get_conn_index(conn);
    LOG_INF("[BASE] Disconnected [%d] (reason 0x%02x)", index, reason);
    if (index >= 0) {
        bt_conn_unref(conns[index]);
        conns[index]               = NULL;
        sensor_char_handles[index] = 0;
        sound_char_handles[index]  = 0;
        bme_char_handles[index]    = 0;
        ens_char_handles[index]    = 0;
        as7_char_handles[index]    = 0;
        mst_char_handles[index]    = 0;
        bat_char_handles[index]    = 0;
        cur_char_handles[index]    = 0;
        env_bme_ready[index]       = false;
        env_ens_ready[index]       = false;
        memset(&sound_rx[index], 0, sizeof(sound_rx[index]));
    }
    start_scan();
}

static struct bt_conn_cb conn_callbacks = {
    .connected    = connected,
    .disconnected = disconnected,
};

/* ═══════════════════════════════════════════════════════════
 * process_data_thread
 * ═══════════════════════════════════════════════════════════ */

#if defined(CONFIG_ESP_SPIRAM)
#  define PROCESS_STACK_SIZE  6144
Z_KERNEL_STACK_DEFINE_IN(process_stack, PROCESS_STACK_SIZE,
    __attribute__((section(".ext_ram.bss"))));
#else
#  define PROCESS_STACK_SIZE  4096
K_THREAD_STACK_DEFINE(process_stack, PROCESS_STACK_SIZE);
#endif

#if defined(CONFIG_ESP_SPIRAM)
static char s_cur_json[JSON_CUR_BUF_SIZE]  __attribute__((section(".ext_ram.bss")));
static char s_env_json[JSON_ENV_BUF_SIZE]  __attribute__((section(".ext_ram.bss")));
static char s_as7_json[JSON_SPEC_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static char s_mst_json[JSON_MST_BUF_SIZE]  __attribute__((section(".ext_ram.bss")));
static char s_bat_json[JSON_BAT_BUF_SIZE]  __attribute__((section(".ext_ram.bss")));
static char s_snd_json[JSON_SND_BUF_SIZE]  __attribute__((section(".ext_ram.bss")));
#else
static char s_env_json[JSON_ENV_BUF_SIZE];
static char s_as7_json[JSON_SPEC_BUF_SIZE];
static char s_mst_json[JSON_MST_BUF_SIZE];
static char s_bat_json[JSON_BAT_BUF_SIZE];
static char s_snd_json[JSON_SND_BUF_SIZE];
static char s_cur_json[JSON_CUR_BUF_SIZE];
#endif

static uint8_t sound_throttle = 0;

void process_data_thread(void)
{
    uint32_t last_timesync_ms = 0;

    while (1) {

        /* ── MQTT connectivity guard ──────────────────────────── */
        if (!azure_mqtt_is_connected()) {
            for (int i = 0; i < MAX_CONN; i++) {
                if (conns[i]) {
                    LOG_WRN("MQTT down — disconnecting BLE node %d", i);
                    bt_conn_disconnect(conns[i],
                        BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                }
            }
            k_sleep(K_SECONDS(5));
            continue;
        }

        /* ── Periodic time sync ───────────────────────────── */
        uint32_t now = k_uptime_get_32();
        if ((now - last_timesync_ms) >= (TIMESYNC_INTERVAL_S * 1000U)
            && time_sync_writer_has_utc())
        {
            for (int i = 0; i < MAX_CONN; i++) {
                if (conns[i] && sensor_char_handles[i] != 0) {
                    time_sync_writer_send(conns[i], i, sensor_char_handles[i]);
                }
            }
            last_timesync_ms = now;
        }

        /* ── Environment ──────────────────────────────────── */
        mod_env_t env_msg;
        if (k_msgq_get(&env_msgq, &env_msg, K_MSEC(5)) == 0) {
            int ret = json_encode_env(&env_msg, s_env_json, sizeof(s_env_json));
            if (ret > 0) {
                LOG_INF("env JSON");
                // LOG_INF("env JSON: %s", s_env_JSON);
                azure_mqtt_publish(s_env_json);
            } else {
                LOG_ERR("env JSON encode failed (%d)", ret);
            }
        }

        /* ── AS7343 spectrum ──────────────────────────────── */
        mod_spec_t as7_msg;
        if (k_msgq_get(&as7_msgq, &as7_msg, K_MSEC(5)) == 0) {
            int ret = json_encode_spec(&as7_msg, s_as7_json, sizeof(s_as7_json));
            if (ret > 0) {
                LOG_INF("as7 JSON");
                // LOG_INF("as7 JSON: %s", s_as7_json);
                azure_mqtt_publish(s_as7_json);
            } else {
                LOG_ERR("spec JSON encode failed (%d)", ret);
            }
        }

        /* ── Soil moisture ────────────────────────────────── */
        mod_mst_t mst_msg;
        if (k_msgq_get(&mst_msgq, &mst_msg, K_MSEC(5)) == 0) {
            int ret = json_encode_mst(&mst_msg, s_mst_json, sizeof(s_mst_json));
            if (ret > 0) {
                LOG_INF("mst JSON: %s", s_mst_json);
                azure_mqtt_publish(s_mst_json);
            } else {
                LOG_ERR("mst JSON encode failed (%d)", ret);
            }
        }

        /* ── Battery ──────────────────────────────────────── */
        mod_bat_t bat_msg;
        if (k_msgq_get(&bat_msgq, &bat_msg, K_MSEC(5)) == 0) {
            int ret = json_encode_bat(&bat_msg, s_bat_json, sizeof(s_bat_json));
            if (ret > 0) {
                LOG_INF("bat JSON: %s", s_bat_json);
                azure_mqtt_publish(s_bat_json);
            } else {
                LOG_ERR("bat JSON encode failed (%d)", ret);
            }
        }

        /* ── Sound spectrum (throttled 1:10) ─────────────── */
        struct sound_queue_pkt_t sqpkt;
        if (k_msgq_get(&sound_msgq, &sqpkt, K_MSEC(5)) == 0) {
            if (++sound_throttle >= 10) {
                sound_throttle = 0;
                int ret = json_encode_snd(&sqpkt.sp, s_snd_json, sizeof(s_snd_json));
                if (ret > 0) {
                    // LOG_INF("snd JSON: %s", s_snd_json);
                    LOG_INF("snd JSON");
                    azure_mqtt_publish(s_snd_json);
                } else {
                    LOG_ERR("snd JSON encode failed (%d)", ret);
                }
            }
        }
        /* ── Current sensor ───────────────────────────────── */
        mod_cur_t cur_msg;
        if (k_msgq_get(&cur_msgq, &cur_msg, K_MSEC(5)) == 0) {
            int ret = json_encode_cur(&cur_msg, s_cur_json, sizeof(s_cur_json));
            if (ret > 0) {
                LOG_INF("cur JSON: %s", s_cur_json);
                azure_mqtt_publish(s_cur_json);
            } else {
                LOG_ERR("cur JSON encode failed (%d)", ret);
            }
        }
    }
}

void base_thread(void)
{
    int err = bt_enable(NULL);
    if (err) {
        LOG_ERR("[BASE] Bluetooth init failed (err %d)", err);
        return;
    }
    LOG_INF("[BASE] Bluetooth initialized");
    bt_conn_cb_register(&conn_callbacks);
    start_scan();
}