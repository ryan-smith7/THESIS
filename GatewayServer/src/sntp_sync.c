/*
 * sntp_sync.c — WiFi Board (ESP32 #2)
 *
 * Periodically queries an NTP server and forwards UTC to the BLE gateway
 * board via UART using the 0xCC frame format expected by uart.c on the
 * gateway.
 *
 * Frame sent on each successful sync:
 *   [ 0xCC ][ utc_b3 ][ utc_b2 ][ utc_b1 ][ utc_b0 ]  (5 bytes, big-endian)
 */

#include "sntp_sync.h"
#include "uart_bridge.h"

#include <zephyr/kernel.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include <time.h>


LOG_MODULE_REGISTER(sntp_sync, LOG_LEVEL_INF);

#define SNTP_SERVER          "pool.ntp.org"
#define SNTP_PORT            123
#define SNTP_TIMEOUT_MS      5000
#define SNTP_RESYNC_INTERVAL_S  60
#define SNTP_RETRY_DELAY_MS     10000

#define SNTP_STACK_SIZE  2048
#define SNTP_PRIORITY    6        /* same as wifi_thread — runs after it exits */

K_THREAD_STACK_DEFINE(sntp_stack, SNTP_STACK_SIZE);
static struct k_thread sntp_tid;

static uint32_t last_utc_time    = 0;
static uint16_t last_utc_ms      = 0;   /* sub-second ms from NTP fraction */
static uint32_t last_utc_set_ms  = 0;  /* k_uptime when last_utc was stored */

uint32_t sntp_get_last_utc(uint16_t *out_ms)
{
    if (last_utc_time == 0) {
        if (out_ms) *out_ms = 0;
        return 0;
    }
    uint32_t elapsed_ms = k_uptime_get_32() - last_utc_set_ms;
    uint32_t total_ms   = last_utc_ms + elapsed_ms;
    uint32_t sec        = last_utc_time + (total_ms / 1000U);
    uint16_t ms         = (uint16_t)(total_ms % 1000U);
    if (out_ms) *out_ms = ms;
    return sec;
}

static int do_sntp_query(uint32_t *utc_out)
{
    struct sntp_time sntp_time;
    struct sockaddr_in addr = {0};

    /* Resolve NTP server */
    struct addrinfo *result;
    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };

    int ret = getaddrinfo(SNTP_SERVER, NULL, &hints, &result);
    if (ret != 0) {
        LOG_ERR("DNS failed for %s: %d", SNTP_SERVER, ret);
        return -ENETUNREACH;
    }

    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SNTP_PORT);
    addr.sin_addr   = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
    freeaddrinfo(result);

    ret = sntp_simple_addr((struct sockaddr *)&addr, sizeof(addr),
                           SNTP_TIMEOUT_MS, &sntp_time);
    if (ret < 0) {
        LOG_ERR("SNTP query failed: %d", ret);
        return ret;
    }

    /* sntp_time.seconds is seconds since 1900; convert to Unix (since 1970) */
    /* NTP epoch offset = 70 years = 2208988800 seconds */
    // *utc_out = (uint32_t)(sntp_time.seconds - 2208988800ULL);
    *utc_out = (uint32_t)(sntp_time.seconds);

    /* Extract milliseconds from NTP fraction field (2^32 per second) */
    uint16_t utc_ms = (uint16_t)(((uint64_t)sntp_time.fraction * 1000ULL) >> 32);

    time_t t = (time_t)(*utc_out);
    struct tm *tm = gmtime(&t);
    LOG_INF("SNTP: UTC = %u", *utc_out);

    LOG_INF("SNTP: UTC = %u (%04d-%02d-%02d %02d:%02d:%02d UTC)",
        *utc_out,
        tm->tm_year + 1900,
        tm->tm_mon + 1,
        tm->tm_mday,
        tm->tm_hour,
        tm->tm_min,
        tm->tm_sec);

    /* Store for sntp_get_last_utc() */
    last_utc_time   = *utc_out;
    last_utc_ms     = utc_ms;
    last_utc_set_ms = k_uptime_get_32();

    return 0;
}

static void sntp_thread(void)
{
    LOG_INF("SNTP sync thread started");

    while (true) {
        uint32_t utc = 0;
        int ret = do_sntp_query(&utc);

        if (ret == 0) {
            /* Forward UTC + ms to BLE gateway via UART 0xCC frame */
            uart_bridge_send_utc(utc, last_utc_ms);
        } else {
            LOG_WRN("SNTP failed (%d), retrying in %d ms",
                    ret, SNTP_RETRY_DELAY_MS);
            k_sleep(K_MSEC(SNTP_RETRY_DELAY_MS));
            continue;
        }

        /* Re-sync every 60 seconds */
        k_sleep(K_SECONDS(SNTP_RESYNC_INTERVAL_S));
    }
}

void sntp_sync_start(void)
{
    k_thread_create(&sntp_tid,
                    sntp_stack,
                    K_THREAD_STACK_SIZEOF(sntp_stack),
                    (k_thread_entry_t)sntp_thread,
                    NULL, NULL, NULL,
                    SNTP_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&sntp_tid, "sntp_sync");
}