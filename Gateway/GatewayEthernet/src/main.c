/*
 * main.c — Combined BLE Central + Network + Azure IoT Hub Gateway
 *
 * Supports two hardware platforms selected at build time:
 *
 *   CONFIG_ETH_GATEWAY=y   →  ESP32-POE  (LAN8720 / RMII Ethernet)
 *   CONFIG_ETH_GATEWAY=n   →  ESP32-WROVER (WiFi)
 *
 * Boot sequence (both platforms):
 *   1. Network thread  — brings up Ethernet/WiFi, obtains DHCP lease (blocks)
 *   2. Azure MQTT thread — connects to IoT Hub, keepalive loop
 *   3. SNTP sync       — sets UTC reference for time_sync_writer
 *   4. BLE threads     — scan, connect to sensor nodes, decode + publish
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "azure_mqtt.h"
#include "bluetooth.h"
// #include "sntp_sync.h"
#include "http_time_sync.h"

#if defined(CONFIG_ETH_GATEWAY)
#  include "ethernet.h"
#  define NET_THREAD_STACK   2048
#  define NET_THREAD_FN      ethernet_thread
#  define GATEWAY_BOARD_STR  "ESP32-POE"
#else
#  include "wifi.h"
#  define NET_THREAD_STACK   4096
#  define NET_THREAD_FN      wifi_thread
#  define GATEWAY_BOARD_STR  "ESP32-WROVER"
#endif

#define NET_THREAD_PRIO  4

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

K_THREAD_STACK_DEFINE(net_stack, NET_THREAD_STACK);
static struct k_thread net_tid;

/*
 * BLE threads — delay 15 s so the network + Azure MQTT are fully up
 * before BLE starts trying to publish.
 * Budget: Ethernet/WiFi ~3–5 s + MQTT connect ~5 s + margin.
 */
K_THREAD_DEFINE(process_data_tid,
                BASE_PROCESS_STACK_SIZE,
                process_data_thread,
                NULL, NULL, NULL,
                BASE_PROCESS_PRIORITY, 0,
                5000);

K_THREAD_DEFINE(base_tid,
                BASE_CONTROL_STACK_SIZE,
                base_thread,
                NULL, NULL, NULL,
                BASE_CONTROL_PRIORITY, 0,
                5000);

int main(void)
{
    LOG_INF("=== Combined BLE+Network Gateway starting (%s) ===",
            GATEWAY_BOARD_STR);

    /*
     * Start the network thread and block until it returns.
     * It only returns after a DHCP lease is obtained.
     */
    k_thread_create(&net_tid,
                    net_stack,
                    K_THREAD_STACK_SIZEOF(net_stack),
                    (k_thread_entry_t)NET_THREAD_FN,
                    NULL, NULL, NULL,
                    NET_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&net_tid, "net");
    // k_thread_join(&net_tid, K_FOREVER);   /* block until IP obtained */
    ethernet_wait_ready(K_FOREVER);
    
    LOG_INF("Network ready — starting Azure MQTT");
    azure_mqtt_thread_start();

    /* Give MQTT a moment to connect before SNTP tries DNS */
    k_sleep(K_SECONDS(3));

    LOG_INF("Starting SNTP sync");
    // sntp_sync_start();
    http_time_sync_start();
    // http_time_sync_start();
    LOG_INF("Boot complete — BLE threads will start in ~15s");

    return 0;
}