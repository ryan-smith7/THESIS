#ifndef ETHERNET_H_
#define ETHERNET_H_

#include <stdbool.h>
#include <zephyr/kernel.h>

#define ETH_DHCP_RETRY_COUNT  0

/**
 * @brief Main Ethernet management thread.
 *
 * Registers event callbacks, brings the interface up, performs the
 * initial DHCP acquisition, then loops as a watchdog: on connectivity
 * loss, waits for the link to return and re-acquires DHCP.
 */
void ethernet_thread(void);

/**
 * @brief Block until Ethernet has an IPv4 address or the timeout expires.
 *
 * @param timeout Maximum time to wait.
 * @return true if Ethernet is ready, false on timeout.
 */
bool ethernet_wait_ready(k_timeout_t timeout);

/**
 * @brief Gets Ethernet readiness flag
 *
 * @return true if the interface currently holds an IPv4 address.
 */
bool ethernet_is_ready(void);

#endif /* ETHERNET_H_ */