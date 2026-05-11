/*
 * ethernet.c — Ethernet (LAN8720/RMII) connection management
 *              for ESP32-POE running Zephyr.
 *
 * Acquisition logic is unchanged from the known-good original:
 * configure the interface, start DHCP, poll iface->config.ip.ipv4
 * until an address appears.
 *
 * Recovery: a watchdog detects link loss via TWO sources, because
 * the ESP32 Ethernet driver does not always propagate PHY link
 * changes up to the net_mgmt layer as NET_EVENT_IF_DOWN:
 *   1. net-mgmt events (IF_DOWN, IPV4_ADDR_DEL) — fast path
 *   2. periodic carrier poll — backup path for silent PHY drops
 *
 * Either source kicks the watchdog, which waits for the link to
 * return and re-runs the acquisition.
 *
 * Public coordination: ethernet_wait_ready() blocks until an IPv4
 * address is held. Other threads (e.g. azure_thread) should call
 * this before attempting to open sockets.
 */

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

/* ── Coordination primitives ────────────────────────────── */

/* Signalled from the event handler when the link drops or the
 * IPv4 address is removed. The watchdog loop blocks on this. */
static K_SEM_DEFINE(eth_lost_sem, 0, 1);

/* Public readiness flag. Signalled when we hold an IPv4 address.
 * Other threads should wait on this before opening sockets.
 *
 * We use a semaphore (rather than k_event) so we don't depend on
 * CONFIG_EVENTS. A boolean flag tracks "currently ready" since a
 * semaphore alone can't represent persistent state — a waiter
 * would consume the give and the next waiter would block. */
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
                              struct net_if *iface)
{
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

/* ── Carrier poll backup ────────────────────────────────────
 *
 * The ESP32 Ethernet driver may not raise NET_EVENT_IF_DOWN when
 * the PHY drops carrier briefly. To catch that case we run a
 * low-priority poll thread that watches the iface's operational
 * state and posts to eth_lost_sem if it observes a transition
 * away from "running".
 *
 * net_if_oper_state() returns NET_IF_OPER_UP when both admin-up
 * and carrier are good; anything else is treated as a loss.
 */
static struct net_if *carrier_poll_iface;

static void carrier_poll_thread(void *a, void *b, void *c)
{
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

/* ── Acquisition routine ────────────────────────────────────
 *
 * Original known-good DHCP acquisition: start DHCP, poll the
 * interface until an address is held. */
static void dhcp_acquire(struct net_if *iface)
{
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
             * ethernet_wait_ready(). Set the flag first so a waker
             * that races between k_sem_give and k_sem_take observes
             * the persistent state correctly. */
            eth_ready_flag = true;
            k_sem_give(&eth_ready_sem);
            return;
        }
        LOG_INF("DHCP state: %d  IP: none",
                iface->config.dhcpv4.state);
        k_sleep(K_SECONDS(3));
    }
}

/* ── Public API ─────────────────────────────────────────── */

/* Block until Ethernet has an IPv4 address.
 * timeout: K_FOREVER, K_NO_WAIT, or a duration.
 * Returns true if ready, false on timeout.
 *
 * Implementation: if the flag is already set (we are currently
 * ready), return immediately. Otherwise wait on the semaphore;
 * when given, re-arm it so subsequent waiters (or this thread
 * waiting again after a loss/recover cycle) see the same state. */
bool ethernet_wait_ready(k_timeout_t timeout)
{
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

bool ethernet_is_ready(void)
{
    return eth_ready_flag;
}

void ethernet_thread(void)
{
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

    /* First acquisition — identical to the known-good original. */
    dhcp_acquire(iface);

    /* ── Watchdog loop ────────────────────────────────────
     * Block until either:
     *   - net-mgmt fires IF_DOWN / IPV4_ADDR_DEL  (fast path)
     *   - carrier poll observes oper_state != UP (backup)
     * Then wait for the link to return and re-acquire. */
    while (true) {
        k_sem_reset(&eth_lost_sem);
        k_sem_take(&eth_lost_sem, K_FOREVER);

        LOG_WRN("Connectivity lost — waiting for link to return...");
        eth_ready_flag = false;
        /* Wait for carrier to come back. Use oper_state because
         * net_if_is_up() can stay true through a PHY bounce on
         * the ESP32 driver. */
        while (net_if_oper_state(iface) != NET_IF_OPER_UP) {
            k_sleep(K_SECONDS(1));
        }

        /* Settle delay so the router has time to come fully online
         * if this was a reboot rather than a cable bounce. */
        k_sleep(K_SECONDS(2));

        LOG_INF("Link returned — re-acquiring DHCP");
        dhcp_acquire(iface);
    }
}