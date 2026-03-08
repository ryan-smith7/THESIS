/*
 * bluetooth.c — BLE GATT Central for combined BLE+WiFi+Azure gateway
 *
 * Change from standalone version:
 *   process_data_thread now calls encode_and_publish_json() instead of
 *   encode_and_print_json() — packets go to Azure IoT Hub via MQTT.
 *
 * Queue depth reduced from 100 to 8 to save ~4KB DRAM.
 */

// Zephyr includes
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

// App-specific includes
#include "bluetooth.h"
#include "my_json.h"
#include <ctype.h>
#include <stdlib.h>

#define MAX_CONN      4
#define UNSET         0
#define SET           1
#define INVALID      -1
#define MSGQ_MAX_MSGS 8    /* reduced from 100 — saves ~4KB DRAM */

#define PACKED_DATA_LEN 51

LOG_MODULE_REGISTER(bluetooth_module);

/* UUIDs */
static struct bt_uuid_128 tracker_service_uuid = BT_UUID_INIT_128(
    0x12, 0x34, 0x56, 0x78,
    0x12, 0x34,
    0x56, 0x78,
    0x12, 0x34,
    0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);
static struct bt_uuid_128 tracker_char_uuid = BT_UUID_INIT_128(
    0x99, 0x88, 0x77, 0x66,
    0x55, 0x44,
    0x33, 0x22,
    0x11, 0x00,
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa);
static struct bt_uuid_16 conn_characteristic_uuid = BT_UUID_INIT_16(0x2902);

/* Bluetooth state */
static struct bt_conn *conns[MAX_CONN];
static struct bt_gatt_subscribe_params subs[MAX_CONN];
static uint16_t char_handles[MAX_CONN];
static uint16_t ccc_handles[MAX_CONN];
struct bt_gatt_discover_params discover_params[MAX_CONN];
struct bt_gatt_discover_params cccd_discover_params[MAX_CONN];
static struct bt_gatt_write_params write_params_array[MAX_CONN];

struct sensor_packet_t {
    uint8_t data[PACKED_DATA_LEN];
};

K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_packet_t), MSGQ_MAX_MSGS, 4);

uint8_t update_dev_id = UNSET;
static uint8_t data_to_write[1];

/* Forward declarations */
static int get_conn_index(struct bt_conn *conn);
static uint16_t check_integer(const char *string);
static bool is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid);
static int cmd_set_device(const struct shell *shell, size_t argc, char **argv);
static void write_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params);
static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length);
static void discover_tracker_characteristic(struct bt_conn *conn, int index);
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params);
static uint8_t cccd_discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params);
static void start_scan(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad);
static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params);
static void connected(struct bt_conn *conn, uint8_t err);
static void disconnected(struct bt_conn *conn, uint8_t reason);

static struct bt_conn_cb conn_callbacks = {
    .connected    = connected,
    .disconnected = disconnected,
};

struct bt_gatt_exchange_params mtu_params = {
    .func = exchange_func,
};

static int get_conn_index(struct bt_conn *conn) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (conns[i] == conn) return i;
    }
    return INVALID;
}

uint16_t check_integer(const char *string) {
    for (uint8_t i = 0; string[i] != '\0'; i++) {
        if (!isdigit(string[i])) return INVALID;
    }
    return atoi(string);
}

static bool is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid) {
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

static int cmd_set_device(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 2) {
        shell_print(shell, "Usage: set_device <value (0-255)>");
        return -EINVAL;
    }
    uint16_t value = check_integer(argv[1]);
    if (value == (uint16_t)INVALID) {
        shell_print(shell, "Value needs to be numerical");
        return -EINVAL;
    }
    if (value > 255) {
        shell_print(shell, "Value out of range (0-255)");
        return -EINVAL;
    }
    data_to_write[0] = (uint8_t)value;
    shell_print(shell, "Next connection Dev ID will change to: %d", data_to_write[0]);
    update_dev_id = SET;
    return 0;
}

static void write_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params) {
    if (err) {
        LOG_ERR("Write failed (err %u)", err);
    } else {
        LOG_INF("Write successful");
    }
}

static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length)
{
    if (!data) {
        LOG_INF("[NOTIFY] Unsubscribed");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    if (length != PACKED_DATA_LEN) {
        LOG_WRN("[NOTIFY] Unexpected length %u (expected %u)", length, PACKED_DATA_LEN);
        return BT_GATT_ITER_CONTINUE;
    }

    struct sensor_packet_t pkt;
    memcpy(pkt.data, data, PACKED_DATA_LEN);

    if (k_msgq_put(&sensor_msgq, &pkt, K_NO_WAIT) != 0) {
        LOG_WRN("[NOTIFY] Message queue full — dropping packet");
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t cccd_discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (index < 0) return BT_GATT_ITER_STOP;

    if (!attr) {
        LOG_INF("[CCCD] Discovery complete");
        return BT_GATT_ITER_STOP;
    }

    if (!bt_uuid_cmp(params->uuid, &conn_characteristic_uuid.uuid)) {
        ccc_handles[index] = attr->handle;
        LOG_INF("[CCCD] Found CCC handle: %u", ccc_handles[index]);

        subs[index].notify       = notify_func;
        subs[index].value        = BT_GATT_CCC_NOTIFY;
        subs[index].value_handle = char_handles[index];
        subs[index].ccc_handle   = ccc_handles[index];

        int err = bt_gatt_subscribe(conn, &subs[index]);
        if (err && err != -EALREADY) {
            LOG_ERR("[CCCD] Subscribe failed (err %d)", err);
        } else {
            LOG_INF("[CCCD] Subscribed to notifications");

            if (update_dev_id == SET) {
                write_params_array[index].func   = write_cb;
                write_params_array[index].handle = char_handles[index];
                write_params_array[index].offset = 0;
                write_params_array[index].data   = data_to_write;
                write_params_array[index].length = sizeof(data_to_write);
                bt_gatt_write(conn, &write_params_array[index]);
                update_dev_id = UNSET;
            }
        }
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              struct bt_gatt_discover_params *params)
{
    int index = get_conn_index(conn);
    if (index < 0) return BT_GATT_ITER_STOP;

    if (!attr) {
        LOG_INF("[DISC] Characteristic discovery complete");
        return BT_GATT_ITER_STOP;
    }

    if (!bt_uuid_cmp(params->uuid, &tracker_char_uuid.uuid)) {
        char_handles[index] = bt_gatt_attr_value_handle(attr);
        LOG_INF("[DISC] Found characteristic handle: %u", char_handles[index]);

        cccd_discover_params[index].uuid     = &conn_characteristic_uuid.uuid;
        cccd_discover_params[index].start_handle = attr->handle + 1;
        cccd_discover_params[index].end_handle   = 0xFFFF;
        cccd_discover_params[index].type         = BT_GATT_DISCOVER_DESCRIPTOR;
        cccd_discover_params[index].func         = cccd_discover_func;

        bt_gatt_discover(conn, &cccd_discover_params[index]);
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}

static void discover_tracker_characteristic(struct bt_conn *conn, int index) {
    discover_params[index].uuid        = &tracker_char_uuid.uuid;
    discover_params[index].start_handle = 0x0001;
    discover_params[index].end_handle   = 0xFFFF;
    discover_params[index].type         = BT_GATT_DISCOVER_CHARACTERISTIC;
    discover_params[index].func         = discover_func;
    bt_gatt_discover(conn, &discover_params[index]);
}

static struct bt_le_scan_param scan_param = {
    .type     = BT_LE_SCAN_TYPE_ACTIVE,
    .options  = BT_LE_SCAN_OPT_NONE,
    .interval = BT_GAP_SCAN_FAST_INTERVAL,
    .window   = BT_GAP_SCAN_FAST_WINDOW,
};

static void start_scan(void) {
    int err = bt_le_scan_start(&scan_param, device_found);
    if (err) {
        LOG_ERR("[SCAN] Failed to start scanning (err %d)", err);
    } else {
        LOG_INF("[SCAN] Scanning started");
    }
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad)
{
    if (type != BT_GAP_ADV_TYPE_ADV_IND &&
        type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        return;
    }
    if (!is_uuid_in_ad(ad, &tracker_service_uuid.uuid)) return;

    struct bt_conn *existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
    if (existing) {
        bt_conn_unref(existing);
        return;
    }

    bool slot_available = false;
    int  index_available = 0;
    for (int i = 0; i < MAX_CONN; i++) {
        if (!conns[i]) {
            slot_available  = true;
            index_available = i;
            break;
        }
    }
    if (!slot_available) return;

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INF("[BASE] Found tracker: %s (RSSI %d)", addr_str, rssi);

    bt_le_scan_stop();
    conns[index_available] = NULL;
    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                                BT_LE_CONN_PARAM_DEFAULT,
                                &conns[index_available]);
    if (err) {
        LOG_ERR("[BASE] Failed to connect (%d)", err);
        start_scan();
    }
}

void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params) {
    if (err) {
        LOG_ERR("MTU exchange failed (err %u)", err);
    } else {
        LOG_INF("MTU exchanged: %d", bt_gatt_get_mtu(conn));
    }
}

static void connected(struct bt_conn *conn, uint8_t err) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        LOG_ERR("[BASE] Failed to connect to %s (err %u)", addr, err);
        bt_conn_unref(conn);
        start_scan();
        return;
    }

    int index = get_conn_index(conn);
    if (index < 0) {
        LOG_INF("[BASE] No free conn slots");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        start_scan();
        return;
    }

    LOG_INF("[BASE] Connected [%d]: %s", index, addr);
    bt_gatt_exchange_mtu(conn, &mtu_params);
    discover_tracker_characteristic(conn, index);
    start_scan();
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    int index = get_conn_index(conn);
    LOG_INF("[BASE] Disconnected [%d] (reason 0x%02x)", index, reason);
    if (index >= 0) {
        bt_conn_unref(conns[index]);
        conns[index]      = NULL;
        char_handles[index] = 0;
    }
}

/* -----------------------------------------------------------------------
 * process_data_thread
 * Dequeues BLE packets, decodes binary payload, builds JSON,
 * and publishes to Azure IoT Hub via MQTT.
 * ----------------------------------------------------------------------- */
void process_data_thread(void)
{
    struct sensor_packet_t pkt;

    while (1) {
        k_msgq_get(&sensor_msgq, &pkt, K_FOREVER);

        tracker_payload_t p = {0};
        size_t i = 0;

        /* time (0..3) */
        p.time = ((uint32_t)pkt.data[i]   << 24) |
                 ((uint32_t)pkt.data[i+1] << 16) |
                 ((uint32_t)pkt.data[i+2] <<  8) |
                  (uint32_t)pkt.data[i+3];
        i += 4;

        /* uptime_ms (4..7) */
        p.uptime_ms = ((uint32_t)pkt.data[i]   << 24) |
                      ((uint32_t)pkt.data[i+1] << 16) |
                      ((uint32_t)pkt.data[i+2] <<  8) |
                       (uint32_t)pkt.data[i+3];
        i += 4;

        /* proto, dev (8,9) */
        p.proto_ver = pkt.data[i++];
        p.dev_id    = pkt.data[i++];

        /* BME280 (10..15) */
        p.temp_c_x100       = (int16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.rh_x100           = (int16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.press_hPa_x1000   = ((uint32_t)pkt.data[i]   << 24) |
                               ((uint32_t)pkt.data[i+1] << 16) |
                               ((uint32_t)pkt.data[i+2] <<  8) |
                                (uint32_t)pkt.data[i+3];
        i += 4;

        /* ENS160 (16..20) */
        p.eco2_ppm = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.tvoc_ppb = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.aqi      = pkt.data[i++];

        /* AS7343 (21..46) */
        for (int k = 0; k < 13; k++) {
            p.as7343[k] = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]);
            i += 2;
        }

        /* Battery (47..48) */
        p.batt_mV = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]);

        /* Build JSON and publish to Azure — replaces encode_and_print_json */
        struct json_full_packet jp = {0};
        fill_json_packet(&p, &jp);
        encode_and_publish_json(&jp);
    }
}

/* -----------------------------------------------------------------------
 * base_thread — initialises BLE and starts scanning
 * ----------------------------------------------------------------------- */
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