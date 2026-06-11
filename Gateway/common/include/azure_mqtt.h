#ifndef MQTT_H_
#define MQTT_H_

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>

/* Oracle Cloud MQTT bridge — plaintext, no TLS required on device.*/
#define AZURE_IOT_HUB_HOSTNAME   "161.33.232.177"

/* Device ID registered in my IoT Hub though now directs to MQTT oracle first*/
#define AZURE_DEVICE_ID          "esp32-device-01"

/* ----------------MQTT connection settings-------------------------------*/
/* Plaintext port for Oracle bridge (changed to 443) */
#define AZURE_MQTT_PORT      443
#define AZURE_MQTT_CLIENT_ID AZURE_DEVICE_ID

/* Username/password not required for anonymous Oracle bridge connection */
#define AZURE_MQTT_TOPIC  "devices/" AZURE_DEVICE_ID "/messages/events/$.ct=application%2Fjson&$.ce=utf-8"

/**
 * @brief Returns true if the MQTT client is currently connected.
 */
bool azure_mqtt_is_connected(void);

/**
 * @brief Public wrapper around do_connect().
 */
int  azure_mqtt_connect(void);

/**
 * @brief Enqueue a JSON string into the ring buffer for publishing.
 *
 * Safe to call from any thread. Drops the message and returns -ENOMEM
 * if the ring buffer is full, or -ENOTCONN if not connected.
 */
int  azure_mqtt_publish(const char *json);

/**
 * @brief Main MQTT thread — connects, drains the ring buffer, and keeps alive.
 *
 * Initialises the ring buffer and mutex, then loops: waits for network
 * readiness, reconnects as needed, publishes queued messages, and polls
 * for incoming packets every 20 ms.
 */
void azure_mqtt_thread(void);

/**
 * @brief Spawn the azure_mqtt_thread as a named Zephyr thread.
 */
void azure_mqtt_thread_start(void);

#endif /* MQTT_H_ */