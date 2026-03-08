/*
 * azure_mqtt.c — Azure IoT Hub MQTT/TLS client for Zephyr 4.x / ESP32
 */

#include "azure_mqtt.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(azure_mqtt, LOG_LEVEL_DBG);

/* -------------------------------------------------------------------------
 * Buffers and state
 * ----------------------------------------------------------------------- */
static struct mqtt_client     client;
static struct sockaddr_storage broker_addr;

static uint8_t rx_buf[1024];
static uint8_t tx_buf[1024];
static uint8_t payload_buf[1400];

static bool connected = false;

/* -------------------------------------------------------------------------
 * TLS credentials
 * ----------------------------------------------------------------------- */
static const char root_ca[] = AZURE_ROOT_CA_CERT;

static int tls_credentials_register(void)
{
    int ret = tls_credential_add(AZURE_TLS_TAG,
                                 TLS_CREDENTIAL_CA_CERTIFICATE,
                                 root_ca, sizeof(root_ca));
    if (ret < 0 && ret != -EEXIST) {LOG_INF("Client ID bytes: %d", (int)client.client_id.size);
        LOG_ERR("Failed to register CA cert: %d", ret);
        return ret;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * DNS resolution
 * ----------------------------------------------------------------------- */
static int resolve_broker(void)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;
    char port_str[8];

    snprintf(port_str, sizeof(port_str), "%d", AZURE_MQTT_PORT);

    LOG_INF("Resolving %s ...", AZURE_IOT_HUB_HOSTNAME);
    int ret = zsock_getaddrinfo(AZURE_IOT_HUB_HOSTNAME, port_str, &hints, &res);
    if (ret != 0) {
        LOG_ERR("DNS failed: %d (errno %d)", ret, errno);
        return -ENETUNREACH;
    }

    memcpy(&broker_addr, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    LOG_INF("DNS resolved");
    return 0;
}

/* -------------------------------------------------------------------------
 * MQTT event callback
 * ----------------------------------------------------------------------- */
static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("CONNACK: connected to Azure IoT Hub");
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
        LOG_DBG("MQTT evt %d", evt->type);
        break;
    }
}

static int do_connect(void)
{
    struct addrinfo *result;
    struct sockaddr_in *broker4;
    char port_str[8];
    static sec_tag_t sec_tags[] = { AZURE_TLS_TAG };
    struct mqtt_sec_config *tls_config;

    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    snprintf(port_str, sizeof(port_str), "%d", AZURE_MQTT_PORT);

    LOG_INF("Resolving %s ...", AZURE_IOT_HUB_HOSTNAME);
    int ret = getaddrinfo(AZURE_IOT_HUB_HOSTNAME, port_str, &hints, &result);
    if (ret != 0) {
        LOG_ERR("DNS failed: %d", ret);
        return -ENETUNREACH;
    }

    broker4 = (struct sockaddr_in *)&broker_addr;
    broker4->sin_addr.s_addr =
        ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
    broker4->sin_family = AF_INET;
    broker4->sin_port =
        ((struct sockaddr_in *)result->ai_addr)->sin_port;
    freeaddrinfo(result);
    LOG_INF("DNS resolved");

    static const char username_str[] = AZURE_MQTT_USERNAME;
    static const char password_str[] = AZURE_SAS_TOKEN;
    static const char client_id_str[] = AZURE_MQTT_CLIENT_ID;

    static struct mqtt_utf8 password;
    static struct mqtt_utf8 username;

    password.utf8 = (uint8_t *)password_str;
    password.size = strlen(password_str);
    username.utf8 = (uint8_t *)username_str;
    username.size = strlen(username_str);

    mqtt_client_init(&client);
    client.broker           = &broker_addr;
    client.evt_cb           = mqtt_evt_handler;
    client.client_id.utf8   = (uint8_t *)client_id_str;
    client.client_id.size   = strlen(client_id_str);
    client.password         = &password;
    client.user_name        = &username;
    client.protocol_version = MQTT_VERSION_3_1_1;
    client.rx_buf           = rx_buf;
    client.rx_buf_size      = sizeof(rx_buf);
    client.tx_buf           = tx_buf;
    client.tx_buf_size      = sizeof(tx_buf);
    client.transport.type   = MQTT_TRANSPORT_SECURE;
    client.keepalive        = CONFIG_MQTT_KEEPALIVE;
    client.clean_session    = 1;

    /* Set TLS config via pointer — same pattern as Zephyr sample */
    tls_config = &client.transport.tls.config;
    tls_config->peer_verify   = TLS_PEER_VERIFY_REQUIRED;
    tls_config->cipher_list   = NULL;
    tls_config->sec_tag_list  = sec_tags;
    tls_config->sec_tag_count = ARRAY_SIZE(sec_tags);
    tls_config->hostname      = AZURE_IOT_HUB_HOSTNAME;

    LOG_INF("MQTT connecting to %s:%d ...", AZURE_IOT_HUB_HOSTNAME, AZURE_MQTT_PORT);

    ret = mqtt_connect(&client);
    if (ret < 0) {
        LOG_ERR("mqtt_connect failed: %d", ret);
        return ret;
    }

    /* Poll for CONNACK — same pattern as sample */
    struct zsock_pollfd fds = {
        .fd     = client.transport.tls.sock,
        .events = ZSOCK_POLLIN,
    };

    int rc = zsock_poll(&fds, 1, 5000);
    if (rc > 0) {
        mqtt_input(&client);
    }

    if (!connected) {
        LOG_ERR("CONNACK failed or timeout");
        mqtt_abort(&client);
        return -ETIMEDOUT;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Connect
 * ----------------------------------------------------------------------- */
// static int do_connect(void)
// {
//     int ret = resolve_broker();
//     if (ret < 0) {
//         return ret;
//     }

//     static sec_tag_t sec_tags[] = { AZURE_TLS_TAG };

//     // struct mqtt_sec_config tls_cfg = {
//     //     // .peer_verify  = TLS_PEER_VERIFY_REQUIRED,
//     //     .peer_verify  = TLS_PEER_VERIFY_NONE,
//     //     .cipher_list  = NULL,
//     //     .sec_tag_list = sec_tags,
//     //     .sec_tag_count = ARRAY_SIZE(sec_tags),
//     //     .hostname     = AZURE_IOT_HUB_HOSTNAME,
//     // };
//     static struct mqtt_sec_config tls_cfg = {
//         .peer_verify   = TLS_PEER_VERIFY_NONE,
//         .cipher_list   = NULL,
//         .sec_tag_list  = sec_tags,
//         .sec_tag_count = ARRAY_SIZE(sec_tags),
//         .hostname      = AZURE_IOT_HUB_HOSTNAME,
//     };

//     static const char username_str[] = AZURE_MQTT_USERNAME;
//     static const char password_str[] = AZURE_SAS_TOKEN;

//     struct mqtt_utf8 password = {
//         .utf8 = (uint8_t *)password_str,
//         .size = strlen(password_str),
//     };
//     struct mqtt_utf8 username = {
//         .utf8 = (uint8_t *)username_str,
//         .size = strlen(username_str),
//     };

//     static const char client_id[] = AZURE_MQTT_CLIENT_ID;
    
//     mqtt_client_init(&client);
//     client.broker           = &broker_addr;
//     client.evt_cb           = mqtt_evt_handler;
//     client.client_id.utf8 = (uint8_t *)client_id;
//     client.client_id.size = strlen(client_id);
//     client.password         = &password;
//     client.user_name        = &username;
//     client.protocol_version = MQTT_VERSION_3_1_1;
//     client.rx_buf           = rx_buf;
//     client.rx_buf_size      = sizeof(rx_buf);
//     client.tx_buf           = tx_buf;
//     client.tx_buf_size      = sizeof(tx_buf);
//     client.transport.type   = MQTT_TRANSPORT_SECURE;
//     client.transport.tls.config = tls_cfg;
//     client.keepalive        = CONFIG_MQTT_KEEPALIVE;
//     client.clean_session    = 0;

//     LOG_INF("Client ID: %s (%d)", AZURE_MQTT_CLIENT_ID, (int)strlen(AZURE_MQTT_CLIENT_ID));
//     LOG_INF("Username: %s (%d)", AZURE_MQTT_USERNAME, (int)strlen(AZURE_MQTT_USERNAME));
//     LOG_INF("Token length: %d", (int)strlen(AZURE_SAS_TOKEN));
//     LOG_INF("Token start: %.50s", AZURE_SAS_TOKEN);
//     LOG_INF("Client ID bytes: %d", (int)client.client_id.size);
//     LOG_HEXDUMP_INF((uint8_t *)AZURE_SAS_TOKEN, 
//                 MIN(strlen(AZURE_SAS_TOKEN), 64),
//                 "Token hex:");

//     struct sockaddr_in *addr = (struct sockaddr_in *)&broker_addr;
//     LOG_INF("Broker IP port: %d", ntohs(addr->sin_port));

//     LOG_INF("MQTT connecting to %s:%d ...", AZURE_IOT_HUB_HOSTNAME, AZURE_MQTT_PORT);
//     LOG_INF("Connect flags check - password ptr: %p, username ptr: %p",
//         (void*)client.password, (void*)client.user_name);
    
//     ret = mqtt_connect(&client);
//     if (ret < 0) {
//         LOG_ERR("mqtt_connect failed: %d", ret);
//         return ret;
//     }
//     LOG_INF("Transport sock: %d", client.transport.tls.sock);

//     /* Poll for CONNACK */
//     struct zsock_pollfd fds = {
//         .fd     = client.transport.tls.sock,
//         .events = ZSOCK_POLLIN,
//     };

//     for (int i = 0; i < 20 && !connected; i++) {
//         if (zsock_poll(&fds, 1, 1000) > 0) {
//             mqtt_input(&client);
//         }
//         mqtt_live(&client);
//     }

//     if (!connected) {
//         LOG_ERR("CONNACK timeout");
//         mqtt_disconnect(&client, NULL);
//         return -ETIMEDOUT;
//     }

//     return 0;
// }

/* -------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
int azure_mqtt_connect(void)
{
    int ret = tls_credentials_register();
    if (ret < 0) {
        return ret;
    }
    return do_connect();
}

int azure_mqtt_publish(const char *json)
{
    if (!connected) {
        LOG_WRN("Not connected — skipping publish");
        return -ENOTCONN;
    }

    size_t len = strlen(json);
    if (len >= sizeof(payload_buf)) {
        LOG_ERR("Payload too large: %zu", len);
        return -EMSGSIZE;
    }
    memcpy(payload_buf, json, len);

    struct mqtt_publish_param pub = {
        .message = {
            .topic = {
                .qos   = MQTT_QOS_1_AT_LEAST_ONCE,
                .topic = {
                    .utf8 = (uint8_t *)AZURE_MQTT_TOPIC,
                    .size = strlen(AZURE_MQTT_TOPIC),
                },
            },
            .payload = {
                .data = payload_buf,
                .len  = len,
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
        return ret;
    }

    LOG_INF("Published %zu bytes", len);
    return 0;
}

void azure_mqtt_process(void)
{
    if (!connected) {
        return;
    }

    struct zsock_pollfd fds = {
        .fd     = client.transport.tls.sock,
        .events = ZSOCK_POLLIN,
    };

    if (zsock_poll(&fds, 1, 0) > 0) {
        mqtt_input(&client);
    }
    mqtt_live(&client);
}

/* -------------------------------------------------------------------------
 * Background thread
 * ----------------------------------------------------------------------- */
#define AZURE_MQTT_STACK     8192
#define AZURE_MQTT_PRIO      7
#define PUBLISH_INTERVAL_MS  30000
#define RECONNECT_DELAY_MS   10000

K_THREAD_STACK_DEFINE(azure_mqtt_stack, AZURE_MQTT_STACK);
static struct k_thread azure_mqtt_tid;

static void build_telemetry_json(char *buf, size_t buf_size, uint32_t uptime_s)
{
    /*
     * Replace the literal sensor strings below with live readings
     * from your IMU / BME680 / GPS drivers once wired up.
     */
    snprintf(buf, buf_size,
        "{"
          "\"header\":{"
            "\"messageId\":\"%08X%08X\","
            "\"gatewayId\":\"GW-01\","
            "\"schemaVersion\":\"1.0\","
            "\"messageType\":\"telemetry\""
          "},"
          "\"payload\":{"
            "\"deviceId\":\"" AZURE_DEVICE_ID "\","
            "\"timestamp\":\"%u\","
            "\"uptime\":\"%u\","
            "\"location\":{"
              "\"latitude\":\"27.5002432\",\"ns\":\"S\","
              "\"longitude\":\"153.0153600\",\"ew\":\"E\","
              "\"altitude_m\":\"0.0\""
            "},"
            "\"environment\":{"
              "\"temperature_c\":\"26.22\","
              "\"humidity_percent\":\"69.00\","
              "\"pressure_hpa\":\"102.3\","
              "\"gas_ppm\":\"28.00\""
            "},"
            "\"acceleration\":{"
              "\"x_mps2\":\"2.413\","
              "\"y_mps2\":\"-0.459\","
              "\"z_mps2\":\"-6.511\""
            "}"
          "},"
          "\"signature\":{"
            "\"alg\":\"HS256\","
            "\"keyId\":\"key-001\","
            "\"value\":\"FBDA00C64513C32B027DA202C4C0574E86CE9FE462C42B8BB3B7734380810DA8\""
          "}"
        "}",
        sys_rand32_get(), sys_rand32_get(),
        uptime_s, uptime_s
    );
}

void azure_mqtt_thread(void)
{
    static char json_buf[1300];

    LOG_INF("azure_mqtt_thread started");

    while (true) {
        if (!connected) {
            LOG_INF("Connecting to Azure IoT Hub ...");
            int ret = azure_mqtt_connect();
            if (ret < 0) {
                LOG_ERR("Connection failed (%d), retrying in %d s",
                        ret, RECONNECT_DELAY_MS / 1000);
                k_sleep(K_MSEC(RECONNECT_DELAY_MS));
                continue;
            }
        }

        azure_mqtt_process();

        static int64_t last_pub = 0;
        int64_t now = k_uptime_get();

        if (now - last_pub >= PUBLISH_INTERVAL_MS) {
            last_pub = now;
            build_telemetry_json(json_buf, sizeof(json_buf),
                                 (uint32_t)(now / 1000));
            azure_mqtt_publish(json_buf);
        }

        k_sleep(K_MSEC(500));
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