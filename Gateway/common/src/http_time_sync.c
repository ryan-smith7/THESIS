/*
 * http_time_sync.c — HTTP-based UTC time synchronisation with SNTP offset
 *
 * Implements the SNTP clock offset formula (RFC 4330 §5) over plain HTTP/TCP
 * rather than UDP/123, satisfying the protocol constraint of ports 80/443 only.
 *
 * Four timestamps are exchanged per sync cycle:
 *   t1 — client uptime (ms) at the moment the HTTP request is sent
 *   t2 — server UTC (ms)    at the moment the request arrives 
 *   t3 — server UTC (ms)    at the moment the response is sent
 *   t4 — client uptime (ms) at the moment the full response is received
 *
 * Clock offset θ is then:
 *   θ = ((t2 − t1) + (t3 − t4)) / 2
 *
 * θ directly encodes (UTC_ms − uptime_ms) and is stored as utc_offset_ms.
 * All subsequent time queries are then simply:
 *   current UTC = k_uptime_get() + utc_offset_ms
 *
 * utc_offset_ms remains 0 until the first successful sync.  Callers treat
 * a return value of 0 from http_time_get_utc() as not yet synchronised
 * and hold any transmissions until a valid UTC is available.
 *
 * Server response body (12 bytes, big-endian):
 *   [0..3]  t2_sec  uint32
 *   [4..5]  t2_ms   uint16
 *   [6..9]  t3_sec  uint32
 *   [10..11] t3_ms  uint16
 */

#include "http_time_sync.h"
#include "time_sync_writer.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(http_time_sync, LOG_LEVEL_INF);

/* ── Configuration ───────────────────────────────────────────────────────── */

#define TIME_SERVER_HOST  "161.33.232.177"  /* Oracle VM serving /time */
#define TIME_SERVER_PORT  80                /* Plain HTTP — no TLS required*/
#define TIME_TIMEOUT_MS   5000              /* Per-operation socket timeout*/
#define TIME_RESYNC_S     60                /* Seconds between SNTP syncs*/
#define TIME_RETRY_MS     10000             /* Retry delay after a failed sync  */

/* HTTP/1.0 so the server closes after the body — no chunked transfer needed  */
#define HTTP_GET "GET /time HTTP/1.0\r\nHost: 161.33.232.177\r\n\r\n"

/* Binary response body layout — 12 bytes total                                */
#define BODY_LEN         12
#define BODY_T2_SEC_OFF   0   /* uint32 big-endian */
#define BODY_T2_MS_OFF    4   /* uint16 big-endian */
#define BODY_T3_SEC_OFF   6   /* uint32 big-endian */
#define BODY_T3_MS_OFF   10   /* uint16 big-endian */

#define STACK_SIZE  4096   /* Sized for TCP socket overhead                    */
#define PRIORITY    6      /* Below sensor threads; time sync is not urgent    */

K_THREAD_STACK_DEFINE(http_time_stack, STACK_SIZE);
static struct k_thread http_time_tid;

/*
 * UTC offset in milliseconds: utc_offset_ms = UTC_ms − uptime_ms.
 * Zero means unsynced — http_time_get_utc() returns 0 until first sync.
 */
static int64_t utc_offset_ms = 0;

/**
 * @brief Return the current UTC estimate without a network round-trip.
 *
 * Adds the stored offset to the current uptime. Returns 0 if no sync
 * has occurred yet.
 *
 * @param out_ms  Optional output for the sub-second millisecond component.
 * @return        UTC seconds, or 0 if unsynchronised.
 */
uint32_t http_time_get_utc(uint16_t *out_ms) {
    if (utc_offset_ms == 0) {
        if (out_ms) *out_ms = 0;
        return 0;
    }
    int64_t utc_ms = (int64_t)k_uptime_get() + utc_offset_ms;
    if (out_ms) *out_ms = (uint16_t)(utc_ms % 1000);
    return (uint32_t)(utc_ms / 1000);
}

/**
 * @brief Open a TCP socket to the time server with send/recv timeouts applied.
 *
 * @return  Connected socket fd, or negative errno on failure.
 */
static int open_socket(void) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(TIME_SERVER_PORT);
    zsock_inet_pton(AF_INET, TIME_SERVER_HOST, &addr.sin_addr);

    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LOG_ERR("Socket failed: %d", errno);
        return -errno;
    }

    /* Timeouts prevent a hung server from blocking the thread indefinitely   */
    struct zsock_timeval tv = { .tv_sec = TIME_TIMEOUT_MS / 1000 };
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("Connect failed: %d", errno);
        zsock_close(sock);
        return -errno;
    }
    return sock;
}

/**
 * @brief Receive the full HTTP response and return a pointer to the body.
 *
 * Reads until EOF (HTTP/1.0 server closes after body). Returns NULL if
 * the header/body separator is not found.
 *
 * @param sock  Connected socket.
 * @param buf   Caller-supplied buffer.
 * @param len   Size of buf.
 * @return      Pointer to first body byte, or NULL on failure.
 */
static uint8_t *recv_body(int sock, uint8_t *buf, size_t len) {
    size_t total = 0;
    int n;
    while (total < len - 1 &&
           (n = zsock_recv(sock, buf + total, len - 1 - total, 0)) > 0) {
        total += n;
    }
    /* Locate the blank line separating HTTP headers from body */
    uint8_t *body = (uint8_t *)strstr((char *)buf, "\r\n\r\n");
    if (body == NULL) {
        return NULL;
    }
    return body + 4;
}

/**
 * @brief Perform one SNTP exchange and update utc_offset_ms.
 *
 * Sends an HTTP GET, records t1/t4 client uptime timestamps, decodes
 * t2/t3 from the 12-byte binary response body, then computes the clock
 * offset via the formula: θ = ((t2−t1) + (t3−t4)) / 2.
 *
 * @param utc_out  Corrected UTC seconds at time of computation.
 * @param ms_out   Corrected UTC milliseconds.
 * @return         0 on success, negative errno on failure.
 */
static int do_sntp_sync(uint32_t *utc_out, uint16_t *ms_out) {

    int sock = open_socket();
    if (sock < 0) {
        return sock;
    }

    /* t1: client uptime at send */
    int64_t t1 = k_uptime_get();
    zsock_send(sock, HTTP_GET, strlen(HTTP_GET), 0);

    uint8_t buf[256] = {0};
    uint8_t *body = recv_body(sock, buf, sizeof(buf));

    /* t4: client uptime once full response is received */
    int64_t t4 = k_uptime_get();
    zsock_close(sock);

    if (!body) {
        LOG_ERR("No HTTP body");
        return -EBADMSG;
    }

    /* Validate we have at least BODY_LEN bytes of body */
    size_t body_len = (buf + sizeof(buf)) - body;
    if (body_len < BODY_LEN) {
        LOG_ERR("Body too short: %u bytes", (unsigned)body_len);
        return -EBADMSG;
    }

    /* Read t2 and t3 directly from binary body — no parsing needed           */
    uint32_t t2_sec = (uint32_t)body[BODY_T2_SEC_OFF    ] << 24 |
                      (uint32_t)body[BODY_T2_SEC_OFF + 1] << 16 |
                      (uint32_t)body[BODY_T2_SEC_OFF + 2] <<  8 |
                      (uint32_t)body[BODY_T2_SEC_OFF + 3];
    uint16_t t2_ms  = (uint16_t)body[BODY_T2_MS_OFF     ] <<  8 |
                      (uint16_t)body[BODY_T2_MS_OFF  + 1];
    uint32_t t3_sec = (uint32_t)body[BODY_T3_SEC_OFF    ] << 24 |
                      (uint32_t)body[BODY_T3_SEC_OFF + 1] << 16 |
                      (uint32_t)body[BODY_T3_SEC_OFF + 2] <<  8 |
                      (uint32_t)body[BODY_T3_SEC_OFF + 3];
    uint16_t t3_ms  = (uint16_t)body[BODY_T3_MS_OFF     ] <<  8 |
                      (uint16_t)body[BODY_T3_MS_OFF  + 1];

    if (t2_sec < 1700000000U || t3_sec < 1700000000U) {
        LOG_ERR("Implausible t2=%u t3=%u", t2_sec, t3_sec);
        return -EBADMSG;
    }

    int64_t t2_utc = (int64_t)t2_sec * 1000 + t2_ms;
    int64_t t3_utc = (int64_t)t3_sec * 1000 + t3_ms;
    int64_t rtt    = t4 - t1;

    /* θ = ((t2 − t1) + (t3 − t4)) / 2 — equals UTC_ms − uptime_ms          */
    utc_offset_ms = ((t2_utc - t1) + (t3_utc - t4)) / 2;

    /* Read corrected UTC at this exact moment via the stored offset           */
    *utc_out = http_time_get_utc(ms_out);

    if (*utc_out < 1700000000U) {
        LOG_ERR("Bad corrected UTC: %u", *utc_out);
        return -EBADMSG;
    }

    struct tm tm_buf;
    time_t t_log = (time_t)(*utc_out);
    gmtime_r(&t_log, &tm_buf);
    LOG_INF("UTC = %u.%03u (%04d-%02d-%02d %02d:%02d:%02d) RTT=%lldms offset=%lldms",
            *utc_out, *ms_out,
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            rtt, utc_offset_ms);

    LOG_INF("SNTP detail: "
        "t1=%" PRId64 " t4=%" PRId64 " rtt=%" PRId64
        "  fwd=(t2-t1)=%" PRId64
        "  ret=(t3-t4)=%" PRId64
        "  asymmetry=%" PRId64,
        t1, t4, rtt,
        (t2_utc - t1),
        (t3_utc - t4),
        (t2_utc - t1) + (t3_utc - t4));

    return 0;
}

/**
 * @brief Time sync thread — runs do_sntp_sync() on first boot then every
 * TIME_RESYNC_S seconds, retrying after TIME_RETRY_MS on failure.
 *
 * utc_offset_ms stays 0 until the first successful sync; other threads
 * treat http_time_get_utc() == 0 as "not yet valid" and hold transmissions.
 */
static void http_time_thread(void) {

    LOG_INF("HTTP time sync thread started");
    k_sleep(K_SECONDS(3)); /* allow network stack to come up */

    while (true) {
        uint32_t utc = 0;
        uint16_t ms  = 0;
        int ret = do_sntp_sync(&utc, &ms);

        if (ret == 0) {
            time_sync_writer_set_utc(utc, ms);
            k_sleep(K_SECONDS(TIME_RESYNC_S));
        } else {
            LOG_WRN("SNTP sync failed (%d), retrying in %d ms", ret, TIME_RETRY_MS);
            k_sleep(K_MSEC(TIME_RETRY_MS));
        }
    }
}

/**
 * @brief Spawn the HTTP time sync thread.
 *
 * Must be called after the network interface is initialised.
 */
void http_time_sync_start(void) {
    k_thread_create(&http_time_tid,
                    http_time_stack,
                    K_THREAD_STACK_SIZEOF(http_time_stack),
                    (k_thread_entry_t)http_time_thread,
                    NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&http_time_tid, "http_time_sync");
}