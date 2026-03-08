/*
 * main.c — Combined BLE Central + WiFi + Azure IoT Hub Gateway
 *
 * Boot sequence:
 *   1. WiFi thread connects and obtains IP (blocks up to 30s)
 *   2. Azure MQTT thread starts — connects to IoT Hub
 *   3. BLE threads start — scan, connect, receive, publish
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wifi.h"
#include "azure_mqtt.h"
// #include "bluetooth.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define WIFI_THREAD_STACK  4096
#define WIFI_THREAD_PRIO   6

K_THREAD_STACK_DEFINE(wifi_stack, WIFI_THREAD_STACK);
static struct k_thread wifi_tid;

// /* BLE threads — defined here so WiFi/Azure are up first */
// K_THREAD_DEFINE(process_data_tid,
//                 BASE_PROCESS_STACK_SIZE,
//                 process_data_thread,
//                 NULL, NULL, NULL,
//                 BASE_PROCESS_PRIORITY, 0,
//                 2000);   /* 2s delay — wait for Azure to connect */

// K_THREAD_DEFINE(base_tid,
//                 BASE_CONTROL_STACK_SIZE,
//                 base_thread,
//                 NULL, NULL, NULL,
//                 BASE_CONTROL_PRIORITY, 0,
//                 2000);   /* 2s delay — wait for Azure to connect */

int main(void)
{
    LOG_INF("=== BLE→Azure Gateway starting ===");

    /* Start WiFi — blocks until IP obtained or 30s timeout */
    k_thread_create(&wifi_tid,
                    wifi_stack,
                    K_THREAD_STACK_SIZEOF(wifi_stack),
                    (k_thread_entry_t)wifi_thread,
                    NULL, NULL, NULL,
                    WIFI_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&wifi_tid, "wifi");
    k_thread_join(&wifi_tid, K_SECONDS(30));

    LOG_INF("WiFi ready — starting Azure MQTT");
    azure_mqtt_thread_start();

    /* BLE threads start automatically after their 2s delay (K_THREAD_DEFINE above) */

    return 0;
}