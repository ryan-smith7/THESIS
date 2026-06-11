#ifndef ETHERNET_H_
#define ETHERNET_H_

#include <stdbool.h>
#include <zephyr/kernel.h>

#define ETH_DHCP_RETRY_COUNT  0

/**
 * @brief Initialise Ethernet management callbacks.
 */
void ethernet_callbacks_init(void);

/**
 * @brief Blocking thread: brings up the Ethernet interface and obtains an
 * IP address via DHCP.  Returns only after a lease is acquired.
 */
void ethernet_thread(void);

bool ethernet_wait_ready(k_timeout_t timeout);

bool ethernet_is_ready(void);

#endif /* ETHERNET_H_ */