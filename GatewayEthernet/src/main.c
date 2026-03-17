/*
 * main.c — Combined BLE Central + Ethernet + Azure IoT Hub Gateway
 *          Target: esp32_poe/esp32/procpu
 *
 * Boot sequence:
 *   1. Ethernet thread — brings up LAN8720, obtains DHCP lease (blocks)
 *   2. Azure MQTT thread — connects to IoT Hub, keepalive loop
 *   3. SNTP sync — sets UTC reference for time_sync_writer
 *   4. BLE threads — scan, connect to sensor nodes, decode + publish
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ethernet.h"        /* replaces wifi.h */
#include "azure_mqtt.h"
#include "bluetooth.h"
#include "sntp_sync.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define ETH_THREAD_STACK  2048   // was 4096 — thread just does DHCP then exits
#define ETH_THREAD_PRIO   4

K_THREAD_STACK_DEFINE(eth_stack, ETH_THREAD_STACK);
static struct k_thread eth_tid;

/*
 * BLE threads — uncomment and adjust delay once Ethernet + Azure are stable.
 * Delay is 15s: Ethernet/DHCP ~3s + MQTT connect ~5s + margin.
 */
K_THREAD_DEFINE(process_data_tid,
                BASE_PROCESS_STACK_SIZE,
                process_data_thread,
                NULL, NULL, NULL,
                BASE_PROCESS_PRIORITY, 0,
                15000);   // was 15000

K_THREAD_DEFINE(base_tid,
                BASE_CONTROL_STACK_SIZE,
                base_thread,
                NULL, NULL, NULL,
                BASE_CONTROL_PRIORITY, 0,
                15000);   // was 15000

int main(void)
{
    LOG_INF("=== Combined BLE+Ethernet Gateway starting (ESP32-POE) ===");

    /*
     * Start the Ethernet thread and wait for it to return.
     * It only returns after a DHCP lease is obtained.
     */
    k_thread_create(&eth_tid,
                    eth_stack,
                    K_THREAD_STACK_SIZEOF(eth_stack),
                    (k_thread_entry_t)ethernet_thread,
                    NULL, NULL, NULL,
                    ETH_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&eth_tid, "ethernet");
    k_thread_join(&eth_tid, K_FOREVER);   /* block until IP obtained */

    LOG_INF("Ethernet ready — starting Azure MQTT");
    azure_mqtt_thread_start();

    /* Give MQTT a moment to complete TLS handshake before SNTP DNS */
    k_sleep(K_SECONDS(3));

    LOG_INF("Starting SNTP sync");
    sntp_sync_start();

    LOG_INF("Boot complete — BLE threads will start in ~15s");

    return 0;
}