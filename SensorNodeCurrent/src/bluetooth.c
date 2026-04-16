#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/timeutil.h>

#include <string.h>
#include "bluetooth.h"
#include "sensor.h"

#include "time_sync.h"

#ifndef CONFIG_BT_DEVICE_NAME
#define CONFIG_BT_DEVICE_NAME "SensorNode"
#endif

#define PACKED_DATA_LEN 61   /* +6 bytes: snd_rms(2) + snd_peak_freq(2) + snd_peak_mag(2) */


LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

/* GLOBAL VARIABLES*/
uint8_t packed_data[PACKED_DATA_LEN];
static struct bt_conn *current_conn;
static bool notify_enabled = false;  // NEW
uint8_t ble_tick = 0;

// Custom 128-bit service UUID for advertising (little endian)
static const uint8_t custom_service_uuid[16] = {
    0x12, 0x34, 0x56, 0x78,
    0x12, 0x34, 0x56, 0x78,
    0x12, 0x34, 0x56, 0x78,
    0x9a, 0xbc, 0xde, 0xf0,
};

// Advertising data: Flags for general discovery and custome 128-bit service UUID to display (for base to see then establish gatt connection)
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_UUID128_ALL, custom_service_uuid, sizeof(custom_service_uuid)),
};

// Scan response data: Complete device name from configuration (only using for testing on nrf app)
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, strlen(CONFIG_BT_DEVICE_NAME)),
};

static ssize_t pack_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset);
static ssize_t write_handler(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags);
static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void connected(struct bt_conn *conn, uint8_t err);
static void disconnected(struct bt_conn *conn, uint8_t reason);
extern int init_bluetooth(void);
extern int start_advertising(void);
extern int stop_advertising(void);
extern int stop_advertising_and_disconnect();
void tracker_thread(void);

struct bt_le_adv_param adv_params = {
    .id = BT_ID_DEFAULT,
    .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
    .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
    .options = BT_LE_ADV_OPT_CONN, // Connectable mode (this is depreciatetd though only thing i found to work)
};

//Bluetooth connection callbacks structure:
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

static K_SEM_DEFINE(adv_restart_sem, 0, 1);

/*
GATT (Generic Attribute Profile) is a protocol used in Bluetooth Low Energy (BLE) 
defining how data is structured, discovered, and exchanged between devices. 
Data is organised into services and characteristics.

- Services are collections of data and associated behaviors (like a sensor service).
- Characteristics are individual data points or features within a service (like a temperature value).

Each attribute (service, characteristic, descriptor, etc.) in GATT has a unique handle,
A 16-bit identifier used by the BLE stack to read, write, or discover that attribute. 
Handles allow clients to specifically refer and operate on specific data points.
*/
BT_GATT_SERVICE_DEFINE(custom_svc, //custom service
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128( //primary service with 128-bit custom UUID
        0x12, 0x34, 0x56, 0x78,
        0x12, 0x34,
        0x56, 0x78,
        0x12, 0x34,
        0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128( //characteristic defined within the service
        0x99, 0x88, 0x77, 0x66,
        0x55, 0x44,
        0x33, 0x22,
        0x11, 0x00,
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY, // read, write and notifying characteristic permissions
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, // read and write attribute permissions
        pack_data, //read callback
        write_handler, //write callback
        NULL),
    // Client Characteristic Configuration Descriptor (CCCD)
    // Allows the client to enable or disable notifications
    BT_GATT_CCC(ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/**
 * @brief Read callback for the custom characteristic to return packed sensor data.
 *
 * This function called when a connected BLE client reads the value of the
 * custom characteristic. It returns the current contents of the `packed_data` buffer,
 * which containing the tracker sensor data
 *
 * @param conn   Pointer to the connection object.
 * @param attr   Pointer to the GATT attribute being read.
 * @param buf    Buffer to store the read data.
 * @param len    Maximum number of bytes the buffer can hold.
 * @param offset Offset from the beginning of the attribute value.
 *
 * @return Number of bytes read and copied to the buffer, or a negative error code.
 */
static ssize_t pack_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset) {
    printk("[TRACKER] packing data\n");
    return bt_gatt_attr_read(conn, attr, buf, len, offset, packed_data, PACKED_DATA_LEN);
}

/**
 * @brief Write callback for the custom characteristic to handle time synchronisation.
 *
 * This function is called when a connected BLE client (gateway) writes to the
 * custom characteristic. It passes the incoming buffer to time_sync_handle_write()
 * which validates the magic byte (0xFE) and, if matched, updates the node's
 * drift-corrected UTC clock model. Any write not matching the time sync packet
 * format is logged and ignored.
 *
 * @param conn   Pointer to the connection object.
 * @param attr   Pointer to the GATT attribute being written.
 * @param buf    Pointer to the data buffer provided by the client.
 * @param len    Length of the data being written (expected TIMESYNC_PACKET_LEN).
 * @param offset Offset into the attribute value.
 * @param flags  Flags indicating write behavior.
 *
 * @return Number of bytes written (always len).
 */
static ssize_t write_handler(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags) {
    if (time_sync_handle_write(buf, len)) {
        return len;
    }

    LOG_WRN("write_handler: unexpected write (len=%u) ignored", len);
    return len;
}


/**
 * @brief Callback triggered when the CCC (Client Characteristic Configuration) is changed.
 *
 * This function is invoked when a BLE client writes to the CCC descriptor of a
 * characteristic to enable or disable notifications. It updates the global
 * notify_enabled flag to indicate.
 *
 * @param attr  Pointer to the CCC GATT attribute.
 * @param value New CCC value set by the client (BT_GATT_CCC_NOTIFY to enable notifications).
 */
static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {

    if (value == BT_GATT_CCC_NOTIFY) {
        notify_enabled = true;
        printk("Notifications enabled\n");
    } else {
        printk("Notifications disabled\n");
        notify_enabled = false;
    }
}

/**
 * @brief Bluetooth connection callback when a connection is successfully established.
 *
 * It checks for connection errors, stores a reference to the current connection,
 * prints a status message, and sets the BLE tick flag indicating a BT connection.
 *
 * @param conn Pointer to the new connection object.
 * @param err  Error code (0 if successful, non-zero on failure).
 */
static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("[TRACKER] Failed to connect (err %u)\n", err);
        return;
    }

    current_conn = bt_conn_ref(conn);
    printk("[TRACKER] Connected\n");
    ble_tick = 1;
}

/*
 * On disconnect, unreference the connection and give semaphore to restart advertising so
 * the gateway can reconnect
 */
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("[TRACKER] Disconnected (reason 0x%02x)\n", reason);

    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    notify_enabled = false;
    ble_tick = 0;

    k_sem_give(&adv_restart_sem);
}

static void start_advertising_with_retry(bool after_disconnect) {
    if (after_disconnect) {
        k_sleep(K_MSEC(500));
    }

    while (1) {
        int err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad),
                                   sd, ARRAY_SIZE(sd));
        if (err == 0) {
            printk("[SENSOR] Advertising started\n");
            return;
        }
        printk("[SENSOR] Advertising failed (%d) — retrying in 1s\n", err);
        k_sleep(K_SECONDS(1));
    }
}

/**
 * @brief Initialize Bluetooth.
 *
 * Enables Bluetooth and registers connection callbacks.
 *
 * @return 0 on success, or a negative error code on failure.
 */
extern int init_bluetooth(void) {
    int err;
    printk("[SENSOR] Initializing Bluetooth...\n");
    err = bt_enable(NULL);
    if (err) {
        printk("[SENSOR] Bluetooth init failed (err %d)\n", err);
        return err;
    }
    bt_conn_cb_register(&conn_callbacks);

    return 0;
}

/**
 * @brief Start Bluetooth advertising.
 *
 * Begins advertising with predefined parameters and advertising data.
 * Waits briefly after starting to allow advertising packets to propagate.
 *
 * @return 0 on success, or a negative error code on failure.
 */
extern int start_advertising(void) {
    int err;
    printk("[SENSOR] Starting advertising...\n");
    err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        printk("[SENSOR] Advertising failed to start (err %d)\n", err);
        return err;
    }

    printk("[SENSOR] Advertising started\n");
    k_msleep(3000);  // Wait before restarting next advertisement cycle ?????
    return 0;
}

/**
 * @brief Stop Bluetooth advertising.
 *
 * Waits briefly to ensure packets clear, then stops advertising.
 *
 * @return 0 on success, or a negative error code on failure.
 */
extern int stop_advertising(void) {
    k_msleep(2000); // wait for packets to clear before stopping
    int err;
    printk("[SENSOR] Stopping advertising...\n");

    err = bt_le_adv_stop();
    if (err) {
        printk("[SENSOR] Failed to stop advertising (err %d)\n", err);
        return err;
    }

    printk("[SENSOR] Advertising stopped\n");
    return 0;
}

/**
 * @brief Stop advertising and disconnect any active connection.
 *
 * Stops advertising, then if a connection exists, disconnects it with
 * a remote user termination reason.
 *
 * @return 0 on success, or a negative error code on failure.
 */
extern int stop_advertising_and_disconnect() {
    int err;

    err = stop_advertising();
    if (err) {
        printk("[SENSOR] Failed to stop advertising\n");
        return err;
    }

    if (current_conn) {
        err = bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (err) {
            printk("[SENSOR] Disconnection failed (err %d)\n", err);
            return err;
        } else {
            printk("[SENSOR] Disconnection initiated\n");
        }
    }

    return 0;
}

/*
 *
 * Initialise BT, start advertising once, then block forever.
 * All reconnection logic lives in disconnected() above
 * needed here.
 *
 * Modality BLE threads handle all data flow
 * independently
 */
void bluetooth_init_thread(void) {
    int err = init_bluetooth();
    if (err) {
        printk("[TRACKER] Bluetooth init failed (%d)\n", err);
        return;
    }

    start_advertising_with_retry(false);

    while (1) {
        k_sem_take(&adv_restart_sem, K_FOREVER);
        start_advertising_with_retry(true);
    }
}