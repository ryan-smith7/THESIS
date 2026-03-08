/*
 * Copyright (c) 2017 ARM Ltd.
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/logging/log.h>
#include <wifi.h>
#include <env.h>

char wifi_ssid[MAX_SSID_LEN] = CONFIG_WIFI_DEFAULT_SSID;
char wifi_psk [MAX_PSK_LEN]  = CONFIG_WIFI_DEFAULT_PSK;

K_MUTEX_DEFINE(wifi_retry_mutex);

static uint8_t wifi_connection_retry_count = 0;
static char    wifi_rety_dots[WIFI_RETRY_COUNT + 1] = {0};

static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(ipv4_address_obtained, 0, 1);

static bool got_ipv4 = false;

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback l4_cb;

LOG_MODULE_REGISTER(wifi_module, LOG_LEVEL_INF);

/* -------------------------------------------------------------------------
 * Event handlers
 * ----------------------------------------------------------------------- */

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
    const struct wifi_status *status = (const struct wifi_status *)cb->info;

    if (status->status) {
        LOG_ERR("Connection request failed (%d)", status->status);
    } else {
        LOG_INF("WiFi connected");
        k_sem_give(&wifi_connected);
    }
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
    const struct wifi_status *status = (const struct wifi_status *)cb->info;

    if (status->status) {
        LOG_ERR("Disconnection request (%d)", status->status);

        k_mutex_lock(&wifi_retry_mutex, K_FOREVER);
        wifi_rety_dots[wifi_connection_retry_count++] = '.';

        if (wifi_connection_retry_count < WIFI_RETRY_COUNT) {
            k_msleep(1000 * wifi_connection_retry_count); /* exponential backoff */
            LOG_INF("Retrying connection (%d)", wifi_connection_retry_count);
            wifi_connect();
        } else {
            LOG_ERR("Max retries reached");
        }

        k_mutex_unlock(&wifi_retry_mutex);
    } else {
        LOG_INF("WiFi disconnected");
        k_sem_take(&wifi_connected, K_NO_WAIT);
    }
}

static void handle_ipv4_result(struct net_if *iface)
{
    if (got_ipv4) {
        return;
    }

    struct net_if_config *cfg  = net_if_get_config(iface);
    struct net_if_ipv4   *ipv4 = cfg->ip.ipv4;
    char buf[NET_IPV4_ADDR_LEN];

    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        const struct net_if_addr_ipv4 *u = &ipv4->unicast[i];

        if (u->ipv4.addr_type != NET_ADDR_DHCP) {
            continue;
        }

        net_addr_ntop(AF_INET, &u->ipv4.address.in_addr, buf, sizeof(buf));
        LOG_INF("IP: %s", buf);

        net_addr_ntop(AF_INET, &u->netmask, buf, sizeof(buf));
        LOG_INF("Subnet: %s", buf);

        net_addr_ntop(AF_INET, &ipv4->gw, buf, sizeof(buf));
        LOG_INF("Gateway: %s", buf);

        got_ipv4 = true;
        net_mgmt_del_event_callback(&ipv4_cb);
        k_sem_give(&ipv4_address_obtained);
        break;
    }
}

/* Zephyr 4.x: mgmt_event is uint64_t */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                     uint64_t mgmt_event,
                                     struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        handle_wifi_connect_result(cb);
    } else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        handle_wifi_disconnect_result(cb);
    } else if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD ||
               mgmt_event == NET_EVENT_IPV4_DHCP_BOUND ||
               mgmt_event == NET_EVENT_L4_CONNECTED) {
        handle_ipv4_result(iface);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void wifi_connection_retry_reset(void)
{
    net_mgmt_del_event_callback(&ipv4_cb);
    net_mgmt_del_event_callback(&wifi_cb);
    net_mgmt_del_event_callback(&l4_cb);

    wifi_callbacks_init();

    k_mutex_lock(&wifi_retry_mutex, K_FOREVER);
    wifi_connection_retry_count = 0;
    memset(wifi_rety_dots, 0, sizeof(wifi_rety_dots));
    k_mutex_unlock(&wifi_retry_mutex);
}

void wifi_connect(void)
{
    struct net_if *iface = net_if_get_default();

    struct wifi_connect_req_params wifi_params = {0};
    wifi_params.ssid        = wifi_ssid;
    wifi_params.ssid_length = strlen(wifi_ssid);
    wifi_params.psk         = wifi_psk;
    wifi_params.psk_length  = strlen(wifi_psk);
    wifi_params.channel     = WIFI_CHANNEL_ANY;
    wifi_params.security    = WIFI_SECURITY_TYPE_PSK;
    wifi_params.band        = WIFI_FREQ_BAND_2_4_GHZ;
    wifi_params.mfp         = WIFI_MFP_OPTIONAL;

    LOG_INF("Connecting to SSID: %s", wifi_params.ssid);

    if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &wifi_params,
                 sizeof(struct wifi_connect_req_params))) {
        LOG_ERR("WiFi connection request failed");
    }
}

void wifi_callbacks_init(void)
{
    got_ipv4 = false;

    net_mgmt_init_event_callback(&wifi_cb,
        wifi_mgmt_event_handler,
        NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(&ipv4_cb,
        wifi_mgmt_event_handler,
        NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&ipv4_cb);

    net_mgmt_init_event_callback(&l4_cb,
        wifi_mgmt_event_handler,
        NET_EVENT_L4_CONNECTED);
    net_mgmt_add_event_callback(&l4_cb);
}

void wifi_thread(void)
{
    LOG_INF("WiFi thread started");

    wifi_callbacks_init();
    wifi_connect();

    k_sem_take(&wifi_connected, K_FOREVER);
    LOG_INF("WiFi associated");

    k_sem_take(&ipv4_address_obtained, K_FOREVER);
    LOG_INF("IP address obtained — WiFi ready");

    /* Thread exits; main.c proceeds to start azure_mqtt_thread */
}