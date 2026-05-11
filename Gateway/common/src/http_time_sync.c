/*
 * http_time_sync.c — HTTP-based UTC time sync
 * Mirrors sntp_sync.c architecture exactly.
 * Plain TCP socket GET to Oracle:80, no TLS.
 */

#include "http_time_sync.h"
#include "time_sync_writer.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(http_time_sync, LOG_LEVEL_INF);

#define TIME_SERVER_HOST    "161.33.232.177"
#define TIME_SERVER_PORT    80
#define TIME_TIMEOUT_MS     5000
#define TIME_RESYNC_S       60
#define TIME_RETRY_MS       10000

#define HTTP_GET "GET /time HTTP/1.0\r\nHost: 161.33.232.177\r\n\r\n"

#define STACK_SIZE  2048
#define PRIORITY    6

K_THREAD_STACK_DEFINE(http_time_stack, STACK_SIZE);
static struct k_thread http_time_tid;

static uint32_t last_utc_time   = 0;
static uint16_t last_utc_ms     = 0;
static uint32_t last_utc_set_ms = 0;

uint32_t http_time_get_utc(uint16_t *out_ms) {
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

static int do_http_time_query(uint32_t *utc_out, uint16_t *ms_out) {

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(TIME_SERVER_PORT);
    zsock_inet_pton(AF_INET, TIME_SERVER_HOST, &addr.sin_addr);

    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LOG_ERR("Socket create failed: %d", errno);
        return -errno;
    }

    struct zsock_timeval tv = {
        .tv_sec  = TIME_TIMEOUT_MS / 1000,
        .tv_usec = 0
    };
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        LOG_ERR("Connect failed: %d", errno);
        zsock_close(sock);
        return -errno;
    }

    ret = zsock_send(sock, HTTP_GET, strlen(HTTP_GET), 0);
    if (ret < 0) {
        LOG_ERR("Send failed: %d", errno);
        zsock_close(sock);
        return -errno;
    }

    /* Read until EOF — HTTP/1.0 server closes after body */
    char buf[256] = {0};
    size_t total = 0;

    while (total < sizeof(buf) - 1) {
        int n = zsock_recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n < 0) {
            LOG_ERR("Recv failed: %d", errno);
            zsock_close(sock);
            return -errno;
        }
        if (n == 0) {
            break;  /* server closed — full response received */
        }
        total += n;
    }
    zsock_close(sock);

    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        LOG_ERR("No HTTP body found in %u bytes", (unsigned)total);
        return -EBADMSG;
    }
    body += 4;

    char *dot = strchr(body, '.');
    *utc_out = (uint32_t)atoi(body);
    *ms_out  = dot ? (uint16_t)atoi(dot + 1) : 0;

    if (*utc_out < 1700000000U) {
        LOG_ERR("Implausible UTC: %u", *utc_out);
        return -EBADMSG;
    }

    last_utc_time   = *utc_out;
    last_utc_ms     = *ms_out;
    last_utc_set_ms = k_uptime_get_32();

    time_t t = (time_t)(*utc_out);
    struct tm *tm = gmtime(&t);
    LOG_INF("HTTP time: UTC = %u.%03u (%04d-%02d-%02d %02d:%02d:%02d UTC)",
        *utc_out, *ms_out,
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);

    return 0;
}

static void http_time_thread(void) {

    LOG_INF("HTTP time sync thread started");
    k_sleep(K_SECONDS(3));

    while (true) {
        uint32_t utc = 0;
        uint16_t ms  = 0;
        int ret = do_http_time_query(&utc, &ms);

        if (ret == 0) {
            time_sync_writer_set_utc(utc, ms);
        } else {
            LOG_WRN("HTTP time failed (%d), retrying in %d ms",
                    ret, TIME_RETRY_MS);
            k_sleep(K_MSEC(TIME_RETRY_MS));
            continue;
        }

        k_sleep(K_SECONDS(TIME_RESYNC_S));
    }
}

void http_time_sync_start(void) {

    k_thread_create(&http_time_tid,
                    http_time_stack,
                    K_THREAD_STACK_SIZEOF(http_time_stack),
                    (k_thread_entry_t)http_time_thread,
                    NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&http_time_tid, "http_time_sync");
}