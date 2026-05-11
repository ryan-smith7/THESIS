#ifndef ETHERNET_H_
#define ETHERNET_H_

#include <stdbool.h>
#include <zephyr/kernel.h>
/**
 * @file ethernet.h
 * @brief Ethernet connection management for ESP32-POE (LAN8720 via RMII).
 *
 * Drop-in replacement for wifi.h / wifi.c.
 * The public API is intentionally identical so that main.c needs only
 * a header-include change.
 */

/* How many times to retry DHCP before giving up and rebooting.
 * Set to 0 for infinite retries (recommended for unattended gateway). */
#define ETH_DHCP_RETRY_COUNT  0

/**
 * @brief Initialise Ethernet management callbacks.
 * Must be called before ethernet_thread().
 */
void ethernet_callbacks_init(void);

/**
 * @brief Blocking thread: brings up the Ethernet interface and obtains an
 * IP address via DHCP.  Returns only after a lease is acquired.
 *
 * Spawn this with k_thread_create() exactly as wifi_thread() was used,
 * then k_thread_join() on it before starting Azure MQTT.
 */
void ethernet_thread(void);


bool ethernet_wait_ready(k_timeout_t timeout);

bool ethernet_is_ready(void);

#endif /* ETHERNET_H_ */