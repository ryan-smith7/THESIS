/**
 * @file time_sync_writer.c
 * @brief Writes UTC time-sync packets to sensor nodes over GATT WRITE.
 */

#include "time_sync_writer.h"
#include "http_time_sync.h"
#include "bluetooth.h"
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(time_sync_writer, LOG_LEVEL_INF);

static uint8_t                   timesync_bufs[MAX_CONN][TIMESYNC_PACKET_LEN];
static struct bt_gatt_write_params ts_write_params[MAX_CONN];

/**
 * @brief GATT write completion callback — logs success or failure.
 */
static void ts_write_cb(struct bt_conn *conn, uint8_t err,
                        struct bt_gatt_write_params *params) {
    if (err) LOG_WRN("time_sync write failed (err 0x%02x)", err);
    else     LOG_DBG("time_sync write OK");
}

/**
 * @brief Irrelevant old function http_time_sync.c.
 *
 * SHOULD COME BACK AND REMOVE
 * UTC is sourced directly from http_time_get_utc() at send time rather
 * than stored here.
 */
void time_sync_writer_set_utc(uint32_t utc_sec, uint16_t utc_ms) {
    LOG_INF("time_sync_writer: UTC set to %" PRIu32 ".%03u", utc_sec, utc_ms);
}

/**
 * @brief Returns true if a valid UTC is available from http_time_get_utc().
 */
bool time_sync_writer_has_utc(void) {
    return http_time_get_utc(NULL) != 0;
}

/**
 * @brief Write the current UTC to a sensor node via GATT WRITE.
 *
 * Reads UTC directly from http_time_get_utc() and encodes it into a prefixed
 * TIMESYNC_PACKET_LEN byte.
 * Returns immediately if UTC is not yet valid.
 *
 * @param conn        Connection to the target sensor node.
 * @param index       Connection slot index (0..MAX_CONN-1).
 * @param char_handle GATT characteristic handle to write to.
 */
void time_sync_writer_send(struct bt_conn *conn, int index,
                           uint16_t char_handle) {

    uint16_t ms  = 0;
    uint32_t utc = http_time_get_utc(&ms);

    if (utc == 0) {
        LOG_WRN("time_sync_writer: no UTC available — skipping node %d", index);
        return;
    }

    uint8_t *b = timesync_bufs[index];
    b[0] = TIMESYNC_PREFIX;
    b[1] = (utc >> 24) & 0xFF;
    b[2] = (utc >> 16) & 0xFF;
    b[3] = (utc >>  8) & 0xFF;
    b[4] =  utc        & 0xFF;
    b[5] = (ms  >>  8) & 0xFF;
    b[6] =  ms         & 0xFF;

    ts_write_params[index].handle = char_handle;
    ts_write_params[index].offset = 0;
    ts_write_params[index].data   = b;
    ts_write_params[index].length = TIMESYNC_PACKET_LEN;
    ts_write_params[index].func   = ts_write_cb;

    int err = bt_gatt_write(conn, &ts_write_params[index]);
    if (err) LOG_ERR("time_sync_writer: write to node %d failed (%d)", index, err);
    else     LOG_INF("time_sync_writer: sent UTC=%" PRIu32 ".%03u to node %d",
                     utc, ms, index);
}