#include "ethernet.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>

LOG_MODULE_REGISTER(ethernet, LOG_LEVEL_INF);

/* Signalled from the event handler when the link drops or the
 * IPv4 address is removed.*/
static K_SEM_DEFINE(eth_lost_sem, 0, 1);

static K_SEM_DEFINE(eth_ready_sem, 0, 1);
static volatile bool eth_ready_flag;

/* Cached IP for logging */
static char ip_addr_str[NET_IPV4_ADDR_LEN];

/* ── Net-mgmt event mask ────────────────────────────────── */
#define ETH_EVENTS  (NET_EVENT_IF_UP            | \
                     NET_EVENT_IF_DOWN          | \
                     NET_EVENT_IPV4_ADDR_ADD    | \
                     NET_EVENT_IPV4_ADDR_DEL)

static struct net_mgmt_event_callback eth_cb;

static void eth_event_handler(struct net_mgmt_event_callback *cb,
                              uint64_t event,
                              struct net_if *iface) {
    switch (event) {

    case NET_EVENT_IF_UP:
        LOG_INF("Ethernet event: IF_UP");
        break;

    case NET_EVENT_IF_DOWN:
        LOG_WRN("Ethernet event: IF_DOWN");
        eth_ready_flag = false;
        k_sem_reset(&eth_ready_sem);
        k_sem_give(&eth_lost_sem);
        break;

    case NET_EVENT_IPV4_ADDR_ADD: {
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
        if (ipv4) {
            net_addr_ntop(AF_INET,
                          &ipv4->unicast[0].ipv4.address.in_addr,
                          ip_addr_str, sizeof(ip_addr_str));
            LOG_INF("Ethernet event: IPV4_ADDR_ADD %s", ip_addr_str);
        }
        break;
    }

    case NET_EVENT_IPV4_ADDR_DEL:
        LOG_WRN("Ethernet event: IPV4_ADDR_DEL");
        eth_ready_flag = false;
        k_sem_reset(&eth_ready_sem);
        k_sem_give(&eth_lost_sem);
        break;

    default:
        break;
    }
}

static struct net_if *carrier_poll_iface;

static void carrier_poll_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    bool was_up = true;

    while (true) {
        k_sleep(K_SECONDS(2));

        if (!carrier_poll_iface) {
            continue;
        }

        bool is_up = net_if_is_up(carrier_poll_iface) &&
                     net_if_oper_state(carrier_poll_iface) == NET_IF_OPER_UP;

        if (was_up && !is_up) {
            LOG_WRN("Carrier poll: link lost (oper_state changed)");
            eth_ready_flag = false;
            k_sem_reset(&eth_ready_sem);
            k_sem_give(&eth_lost_sem);
        }

        was_up = is_up;
    }
}

K_THREAD_DEFINE(carrier_poll_tid, 1024, carrier_poll_thread,
                NULL, NULL, NULL, 10, 0, 0);

/* ── Acquisition routine ──*/

static void dhcp_acquire(struct net_if *iface) {
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

            /* Mark ready and wake any threads waiting in
             * ethernet_wait_ready()*/
            eth_ready_flag = true;
            k_sem_give(&eth_ready_sem);
            return;
        }
        LOG_INF("DHCP state: %d  IP: none",
                iface->config.dhcpv4.state);
        k_sleep(K_SECONDS(3));
    }
}


/* Block until Ethernet has an IPv4 address.*/
bool ethernet_wait_ready(k_timeout_t timeout) {
    if (eth_ready_flag) {
        return true;
    }

    int ret = k_sem_take(&eth_ready_sem, timeout);
    if (ret != 0) {
        return false;
    }

    /* Re-arm so the next waiter also unblocks. The flag is the
     * authoritative state; the semaphore is just a wakeup. */
    k_sem_give(&eth_ready_sem);
    return eth_ready_flag;
}

bool ethernet_is_ready(void) {
    return eth_ready_flag;
}

void ethernet_thread(void) {

    LOG_INF("ethernet_thread: starting (watchdog + carrier poll)");

    net_mgmt_init_event_callback(&eth_cb, eth_event_handler, ETH_EVENTS);
    net_mgmt_add_event_callback(&eth_cb);

    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
    if (!iface) {
        LOG_ERR("No Ethernet interface found");
        while (true) { k_sleep(K_SECONDS(5)); }
    }

    net_if_up(iface);
    k_sleep(K_SECONDS(2));

    /* Hand the iface to the carrier-poll thread. */
    carrier_poll_iface = iface;

    /* First acquisition*/
    dhcp_acquire(iface);

    /* ── Watchdog loop ────────────────────────────────────*/
    while (true) {
        k_sem_reset(&eth_lost_sem);
        k_sem_take(&eth_lost_sem, K_FOREVER);

        LOG_WRN("Connectivity lost — waiting for link to return...");
        eth_ready_flag = false;

        while (net_if_oper_state(iface) != NET_IF_OPER_UP) {
            k_sleep(K_SECONDS(1));
        }

        /* Settle delay */
        k_sleep(K_SECONDS(2));

        LOG_INF("Link returned — re-acquiring DHCP");
        dhcp_acquire(iface);
    }
}