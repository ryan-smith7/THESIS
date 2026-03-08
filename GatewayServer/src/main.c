/*
 * main.c — WiFi Board (ESP32 #2)
 *
 * Boot sequence:
 *   1. WiFi thread — connects and obtains IP
 *   2. Azure MQTT thread — connects to IoT Hub, runs keepalive loop
 *   3. UART bridge — receives sensor JSON frames from BLE gateway, publishes to Azure
 *   4. SNTP sync — queries NTP, forwards UTC to BLE gateway via UART every 60s
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "wifi.h"
#include "azure_mqtt.h"
#include "uart_bridge.h"
#include "sntp_sync.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define WIFI_THREAD_STACK  4096
#define WIFI_THREAD_PRIO   6

K_THREAD_STACK_DEFINE(wifi_stack, WIFI_THREAD_STACK);
static struct k_thread wifi_tid;

int main(void)
{
    LOG_INF("=== WiFi+Azure Gateway starting ===");

    /* 1. Start WiFi — blocks until IP obtained */
    k_thread_create(&wifi_tid,
                    wifi_stack,
                    K_THREAD_STACK_SIZEOF(wifi_stack),
                    (k_thread_entry_t)wifi_thread,
                    NULL, NULL, NULL,
                    WIFI_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&wifi_tid, "wifi");
    k_thread_join(&wifi_tid, K_SECONDS(30));

    /* 2. Start Azure MQTT thread */
    LOG_INF("Starting Azure MQTT thread");
    azure_mqtt_thread_start();

    /* 3. Give MQTT a moment to connect */
    k_sleep(K_SECONDS(5));

    /* 4. Start UART bridge (RX: sensor frames from gateway → Azure publish) */
    LOG_INF("Starting UART bridge");
    uart_bridge_start();

    /* 5. Start SNTP sync (TX: UTC frames → gateway → sensor nodes) */
    LOG_INF("Starting SNTP sync");
    sntp_sync_start();

    return 0;
}