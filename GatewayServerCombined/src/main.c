/*
 * main.c — Combined BLE Central + WiFi + Azure IoT Hub Gateway (WROVER)
 *
 * Boot sequence:
 *   1. WiFi thread — connects and obtains IP (retries forever, no false-start)
 *   2. Azure MQTT thread — connects to IoT Hub, keepalive loop
 *   3. SNTP sync — sets UTC reference for time_sync_writer
 *   4. BLE threads — scan, connect to sensor nodes, decode + publish direct
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wifi.h"
#include "azure_mqtt.h"
#include "bluetooth.h"
#include "sntp_sync.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define WIFI_THREAD_STACK  4096
#define WIFI_THREAD_PRIO   4

K_THREAD_STACK_DEFINE(wifi_stack, WIFI_THREAD_STACK);
static struct k_thread wifi_tid;

/*
 * BLE threads — use K_THREAD_DEFINE with a delay so WiFi + Azure
 * are up before BLE starts trying to publish.
 * Delay is 15s: WiFi ~5s + MQTT connect ~5s + margin.
 */
K_THREAD_DEFINE(process_data_tid,
                BASE_PROCESS_STACK_SIZE,
                process_data_thread,
                NULL, NULL, NULL,
                BASE_PROCESS_PRIORITY, 0,
                15000);

K_THREAD_DEFINE(base_tid,
                BASE_CONTROL_STACK_SIZE,
                base_thread,
                NULL, NULL, NULL,
                BASE_CONTROL_PRIORITY, 0,
                15000);

int main(void)
{
    LOG_INF("=== Combined BLE+WiFi Gateway starting (WROVER) ===");

    /*
     * Start WiFi thread and wait for it to exit (it exits only after
     * obtaining an IP address). No timeout — if WiFi never connects,
     * there is nothing useful to do anyway.
     */
    k_thread_create(&wifi_tid,
                    wifi_stack,
                    K_THREAD_STACK_SIZEOF(wifi_stack),
                    (k_thread_entry_t)wifi_thread,
                    NULL, NULL, NULL,
                    WIFI_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&wifi_tid, "wifi");
    k_thread_join(&wifi_tid, K_FOREVER);   /* block until IP obtained */

    LOG_INF("WiFi ready — starting Azure MQTT");
    azure_mqtt_thread_start();

    /* Give MQTT a moment to connect before SNTP tries DNS */
    k_sleep(K_SECONDS(3));

    LOG_INF("Starting SNTP sync");
    sntp_sync_start();

    /* BLE threads start automatically via K_THREAD_DEFINE delay above */
    LOG_INF("Boot complete — BLE threads will start in ~15s");

    return 0;
}