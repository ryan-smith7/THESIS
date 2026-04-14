/*
 * azure_mqtt.c — MQTT client via Oracle plaintext bridge to Azure IoT Hub
 *
 * Single-threaded MQTT ownership — BLE thread queues messages via ring
 * buffer, azure_mqtt_thread dequeues and publishes. No fixed-slot waste.
 *
 * ESP32-WROOM workaround: BT + MbedTLS cannot coexist in 96KB dram1.
 * Production: ESP32-WROVER + direct TLS to Azure.
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

LOG_MODULE_REGISTER(azure_mqtt, LOG_LEVEL_INF);

/* ── Ring buffer ─────────────────────────────────────────── */
/*
 * Dynamic byte-level queue — each message prefixed with a 2-byte length
 * header so the consumer can read variable-length messages cleanly.
 *
 * 8KB covers ~40 env msgs (200B), ~16 spectrum msgs (400B), or any mix.
 *
 * WiFi build:    buffer in SPIRAM  (.ext_ram.bss)
 * Ethernet build: buffer in dram1  (.dram1.bss)
 */
#define MQTT_QUEUE_MAX_LEN   4096  /* max single message — sound JSON */

#if defined(CONFIG_ESP_SPIRAM)
#define MQTT_RING_BUF_SIZE   2 * MQTT_QUEUE_MAX_LEN
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

bool azure_mqtt_is_connected(void)
{
    return connected;
}

/* ── Event handler ───────────────────────────────────────── */
static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
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

/* ── Connect ─────────────────────────────────────────────── */
static int do_connect(void)
{
    struct addrinfo *result;
    struct sockaddr_in *broker4;
    char port_str[8];

    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    snprintf(port_str, sizeof(port_str), "%d", AZURE_MQTT_PORT);

    LOG_INF("Connecting to %s:%d ...", AZURE_IOT_HUB_HOSTNAME, AZURE_MQTT_PORT);

    int ret = getaddrinfo(AZURE_IOT_HUB_HOSTNAME, port_str, &hints, &result);
    if (ret != 0) {
        LOG_ERR("DNS failed: %d", ret);
        return -ENETUNREACH;
    }

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

int azure_mqtt_connect(void)
{
    return do_connect();
}

/* ── Publish (called from BLE thread — enqueues into ring buffer) ─────── */
int azure_mqtt_publish(const char *json)
{
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

/* ── Internal publish (called only from MQTT thread) ─────── */
static void do_publish(void)
{
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

/* ── Thread ──────────────────────────────────────────────── */
K_THREAD_STACK_DEFINE(azure_mqtt_stack, AZURE_MQTT_STACK);
static struct k_thread azure_mqtt_tid;

void azure_mqtt_thread(void)
{
    LOG_INF("azure_mqtt_thread started");

    ring_buf_init(&mqtt_ring, MQTT_RING_BUF_SIZE, mqtt_ring_data);
    k_mutex_init(&mqtt_ring_mutex);

    struct zsock_pollfd fds;

    while (true) {
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
        if (zsock_poll(&fds, 1, 100) > 0) {
            mqtt_input(&client);
        }
        mqtt_live(&client);
    }
}

void azure_mqtt_thread_start(void)
{
    k_thread_create(&azure_mqtt_tid,
                    azure_mqtt_stack,
                    K_THREAD_STACK_SIZEOF(azure_mqtt_stack),
                    (k_thread_entry_t)azure_mqtt_thread,
                    NULL, NULL, NULL,
                    AZURE_MQTT_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&azure_mqtt_tid, "azure_mqtt");
}