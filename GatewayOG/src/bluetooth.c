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

#define MAX_CONN 4 // Maximum peripherals to connect to
#define UNSET 0
#define SET 1
#define INVALID -1
#define MSGQ_MAX_MSGS 100

#define PACKED_DATA_LEN 51

LOG_MODULE_REGISTER(bluetooth_module);

/* GLOBAL VARIABLES*/

// UUIDs for tracker service and characteristic
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
// UUID for connection characteristic
static struct bt_uuid_16 conn_characteristic_uuid = BT_UUID_INIT_16(0x2902);

// Bluetooth state
static struct bt_conn *conns[MAX_CONN];
static struct bt_gatt_subscribe_params subs[MAX_CONN];
static uint16_t char_handles[MAX_CONN];
static uint16_t ccc_handles[MAX_CONN];
// Discovery parameters per connection (could be static or allocated per connection)
struct bt_gatt_discover_params discover_params[MAX_CONN];
struct bt_gatt_discover_params cccd_discover_params[MAX_CONN];
static struct bt_gatt_write_params write_params_array[MAX_CONN];

// Packet structure to pass through message queue
struct sensor_packet_t {
    uint8_t data[PACKED_DATA_LEN];
};

K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_packet_t), MSGQ_MAX_MSGS, 4);

uint8_t update_dev_id = UNSET; // Whether to gatt write to update connected device ID

static uint8_t data_to_write[1]; // data to write for gatt write callback (device ID)

/* FUNCTION PROTOTYPES*/

/* HELPER FUNCTIONS*/
static int get_conn_index(struct bt_conn *conn);
static uint16_t check_integer(const char* string);
static bool is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid);

/* CLI COMMANDS*/
static int cmd_set_device(const struct shell *shell, size_t argc, char **argv);

/* GATT CALLBACK FUNCTIONS*/
static void write_cb(struct bt_conn *conn, uint8_t err,
                     struct bt_gatt_write_params *params);
static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length);

/* BLUETOOTH DISCOVER FUNCTIONS FOR GATT HANDLERS*/
static void discover_tracker_characteristic(struct bt_conn *conn, int index);
static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params);
static uint8_t cccd_discover_func(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params);

/* BLUETOOTH CONNECTION/ SET-UP FUNCITONS*/
static void start_scan(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad);
static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params);
static void connected(struct bt_conn *conn, uint8_t err);
static void disconnected(struct bt_conn *conn, uint8_t reason);

/* THREADS*/
extern void base_thread(void);
extern void process_data_thread(void);

//Bluetooth connection callbacks structure:
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

// Parameters for GATT MTU exchange
struct bt_gatt_exchange_params mtu_params = {
    .func = exchange_func,
};

/**
 * @brief Get the index of a Bluetooth connection from the connection array.
 *
 * @param conn Pointer to the Bluetooth connection to search for.
 * @return Index of the connection in the `conns` array, or INVALID if not found.
 */
static int get_conn_index(struct bt_conn *conn) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (conns[i] == conn) {
            return i;
        }
    }
    return INVALID;
}

/**
 * @brief Validate and convert a numeric string to an integer.
 *
 * @param string Pointer to a null-terminated string to validate and convert.
 * @return The integer value of the string if valid, or `INVALID` if the string
 *         contains non-digit characters.
 */
uint16_t check_integer(const char* string) {

   for (uint8_t i = 0; string[i] != '\0'; i++) {
        if (!isdigit(string[i])) {
            return INVALID;
        }
   }

   return atoi(string);
}

/**
 * @brief Check if a specific 128-bit UUID is present in advertisement data.
 *
 * @param ad Pointer to the advertisement data buffer (`net_buf_simple`) containing raw advertisement bytes.
 * @param uuid Pointer to the target UUID to search for in the advertisement data.
 *
 * @return true if the UUID is found in the advertisement data, false otherwise.
 */
static bool is_uuid_in_ad(struct net_buf_simple *ad, const struct bt_uuid *uuid) {
    size_t offset = 0;

    // Iterate through the advertisement data
    while (offset < ad->len) {
        // Get the length of the current AD structure
        uint8_t len = ad->data[offset];
        // Stop parsing if length is zero or would exceed buffer length
        if (len == 0 || (offset + len) >= ad->len) {
            break;
        }
         // Get the AD type (e.g., UUID list, flags, name, etc.)
        uint8_t type = ad->data[offset + 1];

        // Pointer to the actual data after type field
        const uint8_t *data = &ad->data[offset + 2];

        // Length of the actual payload data (excluding the type byte)
        uint8_t data_len = len - 1;
         // Check if the AD type contains 128-bit UUIDs (complete or partial list)
        if (type == BT_DATA_UUID128_ALL || type == BT_DATA_UUID128_SOME) {
            // Iterate over each 128-bit UUID block in the data
            for (size_t i = 0; i + 16 <= data_len; i += 16) {

                // Copy 16 bytes (128-bit UUID) into a temporary structure
                struct bt_uuid_128 adv_uuid;
                memcpy(adv_uuid.val, &data[i], 16);
                adv_uuid.uuid.type = BT_UUID_TYPE_128;

                // Compare the extracted UUID with the target UUID
                if (!bt_uuid_cmp(uuid, &adv_uuid.uuid)) {
                    return true; //MATCH FOUND
                }
            }
        }
        // Move to the next AD structure
        offset += len + 1;
    }
    return false; //UUID NOT FOUND
}

/**
 * @brief Shell command to set the device ID in the gatt write buffer for the next BLE connection.
 *
 * @param shell Pointer to the shell instance (used for printing messages).
 * @param argc Argument count; should be 2 for proper usage.
 * @param argv Argument vector; expects the second argument to be the device ID string.
 * 
 * @return 0 on success, -EINVAL (invalid argument) on failure due to argument count, non-numeric input,
 *         or value out of acceptable range.
 */
static int cmd_set_device(const struct shell *shell, size_t argc, char **argv) {
    //Ensure correct amount of arguments
    if (argc != 2) {
        shell_print(shell, "Usage: set_device <value (0-255)>");
        return -EINVAL;
    }
    //Check inputted device id is a valid integer and in range
    uint16_t value = check_integer(argv[1]);
    if (value == INVALID) {
        shell_print(shell, "Value needs to be numerical");
        return -EINVAL;
    }
    if (value > 255) {
        shell_print(shell, "Value out of range (0–255)");
        return -EINVAL;
    }

    //Store the device ID value in gatt write buffer for later bluetooth gatt write
    data_to_write[0] = (uint8_t)value;
    shell_print(shell, "Next connection Dev ID will change to: %d", data_to_write[0]);

    //Set global variable so the ID of the next connected device is updated
    update_dev_id = SET;

    return 0;
}

/**
 * @brief Callback invoked after a GATT write operation completes.
 *
 * @param conn   Pointer to the Bluetooth connection object.
 * @param err    Error code: 0 on success, or a non-zero value on failure.
 * @param params Pointer to the GATT write parameters structure used in the operation.
 */
static void write_cb(struct bt_conn *conn, uint8_t err,
                     struct bt_gatt_write_params *params) {
    if (err) {
        printk("Write failed: 0x%02x\n", err);
    } else {
        printk("Write successful\n");
    }
}

/**
 * @brief Callback function for handling GATT notifications.
 *
 * This function is triggered when a GATT notification is received from a
 * subscribed characteristic. It validates the packet length, copies the
 * received data into a `sensor_packet_t` structure, and attempts to place it
 * into a message queue.
 *
 * @param conn   Pointer to the Bluetooth connection that sent the notification.
 * @param params Pointer to the subscription parameters used for this notification.
 * @param data   Pointer to the received data payload.
 * @param length Length of the data payload in bytes.
 *
 * @return BT_GATT_ITER_CONTINUE to continue receiving notifications.
 */
static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length)
{
    if (!data || length != PACKED_DATA_LEN) {   // was 68
        return BT_GATT_ITER_CONTINUE;
    }

    printk("[BASE] notify len=%u (expect 51)\n", length);
    
    struct sensor_packet_t pkt;
    memcpy(pkt.data, data, PACKED_DATA_LEN);    // was 68
    k_msgq_put(&sensor_msgq, &pkt, K_NO_WAIT);
    return BT_GATT_ITER_CONTINUE;
}

/**
 * @brief Initiates GATT discovery for a characteristic on a connected device.
 *
 * This function begins a GATT characteristic discovery procedure to locate the
 * custom tracker characteristic for the connected tracker bluetooth gatt connection
 * The discovered characteristic will be handled by the discover_func
 *
 * @param conn  Pointer to the Bluetooth connection representing the remote device.
 * @param index Index into the discovery parameter array, used to track per-connection discovery state.
 */
static void discover_tracker_characteristic(struct bt_conn *conn, int index) {

    discover_params[index].uuid = &tracker_char_uuid.uuid; //UUID of target charactreristic
    discover_params[index].func = discover_func; //callback function to be called upon discovery
    // Range of characteristic handles to search within 
    discover_params[index].start_handle = 0x0001;
    discover_params[index].end_handle = 0xffff;
    discover_params[index].type = BT_GATT_DISCOVER_CHARACTERISTIC; // type of discovery set to charactreristic

    // Initiate the discovery procedure
    int err = bt_gatt_discover(conn, &discover_params[index]);
    if (err) {
        LOG_ERR("[BASE %d] Discover characteristic failed (%d)", index, err);
    }
}

/**
 * @brief GATT discovery callback for tracker characteristic.
 *
 * This function is called as a result of the bt_gatt_discover() call initiated
 * in discover_tracker_characteristic(). It stores the target characteristic handle
 * then proceeds to discover the CCCD.
 * If update_dev_id is set, it also writes a value to the characteristic.
 *
 * @param conn    Pointer to the Bluetooth connection associated with the discovery.
 * @param attr    Pointer to the discovered attribute.
 * @param params  Pointer to the discovery parameters used in the original request.
 *
 * @return BT_GATT_ITER_STOP to stop the discovery after the characteristic is found.
 */
static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
    int index = get_conn_index(conn);

    // Characteristic wasn't found is attr is NULL
    if (!attr) {
        LOG_ERR("[BASE %d] Characteristic not found", index);
        return BT_GATT_ITER_STOP;
    }

    // Save characteristic handle so can use in later notifications for a connection
    const struct bt_gatt_chrc *gatt_chrc = attr->user_data;
    char_handles[index] = gatt_chrc->value_handle;
    LOG_INF("[BASE %d] Char handle: 0x%04x", index, char_handles[index]);

    // Set up the discovery for the Client Characteristic Configuration Descriptor (CCCD) 
    cccd_discover_params[index].uuid = &conn_characteristic_uuid.uuid; //specific CCCD UUID
    cccd_discover_params[index].start_handle = char_handles[index]; // located after custom tracker characteristic (defined in tracker bluetooth.c)
    cccd_discover_params[index].end_handle = 0xffff;
    cccd_discover_params[index].type = BT_GATT_DISCOVER_ATTRIBUTE; // Generic attribute discovery
    cccd_discover_params[index].func = cccd_discover_func; // Callback for CCCD discovery

    // Start CCCD discovery
    int err = bt_gatt_discover(conn, &cccd_discover_params[index]);
    if (err) {
        LOG_ERR("[BASE %d] CCCD discover failed (%d)", index, err);
        return BT_GATT_ITER_STOP;
    }

    //All parameters now found: write device ID if indicated to tracker characteric to gatt write to tracker node

    struct bt_gatt_write_params *write_params = &write_params_array[index];
    if (update_dev_id) {
        write_params->handle = char_handles[index]; // Use the discovered handle
        write_params->offset = 0;
        write_params->data = data_to_write; //Global data buffer
        write_params->length = sizeof(data_to_write); // 1 byte
        write_params->func = write_cb; //Callback for write complete
        
        //initiate the write to the characteristic
        int err_write = bt_gatt_write(conn, write_params);
        if (err_write) {
            LOG_ERR("bt_gatt_write failed (err %d)", err_write);
        }
        //UNSET so doesn't update next connections dev_id
        update_dev_id = UNSET;
    }

    return BT_GATT_ITER_STOP;  // stop after finding the characteristic
}

/**
 * @brief Callback function for discovering the CCCD (Client Characteristic Configuration Descriptor).
 *
 * CCCD is a type of attribute in bluetooth gatt allowing a client to enable or disable notificaitons
 * for a characteristic (i.e. tracker data). If found the handle is stored and a subscription
 * is established enable notifications from the subscribed to peripheral (tracker)
 *
 * @param conn    The Bluetooth connection object.
 * @param attr    The discovered attribute
 * @param params  Discovery parameters used for the current operation.
 *
 * @return BT_GATT_ITER_STOP to stop further discovery once the CCCD is found or not found.
 */
static uint8_t cccd_discover_func(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params) {
    //get index for connection stored in connections array
    int index = get_conn_index(conn);

    // Characteristic wasn't found is attr is NULL
    if (!attr) {
        LOG_ERR("[BASE %d] CCCD not found", index);
        return BT_GATT_ITER_STOP;
    }

    // Store handle for later use
    ccc_handles[index] = attr->handle;
    LOG_INF("[BASE %d] CCCD handle: 0x%04x", index, ccc_handles[index]);

    // Fill out subscription struct details
    subs[index].ccc_handle = ccc_handles[index]; //set handle/location of CCCD
    subs[index].value_handle = char_handles[index]; //set handle/location of value characteristic to subscribe to (tracker data)
    subs[index].notify = notify_func; // Callback function for when a notification is recieved
    subs[index].value = BT_GATT_CCC_NOTIFY; // Enable notificiations (writes value to tracker to indicate subscription)

    //Subscribe to the peripheral in the connnection with the subscription details
    int err = bt_gatt_subscribe(conn, &subs[index]);
    if (err) {
        LOG_ERR("[BASE %d] Subscribe failed (%d)", index, err);
    } else {
        LOG_INF("[BASE %d] Subscribed to notifications", index);
    }

    return BT_GATT_ITER_STOP;
}


/**
 * @brief Start BLE scanning to discover nearby devices.
 *
 * This function initiates a passive BLE scan that listens for advertisements but doesn't send scans request 
 * (additional scan response data: i.e. asking for CONFIG_DEVICE_NAKE in sd array in tacker bluetooth.c)
 * A passive scan means the device listens for advertisements but does not
 *
 * If the scan starts successfully, the device_found() callback is called for each discovered device.
 */
static void start_scan(void) {

    struct bt_le_scan_param scan_params = {
        .type = BT_HCI_LE_SCAN_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE, // no scan option set (could potientialy use filter duplicates)
        .interval = BT_GAP_SCAN_FAST_INTERVAL, //time between scan windows
        .window = BT_GAP_SCAN_FAST_WINDOW, // duration of scan window
    };

    //start scan with parameter, setting device found callback for each discovered device
    int err = bt_le_scan_start(&scan_params, device_found);
    if (err) {
        LOG_ERR("[BASE] Scan failed to start (%d)", err);
    } else {
        LOG_INF("[BASE] Scanning for tracker...");
    }
}

/**
 * @brief Callback triggered when a BLE device is found during scanning.
 *
 * This function filters advertising packets to find devices advertising
 * the target tracker service UUID. If such a device is found and there's an
 * available connection slot, it attempts to connect.
 *
 * - Skips non-connectable advertisements.
 * - Skips devices not advertising the target service UUID.
 * - Avoids reconnecting to already connected devices.
 * - Stops scanning before initiating connection.
 *
 * @param addr BLE address of the advertising device.
 * @param rssi Signal strength of the received advertisement.
 * @param type Advertisement type.
 * @param ad Advertisement data buffer.
 */
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad) {

    // Filter out non-connectable advertisements
    if (type != BT_GAP_ADV_TYPE_ADV_IND &&
        type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        return;
    }

    // Check if the advertised data contains the target tracker service UUID
    if (!is_uuid_in_ad(ad, &tracker_service_uuid.uuid)) {
        return;
    }

    // Check if already connected to this device
    struct bt_conn *existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
    if (existing) {
        LOG_WRN("[BASE] Already connected to this device");
        bt_conn_unref(existing); //unreference as won't use
        return;
    }
    //Check if a slot is available in pre-set amount of MAX connections (for pre-defined arrays)
    bool slot_available = false;
    int index_available;
    for (int i = 0; i < MAX_CONN; i++) {
        if (!conns[i]) {
            slot_available = true;
            index_available = i;
            break;
        }
    }

    if (!slot_available) {  
        // All slots full
        return;
    }

    // Convert address to a string
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INF("[BASE] Found tracker: %s (RSSI %d)", addr_str, rssi);
    
    // stop scanning before initiate connectioning, connect and store connection reference
    bt_le_scan_stop();
    conns[index_available] = NULL;
    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                                BT_LE_CONN_PARAM_DEFAULT, &conns[index_available]);
    if (err) {
        LOG_ERR("[BASE] Failed to connect (%d)", err);
        start_scan();
    }
}

/**
 * @brief Callback function for GATT MTU exchange result.
 *
 * This function is called after an MTU exchange request complete.
 *
 * @param conn   Pointer to the Bluetooth connection structure.
 * @param err    Error code from the MTU exchange (0 on success).
 * @param params Pointer to the GATT exchange parameters (defined above though unused).
 */
void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params) {
    if (err) {
        LOG_ERR("MTU exchange failed (err %u)", err);
    } else {
        // get the negotiated Maximum transmission unit (MTU) size for connection (sizes define in prj.conf)
        uint16_t mtu = bt_gatt_get_mtu(conn);
        LOG_INF("MTU exchanged successfully: %d", mtu);
    }
}

/**
 * @brief Callback function to handle connection events for when a Bluetooth connection
 *          is established or fails.
 *
 * If there is an error during connection, it logs the error,
 * releases the connection reference, and restarts scanning.
 * 
 * If the connection is successful, it assigns the connection to an available slot,
 * logs the connection info, initiates MTU exchange, starts service discovery,
 * and restarts scanning to find other devices.
 *
 * @param conn Pointer to the Bluetooth connection object.
 * @param err  Error code indicating the connection result. Zero means success.
 */
static void connected(struct bt_conn *conn, uint8_t err) {
    //Convert bluetooth address to a string
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    //Check if a connection failed
    if (err) {
        LOG_ERR("[BASE] Failed to connect to %s (err %u)", addr, err);
        bt_conn_unref(conn);
        start_scan();
        return;
    }
    // Ckeck is there is an available index for the connection in the connection array
    int index = get_conn_index(conn);
    if (index < 0) {
        LOG_INF("[BASE] No free conn slots");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        start_scan();
        return;
    }

    LOG_INF("[BASE] Connected [%d]: %s", index, addr);

    // Exchange the MTU with the connection device (peripheral) to negotiate the data size
    bt_gatt_exchange_mtu(conn, &mtu_params);

    // Start the characteristic discoveries for the tracker peripheral device (characteristic value and CCCD)
    discover_tracker_characteristic(conn, index);

    //Restart scanning so can find other devices for multi connections
    start_scan();
}


/**
 * @brief Callback function to handle disconnection events for when a Bluetooth diconnection
 *          occur
 *
 * Get the index of connection and unreferences.
 *
 * @param conn Pointer to the Bluetooth connection object.
 * @param reason  Reason code for why device disconnected
 */
static void disconnected(struct bt_conn *conn, uint8_t reason) {

    // get index of connection for connection array
    int index = get_conn_index(conn);
    LOG_INF("[BASE] Disconnected [%d] (reason 0x%02x)", index, reason);

    //If connection was stored unreference and remove handles (avoids error with future reconnection)
    if (index >= 0) {
        bt_conn_unref(conns[index]);
        conns[index] = NULL;
        char_handles[index] = 0;
    }
}

/**
 * @brief Thread that processes incoming sensor packets into JSON.
 */
void process_data_thread(void)
{
    struct sensor_packet_t pkt;

    while (1) {
        k_msgq_get(&sensor_msgq, &pkt, K_FOREVER);

        tracker_payload_t p = {0};
        size_t i = 0;

        // time (0..3)
        p.time =  ((uint32_t)pkt.data[i] << 24) |
                  ((uint32_t)pkt.data[i+1] << 16) |
                  ((uint32_t)pkt.data[i+2] << 8) |
                   (uint32_t)pkt.data[i+3];
        i += 4;

        // uptime_ms (4..7)
        p.uptime_ms = ((uint32_t)pkt.data[i] << 24) |
                      ((uint32_t)pkt.data[i+1] << 16) |
                      ((uint32_t)pkt.data[i+2] << 8) |
                       (uint32_t)pkt.data[i+3];
        i += 4;

        // proto, dev (8,9)
        p.proto_ver = pkt.data[i++];
        p.dev_id    = pkt.data[i++];

        // BME280 (10..15)
        p.temp_c_x100   = (int16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.rh_x100       = (int16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.press_hPa_x1000 =
            ((uint32_t)pkt.data[i]   << 24) |
            ((uint32_t)pkt.data[i+1] << 16) |
            ((uint32_t)pkt.data[i+2] <<  8) |
            (uint32_t)pkt.data[i+3];
        i += 4;

        // ENS160 (16..20)
        p.eco2_ppm = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.tvoc_ppb = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]); i += 2;
        p.aqi      = pkt.data[i++];

        // AS7343 13x uint16 (21..46)
        for (int k = 0; k < 13; k++) {
            p.as7343[k] = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]);
            i += 2;
        }

        // Battery (47..48)
        p.batt_mV = (uint16_t)((pkt.data[i] << 8) | pkt.data[i+1]);

        // ---- Build & print JSON ----
        struct json_full_packet jp = {0};
        fill_json_packet(&p, &jp);
        encode_and_print_json(&jp);
    }
}


/**
 * @brief Main thread for the base node Bluetooth functionality.
 *
 * This thread initializes Bluetooth, registers shell commands and connection 
 * callbacks, and starts scanning for devices.
 */
void base_thread(void) {
    printk("we're alive");
    //register shell command
    // SHELL_CMD_REGISTER(set_device, NULL, "Set device ID hence first byte of data_to_write (0–255)", cmd_set_device);
    //enable bluetooth
    int err = bt_enable(NULL);
    if (err) {
        LOG_ERR("[BASE] Bluetooth init failed (err %d)", err);
        return;
    }

    LOG_INF("[BASE] Bluetooth initialized");
    //set the connection and disconnection callbacks
    bt_conn_cb_register(&conn_callbacks);
    // Start blutooth scanning to discover devices
    start_scan();
    printk("we're alive");
}

void test_thread(void) {

    printk("we're alive");

}