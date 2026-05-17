/*
 * sntp_sync.c — SNTP time synchronisation for BLE+Network Gateway
 *
 * Periodically queries pool.ntp.org and calls time_sync_writer_set_utc()
 * directly. time_sync_writer then distributes UTC to all connected sensor
 * nodes via GATT WRITE inside process_data_thread().
 *
 * There is no UART bridge and no 0xCC frame on either platform —
 * the UTC reference is set in-process.
 *
 * Platform notes:
 *   ESP32-POE  (CONFIG_ETH_GATEWAY=y, no SPIRAM):
 *     Thread stack stays in internal DRAM (K_THREAD_STACK_DEFINE).
 *
 *   ESP32-WROVER (CONFIG_ESP_SPIRAM=y):
 *     Thread stack is placed in SPIRAM to relieve pressure on the 96 KB
 *     internal DRAM shared with BT + MQTT stacks.
 */

#include "sntp_sync.h"
#include "time_sync_writer.h"

#include <zephyr/kernel.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include <time.h>

LOG_MODULE_REGISTER(sntp_sync, LOG_LEVEL_INF);

// #define SNTP_SERVER              "pool.ntp.org"
#define SNTP_SERVER "216.239.35.0" 
#define SNTP_PORT                123
#define SNTP_TIMEOUT_MS          5000
#define SNTP_RESYNC_INTERVAL_S   60
#define SNTP_RETRY_DELAY_MS      10000

#define SNTP_STACK_SIZE  2048
#define SNTP_PRIORITY    6



/*
 * Stack placement: SPIRAM on WROVER, internal DRAM on POE.
 * Z_KERNEL_STACK_DEFINE_IN is a Zephyr internal macro; K_THREAD_STACK_DEFINE
 * is the public API and places the stack in internal DRAM by default.
 */
#if defined(CONFIG_ESP_SPIRAM)
Z_KERNEL_STACK_DEFINE_IN(sntp_stack, SNTP_STACK_SIZE,
    __attribute__((section(".ext_ram.bss"))));
#else
K_THREAD_STACK_DEFINE(sntp_stack, SNTP_STACK_SIZE);
#endif

static struct k_thread sntp_tid;

/* Last successful UTC — accessible via sntp_get_last_utc() */
static uint32_t last_utc_time   = 0;
static uint16_t last_utc_ms     = 0;
static uint32_t last_utc_set_ms = 0;

uint32_t sntp_get_last_utc(uint16_t *out_ms) {
    
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

static int do_sntp_query(uint32_t *utc_out, uint16_t *ms_out) {

    struct sntp_time sntp_time;
    struct sockaddr_in addr = {0};

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

    *utc_out = (uint32_t)(sntp_time.seconds);
    *ms_out  = (uint16_t)(((uint64_t)sntp_time.fraction * 1000ULL) >> 32);

    /* Store for sntp_get_last_utc() */
    last_utc_time   = *utc_out;
    last_utc_ms     = *ms_out;
    last_utc_set_ms = k_uptime_get_32();

    time_t t = (time_t)(*utc_out);
    struct tm *tm = gmtime(&t);
    LOG_INF("SNTP: UTC = %u (%04d-%02d-%02d %02d:%02d:%02d UTC)",
        *utc_out,
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);

    return 0;
}

static void sntp_thread(void) {

    LOG_INF("SNTP sync thread started");

        /* Wait for network stack to fully settle after DHCP */
    k_sleep(K_SECONDS(3));

    while (true) {
        uint32_t utc = 0;
        uint16_t ms  = 0;
        int ret = do_sntp_query(&utc, &ms);

        if (ret == 0) {
            /* Set UTC reference directly — no UART frame needed */
            time_sync_writer_set_utc(utc, ms);
        } else {
            LOG_WRN("SNTP failed (%d), retrying in %d ms",
                    ret, SNTP_RETRY_DELAY_MS);
            k_sleep(K_MSEC(SNTP_RETRY_DELAY_MS));
            continue;
        }

        k_sleep(K_SECONDS(SNTP_RESYNC_INTERVAL_S));
    }
}

void sntp_sync_start(void) {

    k_thread_create(&sntp_tid,
                    sntp_stack,
                    K_THREAD_STACK_SIZEOF(sntp_stack),
                    (k_thread_entry_t)sntp_thread,
                    NULL, NULL, NULL,
                    SNTP_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&sntp_tid, "sntp_sync");
}
