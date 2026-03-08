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

#include "i2c_gate.h"

#ifndef CONFIG_BT_DEVICE_NAME
#define CONFIG_BT_DEVICE_NAME "SensorNode"
#endif

#define DEV_ID 2
#define PACKED_DATA_LEN 51

#define PROTO_VER 1

/* GLOBAL VARIABLES*/
uint8_t packed_data[PACKED_DATA_LEN];
static struct bt_conn *current_conn;
static bool notify_enabled = false;  // NEW
uint8_t ble_tick = 0;
uint8_t dev_id = DEV_ID;

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

/* FUNCTION PROTOTYPES*/
extern uint8_t get_ble_tick(void);
extern void set_ble_tick(uint8_t value);

static ssize_t pack_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset);
static ssize_t write_handler(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags);
static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void connected(struct bt_conn *conn, uint8_t err);
static void disconnected(struct bt_conn *conn, uint8_t reason);
static void print_packed_data();
extern void pack_sensor_data(const struct sensor_blk *sensor);
extern int init_bluetooth(void);
extern int start_advertising(void);
extern int stop_advertising(void);
extern int stop_advertising_and_disconnect();
void tracker_thread(void);

// Advertising parameters
// struct bt_le_adv_param adv_params = {
//     .id = BT_ID_DEFAULT,
//     .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
//     .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
//     .options = BT_LE_ADV_OPT_CONNECTABLE, // Connectable mode (this is depreciatetd though only thing i found to work)
// };

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

//Helper functions

/**
 * @brief Get the current value of the BLE tick counter.
 *
 * @return The current value of the ble_tick variable.
 */
extern uint8_t get_ble_tick(void) {
    return ble_tick;
}

/**
 * @brief Set the BLE tick counter to a specified value.
 *
 * @param value The new value to assign to the ble_tick variable.
 */
extern void set_ble_tick(uint8_t value) {
    ble_tick = value;
}

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
 * @brief Write callback for the custom characteristic to update the device ID.
 *
 * This function is called when a connected BLE client writes to the custom
 * characteristic. It extracts the first byte from the incoming buffer and
 * stores it in the global dev_id variable, representing the device ID.
 *
 * @param conn   Pointer to the connection object.
 * @param attr   Pointer to the GATT attribute being written.
 * @param buf    Pointer to the data buffer provided by the client.
 * @param len    Length of the data being written.
 * @param offset Offset into the attribute value 
 * @param flags  Flags indicating write behavior 
 */
static ssize_t write_handler(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags) {
    //update device ID
    dev_id = ((uint8_t *)buf)[0];
    // printk("dev_id set to: %d\n", dev_id);

    // Return number of bytes written
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

/**
 * @brief Bluetooth disconnection callback triggered when the device disconnects.
 *
 * This function is called when the BLE connection is disconnected, prints the
 * disconnection reason, unreferencing, clearing the current connection, and resetting
 * the BLE tick flag.
 *
 * @param conn   Pointer to the disconnected connection object.
 * @param reason Reason code for the disconnection.
 */
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("[TRACKER] Disconnected (reason 0x%02x)\n", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    ble_tick = 0;
}

/**
 * @brief Unpack and print the contents of the compact sensor packet (no hash).
 * Layout (bytes, big-endian where multi-byte):
 *  0..3:   time (uint32_t, epoch s)
 *  4..7:   uptime_ms (uint32_t)
 *  8:      proto_ver (uint8_t)
 *  9:      dev_id    (uint8_t)
 * 10..11:  temp_c_x100 (int16_t)
 * 12..13:  rh_x100     (int16_t)
 * 14..15:  press_hPa_x10 (int16_t)
 * 16..17:  eco2_ppm (uint16_t)
 * 18..19:  tvoc_ppb (uint16_t)
 * 20:      aqi (uint8_t)
 * 21..46:  AS7343[13] (uint16_t each) order: 405,425,450,475,515,550,555,600,640,690,745,855, VISIBLE
 * 47..48:  batt_mV (uint16_t)
 */
static void print_packed_data(void)
{
    size_t i = 0;

    // Timestamps / meta
    uint32_t timestamp =
        ((uint32_t)packed_data[i] << 24) |
        ((uint32_t)packed_data[i+1] << 16) |
        ((uint32_t)packed_data[i+2] << 8) |
         (uint32_t)packed_data[i+3];
    i += 4;

    uint32_t uptime_ms =
        ((uint32_t)packed_data[i] << 24) |
        ((uint32_t)packed_data[i+1] << 16) |
        ((uint32_t)packed_data[i+2] << 8) |
         (uint32_t)packed_data[i+3];
    i += 4;

    uint8_t proto_ver = packed_data[i++];
    uint8_t dev_id    = packed_data[i++];

    // BME280
    int16_t temp_x100 = (int16_t)((packed_data[i] << 8) | packed_data[i+1]); i += 2;
    int16_t rh_x100   = (int16_t)((packed_data[i] << 8) | packed_data[i+1]); i += 2;
    int32_t p_hPa_x1000 = (int32_t)((packed_data[i] << 24) | (packed_data[i] << 16) | (packed_data[i] << 8) | packed_data[i+1]); i += 4;

    // ENS160
    uint16_t eco2_ppm = (uint16_t)((packed_data[i] << 8) | packed_data[i+1]); i += 2;
    uint16_t tvoc_ppb = (uint16_t)((packed_data[i] << 8) | packed_data[i+1]); i += 2;
    uint8_t aqi       = packed_data[i++];
    

    // AS7343 channels
    static const uint16_t wl[13] = {
        405,425,450,475,515,550,555,600,640,690,745,855,999  // 999 = VISIBLE
    };
    uint16_t ch[13];
    for (int k = 0; k < 13; k++) {
        ch[k] = (uint16_t)((packed_data[i] << 8) | packed_data[i+1]);
        i += 2;
    }

    // Battery
    uint16_t batt_mV = (uint16_t)((packed_data[i] << 8) | packed_data[i+1]);
    // i += 2; // not used

    // ---- Print nicely ----
    printk("[TRACKER] Parsed Data:\n");
    printk("  Time: %u  Uptime: %u ms  Ver: %u  DevID: %u\n",
           timestamp, uptime_ms, proto_ver, dev_id);

    printk("  BME280: T=%.2f °C  RH=%.2f %%  P=%.1f hPa\n",
           temp_x100 / 100.0f, rh_x100 / 100.0f, p_hPa_x1000 / 1000.0f);

    printk("  ENS160: eCO2=%u ppm  TVOC=%u ppb  AQI=%u\n",
           eco2_ppm, tvoc_ppb, aqi);

    for (int k = 0; k < 13; k++) {
        if (wl[k] == 999) {
            printk("  AS7343_VISIBLE: %u\n", ch[k]);
        } else {
            printk("  AS7343_%unm: %u\n", wl[k], ch[k]);
        }
    }

    printk("  Batt: %u mV\n", batt_mV);
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
    printk("[TRACKER] Initializing Bluetooth...\n");
    err = bt_enable(NULL);
    if (err) {
        printk("[TRACKER] Bluetooth init failed (err %d)\n", err);
        return err;
    }
    printk("here1");
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
    printk("[TRACKER] Starting advertising...\n");
    err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        printk("[TRACKER] Advertising failed to start (err %d)\n", err);
        return err;
    }

    printk("[TRACKER] Advertising started\n");
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
    printk("[TRACKER] Stopping advertising...\n");

    err = bt_le_adv_stop();
    if (err) {
        printk("[TRACKER] Failed to stop advertising (err %d)\n", err);
        return err;
    }

    printk("[TRACKER] Advertising stopped\n");
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
        printk("[TRACKER] Failed to stop advertising\n");
        return err;
    }

    if (current_conn) {
        err = bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (err) {
            printk("[TRACKER] Disconnection failed (err %d)\n", err);
            return err;
        } else {
            printk("[TRACKER] Disconnection initiated\n");
        }
    }

    return 0;
}

extern void pack_sensor_data(const struct sensor_blk *s)
{
    size_t o = 0;

    // time
    packed_data[o++] = (s->time >> 24) & 0xFF;
    packed_data[o++] = (s->time >> 16) & 0xFF;
    packed_data[o++] = (s->time >>  8) & 0xFF;
    packed_data[o++] =  s->time        & 0xFF;

    // uptime_ms
    packed_data[o++] = (s->uptime_ms >> 24) & 0xFF;
    packed_data[o++] = (s->uptime_ms >> 16) & 0xFF;
    packed_data[o++] = (s->uptime_ms >>  8) & 0xFF;
    packed_data[o++] =  s->uptime_ms        & 0xFF;

    // meta
    packed_data[o++] = s->proto_ver;
    packed_data[o++] = s->dev_id;

    // BME280
    packed_data[o++] = (s->temp_c_x100 >> 8) & 0xFF;
    packed_data[o++] =  s->temp_c_x100       & 0xFF;
    packed_data[o++] = (s->rh_x100 >> 8) & 0xFF;
    packed_data[o++] =  s->rh_x100       & 0xFF;
    
    packed_data[o++] = (s->press_hPa_x1000 >> 24) & 0xFF;
    packed_data[o++] = (s->press_hPa_x1000 >> 16) & 0xFF;
    packed_data[o++] = (s->press_hPa_x1000 >>  8) & 0xFF;
    packed_data[o++] =  s->press_hPa_x1000        & 0xFF;

    // ENS160
    packed_data[o++] = (s->eco2_ppm >> 8) & 0xFF;
    packed_data[o++] =  s->eco2_ppm       & 0xFF;
    packed_data[o++] = (s->tvoc_ppb >> 8) & 0xFF;
    packed_data[o++] =  s->tvoc_ppb       & 0xFF;
    packed_data[o++] =  s->aqi;

    // AS7343: 405,425,450,475,515,550,555,600,640,690,745,855,VISIBLE
    for (int i = 0; i < AS7343_NUM_CH; i++) {
        uint16_t v = s->as7343[i];
        packed_data[o++] = (v >> 8) & 0xFF;
        packed_data[o++] =  v       & 0xFF;
    }

    // Battery
    packed_data[o++] = (s->batt_mV >> 8) & 0xFF;
    packed_data[o++] =  s->batt_mV       & 0xFF;

    // Notify if enabled
    if (notify_enabled && current_conn) {
        int err = bt_gatt_notify(current_conn, &custom_svc.attrs[1],
                                 packed_data, PACKED_DATA_LEN);
        if (err) {
            printk("[TRACKER] notify failed (%d)\n", err);
        }
    }
}

void tracker_thread(void)
{   
    // k_sleep(K_SECONDS(5));
     /* Hard-stop all I2C before touching BT */
    // i2c_gate_acquire();

    /* init_bluetooth() should just call bt_enable(cb) and return 0 on success */
    int rc = init_bluetooth();
    if (rc) {
        // i2c_gate_release();
        return; /* or retry later */
    }

    while (1) {
        // i2c_gate_acquire();
        if (start_advertising()) break;

        for (int i = 0; i < 10; ++i) {  // ~30 seconds of data, for example
            struct sensor_blk s;
            /* Wait until a full sample is available */
            if (k_msgq_get(&full_q, &s, K_SECONDS(2)) == 0) {
                pack_sensor_data(&s);
                // Optional debug:
                print_packed_data();
            } else {
                /* No sample in 2s: continue (or log) */
            }
        }

        (void)stop_advertising_and_disconnect();
        k_sleep(K_SECONDS(5));
        // i2c_gate_release();
    }
}

// /**
//  * @brief Testing tracker thread sending dummy data (minimal schema, no hash)
//  */
// void tracker_thread(void)
// {
//     int err = init_bluetooth();
//     if (err) {
//         printk("[TRACKER] Bluetooth init failed\n");
//         return;
//     }
//     printk("here\n");
//     // Fixed wall-clock start time (same as before)
//     struct tm timeinfo = {
//         .tm_year = 2025 - 1900,  // years since 1900
//         .tm_mon  = 4,            // May (0-indexed)
//         .tm_mday = 25,
//         .tm_hour = 12,
//         .tm_min  = 38,
//         .tm_sec  = 0,
//     };
//     uint32_t unix_ts = (uint32_t)timeutil_timegm(&timeinfo);
//     printk("Unix timestamp: %u\n", unix_ts);

//     // --- Seed example sensor data (scale factors: T*100, RH*100, P*10) ---
//     struct sensor_blk s = {
//         .time          = unix_ts,
//         .uptime_ms     = k_uptime_get_32(),
//         .proto_ver     = PROTO_VER,
//         .dev_id        = DEV_ID,

//         // BME280
//         .temp_c_x100   = 2350,   // 23.50 °C
//         .rh_x100       = 4567,   // 45.67 %RH
//         .press_hPa_x10 = 10132,  // 1013.2 hPa

//         // ENS160
//         .eco2_ppm      = 415,    // ppm
//         .tvoc_ppb      = 28,     // ppb
//         .aqi           = 1,      // 0..5 (pass-through)

//         // AS7343 channels in this order:
//         // 405,425,450,475,515,550,555,600,640,690,745,855, VISIBLE
//         .as7343 = {
//             12, 14, 18, 20, 22, 24, 23, 19, 16, 13, 9, 6,  75 /*VISIBLE sum*/
//         },

//         .batt_mV       = 3820,   // 3.82 V battery
//     };

//     while (1) {
//         err = start_advertising();
//         if (err) {
//             printk("[TRACKER] Advertising start failed (%d)\n", err);
//             return;
//         }

//         // Send a few samples at ~1 Hz
//         for (int i = 0; i < 5; i++) {
//             // Refresh dynamic fields
//             s.time       = unix_ts + i;           // tick the epoch so `time` changes
//             s.uptime_ms  = k_uptime_get_32();

//             // Mildly vary readings to see motion in Grafana
//             s.temp_c_x100   += (i % 2 ? 1 : -1);  // ±0.01 °C
//             s.rh_x100       += (i % 2 ? 2 : -2);  // ±0.02 %RH
//             s.press_hPa_x10 += (i % 2 ? 1 : -1);  // ±0.1 hPa

//             s.eco2_ppm      = 410 + (i * 2);     // ppm
//             s.tvoc_ppb      = 25  + (i * 3);     // ppb
//             s.aqi           = 1;                  // keep stable

//             // Wiggle a couple of spectral channels + recompute "VISIBLE"
//             s.as7343[2] += (i % 2 ? 1 : -1);      // 450 nm
//             s.as7343[7] += (i % 2 ? 1 : -1);      // 600 nm
//             uint32_t vis = 0;
//             for (int k = 0; k < 12; k++) vis += s.as7343[k];
//             s.as7343[12] = (uint16_t)vis;         // AS7343_VISIBLE

//             // Trickle battery down a touch
//             if (s.batt_mV > 3600) s.batt_mV--;

//             // Ship it
//             pack_sensor_data(&s);

//             // Optional local debug
//             print_packed_data();

//             k_sleep(K_SECONDS(1));
//         }

//         k_sleep(K_SECONDS(5));
//         err = stop_advertising_and_disconnect();
//         if (err) {
//             printk("[TRACKER] Stop+disconnect failed (%d)\n", err);
//             return;
//         }

//         k_sleep(K_SECONDS(10));  // pause before next burst
//     }
// }

//MAIN.C bluetooth test
// #include <zephyr/kernel.h>
// #include <zephyr/types.h>
// #include <zephyr/sys/printk.h>
// #include <zephyr/logging/log.h>
// #include <zephyr/net_buf.h>
// #include <zephyr/bluetooth/bluetooth.h>
// #include <zephyr/bluetooth/conn.h>
// #include <zephyr/bluetooth/gatt.h>
// #include <zephyr/bluetooth/hci.h>

// #include <string.h>

// #include <bluetooth.h>

// K_THREAD_DEFINE(tracker_tid, TRACKER_CONTROL_STACK_SIZE, tracker_thread, NULL, NULL, NULL, TRACKER_CONTROL_PRIORITY, 0, 0);
