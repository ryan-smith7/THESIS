/*
 * bluetooth.c — Combined BLE Central Gateway
 *
 * Supports two hardware platforms selected at build time:
 *
 *   CONFIG_ESP_SPIRAM=y   →  ESP32-WROVER  (SPIRAM available)
 *   CONFIG_ESP_SPIRAM=n   →  ESP32-POE     (internal DRAM only)
 *
 * Receives two types of BLE GATT notifications from each sensor node:
 *
 *   1. Main sensor characteristic (61 bytes):
 *      BME280 + ENS160 + AS7343 + sound summary + soil VWC
 *
 *   2. Sound spectrum characteristic (3 × ~236 byte chunks):
 *      348-bin FFT spectrum, reassembled from 3 packets (698 bytes total)
 *
 * process_data_thread decodes each packet, encodes JSON, and publishes
 * directly via azure_mqtt_publish(). No UART bridge on either platform.
 *
 * Time sync: on each new node connection the gateway immediately writes
 * the current UTC to the node's characteristic, then repeats every
 * TIMESYNC_INTERVAL_S seconds via process_data_thread().
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

/* Set to 1 to log decoded values to serial instead of publishing */
#define DEBUG_PRINT_RAW 0

#define INVALID       -1
#define MSGQ_MAX_MSGS  8

#define DUMMY 600

/* ── Packet lengths ─────────────────────────────────────── */
#define SENSOR_PACKED_LEN   61
#define SOUND_NUM_BINS      348
#define SOUND_BINS_PER_PKT  116
#define SOUND_NUM_PKTS      3
#define SOUND_HDR_SIZE      4     /* pkt_id(1) + total(1) + timestamp16(2) */

/* Assembled sound payload reused as intermediate binary buffer */
#define SOUND_BIN_BUF_LEN   (2 + SOUND_NUM_BINS * 2)   /* rms(2) + bins(696) */

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

/* ── UUIDs ──────────────────────────────────────────────── */

static struct bt_uuid_128 tracker_service_uuid = BT_UUID_INIT_128(
    0x12, 0x34, 0x56, 0x78,
    0x12, 0x34, 0x56, 0x78,
    0x12, 0x34, 0x56, 0x78,
    0x9a, 0xbc, 0xde, 0xf0);

static struct bt_uuid_128 tracker_char_uuid = BT_UUID_INIT_128(
    0x99, 0x88, 0x77, 0x66,
    0x55, 0x44, 0x33, 0x22,
    0x11, 0x00, 0xff, 0xee,
    0xdd, 0xcc, 0xbb, 0xaa);

static struct bt_uuid_128 sound_service_uuid = BT_UUID_INIT_128(
    0x90, 0x78, 0x56, 0x34, 0x12, 0xEF,
    0xCD, 0xAB, 0x90, 0x78, 0xF6, 0xE5,
    0xD4, 0xC3, 0xB2, 0xA1);

static struct bt_uuid_128 sound_char_uuid = BT_UUID_INIT_128(
    0x91, 0x78, 0x56, 0x34, 0x12, 0xEF,
    0xCD, 0xAB, 0x90, 0x78, 0xF6, 0xE5,
    0xD4, 0xC3, 0xB2, 0xA1);

static struct bt_uuid_16 cccd_uuid = BT_UUID_INIT_16(0x2902);

/* ── Per-connection state ───────────────────────────────── */
static struct bt_conn *conns[MAX_CONN];

static struct bt_gatt_subscribe_params  sensor_subs[MAX_CONN];
static uint16_t                         sensor_char_handles[MAX_CONN];
static uint16_t                         sensor_ccc_handles[MAX_CONN];
static struct bt_gatt_discover_params   sensor_disc[MAX_CONN];
static struct bt_gatt_discover_params   sensor_cccd_disc[MAX_CONN];

static struct bt_gatt_subscribe_params  sound_subs[MAX_CONN];
static uint16_t                         sound_char_handles[MAX_CONN];
static uint16_t                         sound_ccc_handles[MAX_CONN];
static struct bt_gatt_discover_params   sound_disc[MAX_CONN];
static struct bt_gatt_discover_params   sound_cccd_disc[MAX_CONN];

/* ── Sound spectrum reassembly ──────────────────────────── */
struct sound_reassembly {
    uint16_t bins[SOUND_NUM_BINS];
    int16_t  rms_dbfs_x100;
    uint8_t  received;
    uint16_t timestamp16;
};

static struct sound_reassembly sound_rx[MAX_CONN];

/* ── Message queues ─────────────────────────────────────── */
struct sensor_packet_t { uint8_t data[SENSOR_PACKED_LEN]; };

/*
 * Sound queue carries the decoded sound_payload_t directly —
 * no intermediate binary packing needed since we no longer go via UART.
 */
struct sound_queue_pkt_t {
    sound_payload_t sp;
};

K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_packet_t),  MSGQ_MAX_MSGS, 4);
K_MSGQ_DEFINE(sound_msgq,  sizeof(struct sound_queue_pkt_t), 2, 4);  /* 2 deep — each slot is 703 bytes */

/* ── Forward declarations ───────────────────────────────── */
static int   get_conn_index(struct bt_conn *conn);
static bool  is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid);

static uint8_t sensor_notify_func(struct bt_conn *conn,
                                   struct bt_gatt_subscribe_params *params,
                                   const void *data, uint16_t length);
static void    discover_sensor_char(struct bt_conn *conn, int index);
static uint8_t sensor_disc_func(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params);
static uint8_t sensor_cccd_disc_func(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      struct bt_gatt_discover_params *params);

static uint8_t sound_notify_func(struct bt_conn *conn,
                                  struct bt_gatt_subscribe_params *params,
                                  const void *data, uint16_t length);
static void    discover_sound_char(struct bt_conn *conn, int index);
static uint8_t sound_disc_func(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                struct bt_gatt_discover_params *params);
static uint8_t sound_cccd_disc_func(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     struct bt_gatt_discover_params *params);

static void start_scan(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                          struct net_buf_simple *ad);
static void connected(struct bt_conn *conn, uint8_t err);
static void disconnected(struct bt_conn *conn, uint8_t reason);

/* ── Helpers ────────────────────────────────────────────── */

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

/* ════════════════════════════════════════════════════════
 * SENSOR PACKET DECODE  (61 bytes → tracker_payload_t)
 * ════════════════════════════════════════════════════════ */
static void decode_sensor(const uint8_t *d, tracker_payload_t *p)
{
    size_t i = 0;

    p->time      = ((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                   ((uint32_t)d[i+2]<<8)|d[i+3];       i += 4;
    p->time_ms   = ((uint16_t)d[i]<<8)|d[i+1];         i += 2;
    p->uptime_ms = ((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                   ((uint32_t)d[i+2]<<8)|d[i+3];       i += 4;

    p->proto_ver = d[i++];
    p->dev_id    = d[i++];

    p->temp_c_x100      = (int16_t)((d[i]<<8)|d[i+1]);  i += 2;
    p->rh_x100          = (int16_t)((d[i]<<8)|d[i+1]);  i += 2;
    p->press_hPa_x1000  = (int32_t)(((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                                     ((uint32_t)d[i+2]<<8)|d[i+3]); i += 4;

    p->eco2_ppm  = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->tvoc_ppb  = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->aqi       = d[i++];

    for (int k = 0; k < AS7343_NUM_CH; k++) {
        p->as7343[k] = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    }

    p->batt_mV            = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->snd_rms_dbfs_x100  = (int16_t) ((d[i]<<8)|d[i+1]); i += 2;
    p->snd_peak_freq_hz   = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->snd_peak_mag_x10   = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->soil_vwc_x100      = (uint16_t)((d[i]<<8)|d[i+1]);
}

/* ════════════════════════════════════════════════════════
 * MAIN SENSOR CHARACTERISTIC
 * ════════════════════════════════════════════════════════ */

static uint8_t sensor_notify_func(struct bt_conn *conn,
                                   struct bt_gatt_subscribe_params *params,
                                   const void *data, uint16_t length)
{
    if (!data || length != SENSOR_PACKED_LEN) {
        LOG_WRN("[SENSOR] Unexpected length %u (expected %u)", length, SENSOR_PACKED_LEN);
        return BT_GATT_ITER_CONTINUE;
    }

    struct sensor_packet_t pkt;
    memcpy(pkt.data, data, SENSOR_PACKED_LEN);

    if (k_msgq_put(&sensor_msgq, &pkt, K_NO_WAIT) != 0) {
        LOG_WRN("[SENSOR] Queue full — dropping packet");
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t sensor_cccd_disc_func(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (!attr) {
        LOG_ERR("[BASE %d] Sensor CCCD not found", index);
        return BT_GATT_ITER_STOP;
    }

    sensor_ccc_handles[index]       = attr->handle;
    sensor_subs[index].ccc_handle   = sensor_ccc_handles[index];
    sensor_subs[index].value_handle = sensor_char_handles[index];
    sensor_subs[index].notify       = sensor_notify_func;
    sensor_subs[index].value        = BT_GATT_CCC_NOTIFY;

    int err = bt_gatt_subscribe(conn, &sensor_subs[index]);
    if (err) LOG_ERR("[BASE %d] Sensor subscribe failed (%d)", index, err);
    else     LOG_INF("[BASE %d] Subscribed to sensor characteristic", index);

    /* Send first time sync immediately */
    time_sync_writer_send(conn, index, sensor_char_handles[index]);

    /* Chain into sound characteristic discovery */
    discover_sound_char(conn, index);

    return BT_GATT_ITER_STOP;
}

static uint8_t sensor_disc_func(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (!attr) {
        LOG_ERR("[BASE %d] Sensor characteristic not found", index);
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;
    sensor_char_handles[index] = chrc->value_handle;
    LOG_INF("[BASE %d] Sensor char handle: 0x%04x", index, sensor_char_handles[index]);

    sensor_cccd_disc[index].uuid         = &cccd_uuid.uuid;
    sensor_cccd_disc[index].start_handle = sensor_char_handles[index];
    sensor_cccd_disc[index].end_handle   = 0xffff;
    sensor_cccd_disc[index].type         = BT_GATT_DISCOVER_ATTRIBUTE;
    sensor_cccd_disc[index].func         = sensor_cccd_disc_func;

    int err = bt_gatt_discover(conn, &sensor_cccd_disc[index]);
    if (err) LOG_ERR("[BASE %d] Sensor CCCD discover failed (%d)", index, err);

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
    if (err) LOG_ERR("[BASE %d] Sensor discover failed (%d)", index, err);
}

/* ════════════════════════════════════════════════════════
 * SOUND SPECTRUM CHARACTERISTIC
 * ════════════════════════════════════════════════════════ */

static uint8_t sound_notify_func(struct bt_conn *conn,
                                  struct bt_gatt_subscribe_params *params,
                                  const void *data, uint16_t length)
{
    if (!data || length < SOUND_HDR_SIZE) {
        return BT_GATT_ITER_CONTINUE;
    }

    int index = get_conn_index(conn);
    if (index < 0) return BT_GATT_ITER_CONTINUE;

    const uint8_t *buf = (const uint8_t *)data;
    uint8_t  pkt_id = buf[0];
    uint8_t  total  = buf[1];
    uint16_t ts16   = ((uint16_t)buf[2] << 8) | buf[3];

    if (pkt_id >= SOUND_NUM_PKTS || total != SOUND_NUM_PKTS) {
        LOG_WRN("[SOUND %d] Bad header pkt=%u total=%u", index, pkt_id, total);
        return BT_GATT_ITER_CONTINUE;
    }

    struct sound_reassembly *rx = &sound_rx[index];

    if (pkt_id == 0 || ts16 != rx->timestamp16) {
        memset(rx, 0, sizeof(*rx));
        rx->timestamp16 = ts16;
    }

    if (rx->received & (1u << pkt_id)) {
        LOG_WRN("[SOUND %d] Duplicate pkt %u — ignored", index, pkt_id);
        return BT_GATT_ITER_CONTINUE;
    }

    uint32_t bin_start     = pkt_id * SOUND_BINS_PER_PKT;
    const uint8_t *payload = buf + SOUND_HDR_SIZE;
    uint16_t payload_len   = length - SOUND_HDR_SIZE;

    uint16_t bin_bytes = (pkt_id == SOUND_NUM_PKTS - 1)
                         ? payload_len - 2
                         : payload_len;
    uint32_t bin_count = bin_bytes / 2;

    for (uint32_t i = 0; i < bin_count && (bin_start + i) < SOUND_NUM_BINS; i++) {
        rx->bins[bin_start + i] =
            ((uint16_t)payload[i * 2] << 8) | payload[i * 2 + 1];
    }

    if (pkt_id == SOUND_NUM_PKTS - 1 && payload_len >= 2) {
        rx->rms_dbfs_x100 =
            (int16_t)(((uint16_t)payload[payload_len - 2] << 8)
                      | payload[payload_len - 1]);
    }

    rx->received |= (1u << pkt_id);
    LOG_DBG("[SOUND %d] Pkt %u received (mask=0x%02x)", index, pkt_id, rx->received);

    /* All 3 packets received — build sound_payload_t and enqueue */
    if (rx->received == 0x07) {
        struct sound_queue_pkt_t qpkt;
        memset(&qpkt, 0, sizeof(qpkt));

        qpkt.sp.rms_dbfs_x100 = rx->rms_dbfs_x100;
        qpkt.sp.uptime_ms     = 0;   /* filled by process_data_thread */
        qpkt.sp.dev_id        = 0;   /* gateway has no per-conn dev_id */
        memcpy(qpkt.sp.bins, rx->bins, sizeof(rx->bins));

        if (k_msgq_put(&sound_msgq, &qpkt, K_NO_WAIT) != 0) {
            LOG_WRN("[SOUND %d] Queue full — dropping spectrum", index);
        } else {
            LOG_INF("[SOUND %d] Spectrum complete (ts=%u rms=%d)",
                    index, ts16, rx->rms_dbfs_x100);
        }

        rx->received = 0;
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t sound_cccd_disc_func(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (!attr) {
        LOG_ERR("[BASE %d] Sound CCCD not found", index);
        return BT_GATT_ITER_STOP;
    }

    sound_ccc_handles[index]       = attr->handle;
    sound_subs[index].ccc_handle   = sound_ccc_handles[index];
    sound_subs[index].value_handle = sound_char_handles[index];
    sound_subs[index].notify       = sound_notify_func;
    sound_subs[index].value        = BT_GATT_CCC_NOTIFY;

    int err = bt_gatt_subscribe(conn, &sound_subs[index]);
    if (err) LOG_ERR("[BASE %d] Sound subscribe failed (%d)", index, err);
    else     LOG_INF("[BASE %d] Subscribed to sound spectrum characteristic", index);

    return BT_GATT_ITER_STOP;
}

static uint8_t sound_disc_func(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (!attr) {
        LOG_ERR("[BASE %d] Sound characteristic not found", index);
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;
    sound_char_handles[index] = chrc->value_handle;
    LOG_INF("[BASE %d] Sound char handle: 0x%04x", index, sound_char_handles[index]);

    sound_cccd_disc[index].uuid         = &cccd_uuid.uuid;
    sound_cccd_disc[index].start_handle = sound_char_handles[index];
    sound_cccd_disc[index].end_handle   = 0xffff;
    sound_cccd_disc[index].type         = BT_GATT_DISCOVER_ATTRIBUTE;
    sound_cccd_disc[index].func         = sound_cccd_disc_func;

    int err = bt_gatt_discover(conn, &sound_cccd_disc[index]);
    if (err) LOG_ERR("[BASE %d] Sound CCCD discover failed (%d)", index, err);

    return BT_GATT_ITER_STOP;
}

static void discover_sound_char(struct bt_conn *conn, int index)
{
    sound_disc[index].uuid         = &sound_char_uuid.uuid;
    sound_disc[index].func         = sound_disc_func;
    sound_disc[index].start_handle = 0x0001;
    sound_disc[index].end_handle   = 0xffff;
    sound_disc[index].type         = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &sound_disc[index]);
    if (err) LOG_ERR("[BASE %d] Sound discover failed (%d)", index, err);
}

/* ── Scan and connection ────────────────────────────────── */

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
    if (err) {
        LOG_ERR("[BASE] Connect failed (%d)", err);
        start_scan();
    }
}

static struct bt_gatt_exchange_params mtu_params[MAX_CONN];

static void exchange_func(struct bt_conn *conn, uint8_t err,
                           struct bt_gatt_exchange_params *params)
{
    if (err) LOG_ERR("MTU exchange failed (%u)", err);
    else     LOG_INF("MTU negotiated: %d bytes", bt_gatt_get_mtu(conn));
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        LOG_ERR("[BASE] Connect to %s failed (err %u)", addr, err);
        bt_conn_unref(conn);
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
        memset(&sound_rx[index], 0, sizeof(sound_rx[index]));
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected    = connected,
    .disconnected = disconnected,
};

/* ════════════════════════════════════════════════════════
 * process_data_thread
 *
 * Dequeues both sensor and sound packets, encodes JSON, and publishes
 * directly to Azure IoT Hub via azure_mqtt_publish().
 *
 * All working buffers are static — sound JSON is ~4 KB and must not
 * live on the thread stack.
 *
 * Also periodically writes UTC time to all connected sensor nodes.
 * ════════════════════════════════════════════════════════ */

/*
 * Process thread stack:
 *   WROVER (SPIRAM): 6144 — larger because more connections + headroom
 *   POE (no SPIRAM): 4096 — tighter DRAM budget, fewer connections
 *
 * Both are sufficient for sensor JSON (~1.5 KB) and sound JSON (~4 KB)
 * encode because the large output buffers are static, not on-stack.
 */
#if defined(CONFIG_ESP_SPIRAM)
#  define PROCESS_STACK_SIZE  6144
#else
#  define PROCESS_STACK_SIZE  4096
#endif

#define PROCESS_PRIO  BASE_PROCESS_PRIORITY

/*
 * Process thread stack placement:
 *   WROVER: place in SPIRAM to save internal DRAM for BT + MQTT.
 *   POE:    internal DRAM only — no SPIRAM available.
 */
#if defined(CONFIG_ESP_SPIRAM)
Z_KERNEL_STACK_DEFINE_IN(process_stack, PROCESS_STACK_SIZE,
    __attribute__((section(".ext_ram.bss"))));
#else
K_THREAD_STACK_DEFINE(process_stack, PROCESS_STACK_SIZE);
#endif

static struct k_thread process_tid;

static uint8_t sound_pkt_count = 0;  /* throttle: publish 1 in 10 sound frames */

/*
 * Large working buffers — static so they don't consume thread stack.
 *
 * WROVER: place in SPIRAM (.ext_ram.bss) to relieve the 96 KB internal
 *         DRAM shared with BT + MQTT stacks.
 * POE:    stay in normal BSS — no SPIRAM available, but fewer connections
 *         (MAX_CONN 2) and smaller stack sizes keep total DRAM in budget.
 */
#if defined(CONFIG_ESP_SPIRAM)
static tracker_payload_t      s_sensor_pkt                               __attribute__((section(".ext_ram.bss")));
static struct json_full_packet s_jp                                      __attribute__((section(".ext_ram.bss")));
static char                    s_sensor_json[JSON_BUFFER_SIZE]           __attribute__((section(".ext_ram.bss")));
static sound_payload_t         s_sound_pkt                               __attribute__((section(".ext_ram.bss")));
static char                    s_sound_json[SOUND_JSON_BUFFER_SIZE]      __attribute__((section(".ext_ram.bss")));
#else
static tracker_payload_t       s_sensor_pkt;
static struct json_full_packet s_jp;
static char                    s_sensor_json[JSON_BUFFER_SIZE];
static sound_payload_t         s_sound_pkt;
static char                    s_sound_json[SOUND_JSON_BUFFER_SIZE];
#endif

void process_data_thread(void)
{
    /* Zero all buffers before first use */
    memset(&s_sensor_pkt, 0, sizeof(s_sensor_pkt));
    memset(&s_jp,         0, sizeof(s_jp));
    memset(s_sensor_json, 0, sizeof(s_sensor_json));
    memset(&s_sound_pkt,  0, sizeof(s_sound_pkt));
    memset(s_sound_json,  0, sizeof(s_sound_json));

    uint32_t last_timesync_ms = 0;

    while (1) {
        /* ── Periodic time sync to all nodes ──────────────── */
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

        /* ── Sensor queue → JSON → Azure ──────────────────── */
        struct sensor_packet_t spkt;
        if (k_msgq_get(&sensor_msgq, &spkt, K_MSEC(5)) == 0) {
#if DEBUG_PRINT_RAW
            printk("[SENSOR RAW] %u bytes:", SENSOR_PACKED_LEN);
            for (int i = 0; i < SENSOR_PACKED_LEN; i++) printk(" %02x", spkt.data[i]);
            printk("\n");
#else
            memset(&s_sensor_pkt, 0, sizeof(s_sensor_pkt));
            memset(&s_jp,         0, sizeof(s_jp));

            decode_sensor(spkt.data, &s_sensor_pkt);
            fill_json_packet(&s_sensor_pkt, &s_jp);

            int ret = snprintk(s_sensor_json, sizeof(s_sensor_json),
                JSON_FORMAT,
                s_jp.header.messageId,
                s_jp.header.gatewayId,
                s_jp.header.schemaVersion,
                s_jp.header.messageType,
                s_jp.payload.deviceId,
                s_jp.payload.timestamp,
                s_jp.payload.uptime_ms,
                s_jp.payload.proto_ver,
                s_jp.payload.environment.temperature_c,
                s_jp.payload.environment.humidity_percent,
                s_jp.payload.environment.pressure_hpa,
                s_jp.payload.environment.eco2_ppm,
                s_jp.payload.environment.tvoc_ppb,
                s_jp.payload.environment.aqi,
                s_jp.payload.environment.batt_mV,
                s_jp.payload.spectrum.AS7343_405nm,
                s_jp.payload.spectrum.AS7343_425nm,
                s_jp.payload.spectrum.AS7343_450nm,
                s_jp.payload.spectrum.AS7343_475nm,
                s_jp.payload.spectrum.AS7343_515nm,
                s_jp.payload.spectrum.AS7343_550nm,
                s_jp.payload.spectrum.AS7343_555nm,
                s_jp.payload.spectrum.AS7343_600nm,
                s_jp.payload.spectrum.AS7343_640nm,
                s_jp.payload.spectrum.AS7343_690nm,
                s_jp.payload.spectrum.AS7343_745nm,
                s_jp.payload.spectrum.AS7343_855nm,
                s_jp.payload.spectrum.AS7343_VISIBLE,
                s_jp.payload.sound.rms_dbfs,
                s_jp.payload.sound.peak_freq_hz,
                s_jp.payload.sound.peak_mag,
                s_jp.payload.soil_vwc
            );

            if (ret > 0 && ret < (int)sizeof(s_sensor_json)) {
                int pub = azure_mqtt_publish(s_sensor_json);
                if (pub < 0) {
                    LOG_WRN("Sensor publish failed (%d)", pub);
                } else {
                    LOG_INF("Sensor published dev-%u (%d bytes)",
                            s_sensor_pkt.dev_id, ret);
                }
            } else {
                LOG_ERR("Sensor JSON encode failed (ret=%d)", ret);
            }
#endif
        }

        /* ── Sound queue → JSON → Azure (1 in 10) ─────────── */
        struct sound_queue_pkt_t sqpkt;
        if (k_msgq_get(&sound_msgq, &sqpkt, K_MSEC(5)) == 0) {
#if DEBUG_PRINT_RAW
            printk("[SOUND RAW] rms=%d bins[0]=%u bins[1]=%u ...\n",
                   sqpkt.sp.rms_dbfs_x100,
                   sqpkt.sp.bins[0], sqpkt.sp.bins[1]);
#else
            if (++sound_pkt_count >= 10) {
                sound_pkt_count = 0;

                memcpy(&s_sound_pkt, &sqpkt.sp, sizeof(s_sound_pkt));
                /* Stamp current UTC as uptime approximation */
                s_sound_pkt.uptime_ms = k_uptime_get_32();

                int ret = encode_sound_json(&s_sound_pkt,
                                            s_sound_json, sizeof(s_sound_json));
                if (ret > 0) {
                    int pub = azure_mqtt_publish(s_sound_json);
                    if (pub < 0) LOG_WRN("Sound publish failed (%d)", pub);
                    else         LOG_INF("Sound published (%d bytes JSON)", ret);
                } else {
                    LOG_ERR("Sound JSON encode failed (%d)", ret);
                }
            } else {
                LOG_DBG("Sound skipped (%u/10)", sound_pkt_count);
            }
#endif
        }
    }
}

/* ── base_thread ────────────────────────────────────────── */
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