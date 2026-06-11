/*
 * azure_mqtt.c — MQTT client to Oracle plaintext MQTT broker
 *
 * Single-threaded MQTT ownership — BLE thread queues messages via ring
 * buffer, azure_mqtt_thread dequeues and publishes. 
 */

#include "azure_mqtt.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/mutex.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_ETH_GATEWAY
    #include "ethernet.h"
#endif


LOG_MODULE_REGISTER(azure_mqtt, LOG_LEVEL_INF);

/* ── Ring buffer ─────────────────────────────────────────── */
/*
 * Dynamic byte-level queue — each message prefixed with a 2-byte length
 * WiFi build:    buffer in SPIRAM  (.ext_ram.bss)
 * Ethernet build: buffer in dram1  (.dram1.bss)
 */
#define MQTT_QUEUE_MAX_LEN   4096  /* max single message — sound JSON */

#if defined(CONFIG_ESP_SPIRAM)
#define MQTT_RING_BUF_SIZE   8 * MQTT_QUEUE_MAX_LEN
static uint8_t mqtt_ring_data[MQTT_RING_BUF_SIZE]
    __attribute__((section(".ext_ram.bss")));
#else
#define MQTT_RING_BUF_SIZE   MQTT_QUEUE_MAX_LEN
static uint8_t mqtt_ring_data[MQTT_RING_BUF_SIZE];
#endif

static struct ring_buf mqtt_ring;
static struct k_mutex  mqtt_ring_mutex;

/* ── MQTT client state ───────────────────────────────────── */
static struct mqtt_client      client;
static struct sockaddr_storage broker_addr;

static uint8_t rx_buf[512];
static uint8_t tx_buf[4608];            /* 4096 payload + 512 MQTT overhead */
static uint8_t payload_buf[MQTT_QUEUE_MAX_LEN];

#define AZURE_MQTT_STACK       6144
#define AZURE_MQTT_PRIO        7
#define RECONNECT_DELAY_MS     5000

static bool connected = false;

K_THREAD_STACK_DEFINE(azure_mqtt_stack, AZURE_MQTT_STACK);
static struct k_thread azure_mqtt_tid;

/**
 * @brief Returns true if the MQTT client is currently connected.
 */
bool azure_mqtt_is_connected(void) {
    return connected;
}

/**
 * @brief MQTT event handler for connect, disconnect, puback, and pingresp.
 *
 * Sets or clears the connected flag on CONNACK/DISCONNECT events.
 */
static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt) {

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("CONNACK: connected to Oracle MQTT bridge");
            connected = true;
        } else {
            LOG_ERR("CONNACK error: %d", evt->result);
        }
        break;
    case MQTT_EVT_DISCONNECT:
        LOG_WRN("Disconnected: %d", evt->result);
        connected = false;
        break;
    case MQTT_EVT_PUBACK:
        LOG_DBG("PUBACK mid=%u", evt->param.puback.message_id);
        break;
    case MQTT_EVT_PINGRESP:
        LOG_DBG("PINGRESP");
        break;
    default:
        break;
    }
}

/**
 * @brief Resolve the broker address, initialise the MQTT client, and connect.
 *
 * Polls for CONNACK with a 5s timeout. Returns 0 on success or a
 * negative errno on DNS failure, connect failure, or timeout.
 */
static int do_connect(void) {

    struct addrinfo *result;
    struct sockaddr_in *broker4;
    char port_str[8];

    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    snprintf(port_str, sizeof(port_str), "%d", AZURE_MQTT_PORT);

    LOG_INF("Connecting to %s:%d ...", AZURE_IOT_HUB_HOSTNAME, AZURE_MQTT_PORT);

    /*this currently irrelevant should remove */
    int ret = getaddrinfo(AZURE_IOT_HUB_HOSTNAME, port_str, &hints, &result);
    if (ret != 0) {
        LOG_ERR("DNS reolve failed: %d", ret);
        return -ENETUNREACH;
    }
    /*should remove above as no longer doing DNS*/

    broker4 = (struct sockaddr_in *)&broker_addr;
    broker4->sin_addr.s_addr =
        ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
    broker4->sin_family = AF_INET;
    broker4->sin_port   =
        ((struct sockaddr_in *)result->ai_addr)->sin_port;
    freeaddrinfo(result);

    static char client_id_str[32];
    snprintf(client_id_str, sizeof(client_id_str),
             "esp32-%08x", (uint32_t)k_uptime_get());

    mqtt_abort(&client);
    mqtt_client_init(&client);

    client.broker           = &broker_addr;
    client.evt_cb           = mqtt_evt_handler;
    client.client_id.utf8   = (uint8_t *)client_id_str;
    client.client_id.size   = strlen(client_id_str);
    client.password         = NULL;
    client.user_name        = NULL;
    client.protocol_version = MQTT_VERSION_3_1_1;
    client.rx_buf           = rx_buf;
    client.rx_buf_size      = sizeof(rx_buf);
    client.tx_buf           = tx_buf;
    client.tx_buf_size      = sizeof(tx_buf);
    client.transport.type   = MQTT_TRANSPORT_NON_SECURE;
    client.keepalive        = CONFIG_MQTT_KEEPALIVE;
    client.clean_session    = 1;

    ret = mqtt_connect(&client);
    if (ret < 0) {
        LOG_ERR("mqtt_connect failed: %d", ret);
        return ret;
    }

    struct zsock_pollfd fds = {
        .fd     = client.transport.tcp.sock,
        .events = ZSOCK_POLLIN,
    };

    int rc = zsock_poll(&fds, 1, 5000);
    if (rc > 0) {
        mqtt_input(&client);
    }

    if (!connected) {
        LOG_ERR("CONNACK timeout");
        mqtt_abort(&client);
        return -ETIMEDOUT;
    }

    LOG_INF("Connected as %s", client_id_str);
    return 0;
}

/**
 * @brief Irrelevant Public wrapper around do_connect().
 * 
 * Should come back and remove 
 */
int azure_mqtt_connect(void) {

    return do_connect();
}

/**
 * @brief Enqueue a JSON string into the ring buffer for publishing.
 *
 * Drops the message and returns -ENOMEM if the ring buffer is full,
 * or -ENOTCONN if not connected.
 */
int azure_mqtt_publish(const char *json) {

    if (!connected) {
        LOG_WRN("Not connected — skipping publish");
        return -ENOTCONN;
    }

    uint16_t len = (uint16_t)strlen(json);

    k_mutex_lock(&mqtt_ring_mutex, K_FOREVER);

    if (ring_buf_space_get(&mqtt_ring) < (uint32_t)(sizeof(len) + len)) {
        k_mutex_unlock(&mqtt_ring_mutex);
        LOG_WRN("Ring buffer full — message dropped (%u bytes)", len);
        return -ENOMEM;
    }

    ring_buf_put(&mqtt_ring, (uint8_t *)&len, sizeof(len));
    ring_buf_put(&mqtt_ring, (uint8_t *)json, len);

    k_mutex_unlock(&mqtt_ring_mutex);

    LOG_INF("Queued %u bytes for publish", len);
    return 0;
}

/**
 * @brief Dequeue one message from the ring buffer and publish it.
 *
 * Must only be called from the MQTT thread. Clears connected on
 * publish failure.
 */
static void do_publish(void) {

    k_mutex_lock(&mqtt_ring_mutex, K_FOREVER);

    uint16_t msg_len = 0;
    if (ring_buf_get(&mqtt_ring, (uint8_t *)&msg_len, sizeof(msg_len))
            != sizeof(msg_len)) {
        k_mutex_unlock(&mqtt_ring_mutex);
        return;
    }

    ring_buf_get(&mqtt_ring, payload_buf, msg_len);
    k_mutex_unlock(&mqtt_ring_mutex);

    payload_buf[msg_len] = '\0';

    struct mqtt_publish_param pub = {
        .message = {
            .topic = {
                .qos   = MQTT_QOS_0_AT_MOST_ONCE,
                .topic = {
                    .utf8 = (uint8_t *)AZURE_MQTT_TOPIC,
                    .size = strlen(AZURE_MQTT_TOPIC),
                },
            },
            .payload = {
                .data = payload_buf,
                .len  = msg_len,
            },
        },
        .message_id  = (uint16_t)sys_rand32_get(),
        .dup_flag    = 0,
        .retain_flag = 0,
    };

    int ret = mqtt_publish(&client, &pub);
    if (ret < 0) {
        LOG_ERR("mqtt_publish failed: %d", ret);
        connected = false;
        return;
    }

    mqtt_live(&client);
    LOG_INF("Published %u bytes", msg_len);
}

/**
 * @brief Main MQTT thread — connects, drains the ring buffer, and keeps alive.
 *
 * Initialises the ring buffer and mutex, then loops: waits for network
 * readiness, reconnects as needed, publishes queued messages, and polls
 * for incoming packets every 20 ms.
 */
void azure_mqtt_thread(void) {

    LOG_INF("azure_mqtt_thread started");

    ring_buf_init(&mqtt_ring, MQTT_RING_BUF_SIZE, mqtt_ring_data);
    k_mutex_init(&mqtt_ring_mutex);

    struct zsock_pollfd fds;

    while (true) {

        #ifdef CONFIG_ETH_GATEWAY
        if (!ethernet_is_ready()) {
            if (connected) {
                LOG_WRN("Ethernet down — aborting MQTT client");
                mqtt_abort(&client);
                connected = false;
            }
            ethernet_wait_ready(K_FOREVER);
            continue;
        }
        #endif

        /* Reconnect if needed */
        if (!connected) {
            LOG_INF("Connecting to Oracle MQTT bridge ...");
            int ret = azure_mqtt_connect();
            if (ret < 0) {
                LOG_ERR("Connection failed (%d), retrying in %d ms",
                        ret, RECONNECT_DELAY_MS);
                k_sleep(K_MSEC(RECONNECT_DELAY_MS));
                continue;
            }
        }

        /* Check for queued messages from BLE thread — non-blocking */
        if (!ring_buf_is_empty(&mqtt_ring)) {
            do_publish();
        }

        /* Poll for incoming MQTT packets and send keepalives */
        fds.fd     = client.transport.tcp.sock;
        fds.events = ZSOCK_POLLIN;
        if (zsock_poll(&fds, 1, 20) > 0) {
            mqtt_input(&client);
        }
        mqtt_live(&client);
    }
}

/**
 * @brief Spawn the azure_mqtt_thread as a named Zephyr thread.
 */
void azure_mqtt_thread_start(void) {
    k_thread_create(&azure_mqtt_tid,
                    azure_mqtt_stack,
                    K_THREAD_STACK_SIZEOF(azure_mqtt_stack),
                    (k_thread_entry_t)azure_mqtt_thread,
                    NULL, NULL, NULL,
                    AZURE_MQTT_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&azure_mqtt_tid, "azure_mqtt");
}