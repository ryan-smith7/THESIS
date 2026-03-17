// /*
//  * ethernet.c — Ethernet (LAN8720/RMII) connection management
//  *              for ESP32-POE running Zephyr.
//  *
//  * Replaces wifi.c.  The rest of the application (azure_mqtt, sntp_sync,
//  * bluetooth) is unchanged — they only need sockets to be available.
//  *
//  * How it works
//  * ────────────
//  *  1. ethernet_callbacks_init() registers NET_EVENT_IF_UP and
//  *     NET_EVENT_IPV4_DHCP_BOUND listeners.
//  *  2. ethernet_thread() triggers DHCP on the first available Ethernet
//  *     interface and blocks until a lease is obtained.
//  *  3. On success it logs the assigned address and returns, allowing
//  *     main() to proceed with Azure MQTT and SNTP.
//  *
//  * No credentials or scanning are needed — the driver and PHY
//  * (configured via DTS) handle link negotiation automatically.
//  */

// #include "ethernet.h"

// #include <zephyr/kernel.h>
// #include <zephyr/logging/log.h>
// #include <zephyr/net/net_if.h>
// #include <zephyr/net/net_event.h>
// #include <zephyr/net/net_mgmt.h>
// #include <zephyr/net/dhcpv4.h>
// #include <zephyr/net/net_ip.h>

// LOG_MODULE_REGISTER(ethernet, LOG_LEVEL_INF);

// /* Semaphore posted once DHCP hands us an address */
// static K_SEM_DEFINE(eth_dhcp_sem, 0, 1);

// /* Cached IP for logging after the semaphore is posted */
// static char ip_addr_str[NET_IPV4_ADDR_LEN];

// /* ── Net-mgmt event mask ────────────────────────────────── */
// #define ETH_EVENTS  (NET_EVENT_IPV4_DHCP_BOUND | NET_EVENT_IF_UP)

// static struct net_mgmt_event_callback eth_cb;

// static void eth_event_handler(struct net_mgmt_event_callback *cb,
//                               uint32_t event,
//                               struct net_if *iface)
// {
//     switch (event) {

//     case NET_EVENT_IF_UP:
//         LOG_INF("Ethernet link UP — waiting for DHCP lease...");
//         break;

//     case NET_EVENT_IPV4_DHCP_BOUND: {
//         /*
//          * Pull the assigned address out of the interface's unicast
//          * address table (the first IPv4 address is always index 0).
//          */
//         struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
//         if (ipv4) {
//             net_addr_ntop(AF_INET,
//                           &ipv4->unicast[0].ipv4.address.in_addr,
//                           ip_addr_str, sizeof(ip_addr_str));
//             LOG_INF("DHCP bound: %s", ip_addr_str);
//         }
//         k_sem_give(&eth_dhcp_sem);
//         break;
//     }

//     default:
//         break;
//     }
// }

// /* ── Public API ─────────────────────────────────────────── */

// void ethernet_callbacks_init(void)
// {
//     net_mgmt_init_event_callback(&eth_cb, eth_event_handler, ETH_EVENTS);
//     net_mgmt_add_event_callback(&eth_cb);
// }

// void ethernet_thread(void)
// {
//     LOG_INF("ethernet_thread: starting");

//     ethernet_callbacks_init();

//     /*
//      * Find the first Ethernet-type network interface.
//      * On the ESP32-POE there will be exactly one (the EMAC/LAN8720).
//      */
//     struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
//     if (!iface) {
//         LOG_ERR("No Ethernet interface found — check DTS / Kconfig");
//         /* Spin here; there is nothing useful to do without a link. */
//         while (true) {
//             k_sleep(K_SECONDS(5));
//         }
//     }

//     LOG_INF("Found Ethernet interface: %s", net_if_get_device(iface)->name);

//     /*
//      * Bring the interface up if it isn't already.
//      * net_if_up() is idempotent — safe to call even if already up.
//      */
//     int ret = net_if_up(iface);
//     if (ret < 0 && ret != -EALREADY) {
//         LOG_ERR("net_if_up failed: %d", ret);
//     }
//     /* Give switch time to bring up the port after link detection */
//     k_sleep(K_SECONDS(2));
//     net_dhcpv4_start(iface);
//     /* Start DHCP */
//     net_dhcpv4_start(iface);
//     LOG_INF("DHCP started — waiting for lease...");

//     /*
//      * Block until DHCP_BOUND event.
//      * ETH_DHCP_RETRY_COUNT == 0 → wait forever (recommended for gateway).
//      */
// #if ETH_DHCP_RETRY_COUNT == 0
//     k_sem_take(&eth_dhcp_sem, K_FOREVER);
// #else
//     int attempts = 0;
//     while (attempts < ETH_DHCP_RETRY_COUNT) {
//         if (k_sem_take(&eth_dhcp_sem, K_SECONDS(30)) == 0) {
//             break;
//         }
//         attempts++;
//         LOG_WRN("DHCP timeout (attempt %d/%d) — retrying",
//                 attempts, ETH_DHCP_RETRY_COUNT);
//         net_dhcpv4_stop(iface);
//         net_dhcpv4_start(iface);
//     }
//     if (attempts >= ETH_DHCP_RETRY_COUNT) {
//         LOG_ERR("DHCP failed after %d attempts — rebooting",
//                 ETH_DHCP_RETRY_COUNT);
//         /* In production you may prefer sys_reboot(SYS_REBOOT_COLD) */
//         while (true) { k_sleep(K_SECONDS(5)); }
//     }
// #endif

//     LOG_INF("Ethernet ready — IP: %s", ip_addr_str);
//     /* Thread exits; main() will k_thread_join() and continue. */
// }


/*
 * ethernet.c — Ethernet (LAN8720/RMII) connection management
 *              for ESP32-POE running Zephyr.
 */

#include "ethernet.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>

LOG_MODULE_REGISTER(ethernet, LOG_LEVEL_INF);

/* Semaphore posted once we have an IPv4 address */
static K_SEM_DEFINE(eth_dhcp_sem, 0, 1);

/* Cached IP for logging */
static char ip_addr_str[NET_IPV4_ADDR_LEN];

/* ── Net-mgmt event mask ────────────────────────────────── */
/* Use NET_EVENT_IPV4_ADDR_ADD — fired reliably when any IPv4
 * address is assigned, whether by DHCP or static config.
 * NET_EVENT_IPV4_DHCP_BOUND can be missed if the callback is
 * registered after the DHCP state machine has already advanced. */
#define ETH_EVENTS  (NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IF_UP)

static struct net_mgmt_event_callback eth_cb;

static void eth_event_handler(struct net_mgmt_event_callback *cb,
                              uint32_t event,
                              struct net_if *iface)
{
    switch (event) {

    case NET_EVENT_IF_UP:
        LOG_INF("Ethernet link UP — waiting for DHCP lease...");
        break;

    case NET_EVENT_IPV4_ADDR_ADD: {
        /* An IPv4 address was added to the interface — DHCP succeeded */
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
        if (ipv4) {
            net_addr_ntop(AF_INET,
                          &ipv4->unicast[0].ipv4.address.in_addr,
                          ip_addr_str, sizeof(ip_addr_str));
            LOG_INF("IPv4 address assigned: %s", ip_addr_str);
        }
        k_sem_give(&eth_dhcp_sem);
        break;
    }

    default:
        break;
    }
}

/* ── Public API ─────────────────────────────────────────── */

// void ethernet_thread(void)
// {
//     LOG_INF("ethernet_thread: starting");

//     /* Register callbacks BEFORE starting DHCP so no event is missed */
//     net_mgmt_init_event_callback(&eth_cb, eth_event_handler, ETH_EVENTS);
//     net_mgmt_add_event_callback(&eth_cb);

//     struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
//     if (!iface) {
//         LOG_ERR("No Ethernet interface found — check DTS / Kconfig");
//         while (true) { k_sleep(K_SECONDS(5)); }
//     }

//     LOG_INF("Found Ethernet interface: %s", net_if_get_device(iface)->name);

//     int ret = net_if_up(iface);
//     if (ret < 0 && ret != -EALREADY) {
//         LOG_ERR("net_if_up failed: %d", ret);
//     }

//     /* Small delay to ensure PHY link is stable before DHCP */
//     k_sleep(K_SECONDS(2));

//     net_dhcpv4_start(iface);
//     LOG_INF("DHCP started — waiting for lease...");

//     /* Block until IPv4 address assigned */
//     k_sem_take(&eth_dhcp_sem, K_FOREVER);

//     LOG_INF("Ethernet ready — IP: %s", ip_addr_str);
// }

// void ethernet_thread(void)
// {
//     LOG_INF("ethernet_thread: starting");

//     net_mgmt_init_event_callback(&eth_cb, eth_event_handler, ETH_EVENTS);
//     net_mgmt_add_event_callback(&eth_cb);

//     struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
//     if (!iface) {
//         LOG_ERR("No Ethernet interface found");
//         while (true) { k_sleep(K_SECONDS(5)); }
//     }

//     net_if_up(iface);
//     k_sleep(K_SECONDS(2));
//     net_dhcpv4_start(iface);
//     LOG_INF("DHCP started — polling for lease...");

//     /* Diagnostic + poll loop */
//     while (true) {
//         struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
//         if (ipv4 && ipv4->unicast[0].ipv4.address.in_addr.s_addr != 0) {
//             net_addr_ntop(AF_INET,
//                           &ipv4->unicast[0].ipv4.address.in_addr,
//                           ip_addr_str, sizeof(ip_addr_str));
//             LOG_INF("Ethernet ready — IP: %s", ip_addr_str);
//             return;
//         }
//         LOG_INF("DHCP state: %d  IP: none",
//                 iface->config.dhcpv4.state);
//         k_sleep(K_SECONDS(3));
//     }
// }

void ethernet_thread(void)
{
    LOG_INF("ethernet_thread: starting");

    net_mgmt_init_event_callback(&eth_cb, eth_event_handler, ETH_EVENTS);
    net_mgmt_add_event_callback(&eth_cb);

    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
    if (!iface) {
        LOG_ERR("No Ethernet interface found");
        while (true) { k_sleep(K_SECONDS(5)); }
    }

    net_if_up(iface);
    k_sleep(K_SECONDS(2));
    net_dhcpv4_start(iface);
    LOG_INF("DHCP started — polling for lease...");

    while (true) {
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
        if (ipv4 && ipv4->unicast[0].ipv4.address.in_addr.s_addr != 0) {
            char gw_str[NET_IPV4_ADDR_LEN];
            char nm_str[NET_IPV4_ADDR_LEN];
            net_addr_ntop(AF_INET,
                          &ipv4->unicast[0].ipv4.address.in_addr,
                          ip_addr_str, sizeof(ip_addr_str));
            net_addr_ntop(AF_INET, &ipv4->gw,
                          gw_str, sizeof(gw_str));
            net_addr_ntop(AF_INET,
                          &ipv4->unicast[0].netmask,
                          nm_str, sizeof(nm_str));
            LOG_INF("Ethernet ready — IP: %s  GW: %s  NM: %s",
                    ip_addr_str, gw_str, nm_str);
            return;
        }
        LOG_INF("DHCP state: %d  IP: none",
                iface->config.dhcpv4.state);
        k_sleep(K_SECONDS(3));
    }
}