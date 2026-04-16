/**
 * @file time_sync_writer.c
 * @brief Writes UTC time-sync packets to sensor nodes over GATT WRITE.
 */

#include "time_sync_writer.h"
#include "bluetooth.h"   /* for write_params_array, conns, etc. */
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(time_sync_writer, LOG_LEVEL_INF);

/* ── Stored UTC reference ───────────────────────────────── */
static uint32_t  gw_utc_sec    = 0;   /* last UTC seconds from WiFi board   */
static uint16_t  gw_utc_ms     = 0;   /* last UTC milliseconds (0–999)       */
static uint32_t  gw_utc_set_ms = 0;   /* k_uptime_get_32() when it was set   */
static bool      utc_valid     = false;

/* One static write buffer per connection slot (matching existing pattern) */
static uint8_t timesync_bufs[MAX_CONN][TIMESYNC_PACKET_LEN];

/* Reuse write_params_array from bluetooth.c but only after dev_id write is done.
 * To avoid collision define a separate write params array for time sync. */
static struct bt_gatt_write_params ts_write_params[MAX_CONN];

/* ── Write callback ─────────────────────────────────────── */
static void ts_write_cb(struct bt_conn *conn, uint8_t err,
                        struct bt_gatt_write_params *params) {
    if (err) {
        LOG_WRN("time_sync write failed (err 0x%02x)", err);
    } else {
        LOG_DBG("time_sync write OK");
    }
}

/* ── Public API ─────────────────────────────────────────── */

void time_sync_writer_set_utc(uint32_t utc_sec, uint16_t utc_ms) {

    gw_utc_sec    = utc_sec;
    gw_utc_ms     = (utc_ms > 999) ? 999 : utc_ms;
    gw_utc_set_ms = k_uptime_get_32();
    utc_valid     = true;
    LOG_INF("time_sync_writer: UTC set to %" PRIu32 ".%03u", utc_sec, utc_ms);
}

bool time_sync_writer_has_utc(void)
{
    return utc_valid;
}

void time_sync_writer_send(struct bt_conn *conn, int index,
                           uint16_t char_handle) {
                            
    if (!utc_valid) {
        LOG_WRN("time_sync_writer: no UTC available — skipping node %d", index);
        return;
    }

    uint32_t elapsed_ms  = k_uptime_get_32() - gw_utc_set_ms;

    /* Extrapolate ms forward from stored reference */
    uint32_t total_ms    = gw_utc_ms + elapsed_ms;
    uint32_t current_utc = gw_utc_sec + (total_ms / 1000U);
    uint16_t current_ms  = (uint16_t)(total_ms % 1000U);

    uint8_t *b = timesync_bufs[index];
    b[0] = TIMESYNC_MAGIC;
    b[1] = (current_utc >> 24) & 0xFF;
    b[2] = (current_utc >> 16) & 0xFF;
    b[3] = (current_utc >>  8) & 0xFF;
    b[4] =  current_utc        & 0xFF;
    b[5] = (current_ms  >>  8) & 0xFF;
    b[6] =  current_ms         & 0xFF;

    ts_write_params[index].handle = char_handle;
    ts_write_params[index].offset = 0;
    ts_write_params[index].data   = b;
    ts_write_params[index].length = TIMESYNC_PACKET_LEN;
    ts_write_params[index].func   = ts_write_cb;

    int err = bt_gatt_write(conn, &ts_write_params[index]);
    if (err) {
        LOG_ERR("time_sync_writer: write to node %d failed (%d)", index, err);
    } else {
        LOG_INF("time_sync_writer: sent UTC=%" PRIu32 ".%03u to node %d",
                current_utc, current_ms, index);
    }
}