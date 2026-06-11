/**
 * @file time_sync_writer.h
 * @brief Gateway-side: periodically writes UTC time to all connected sensor nodes.
 *
 * The gateway receives UTC from the WiFi board over UART as a dedicated
 * frame type (uses prefix 0xCC). Once received it writes a TIMESYNC_PACKET to
 * each connected node's writable characteristic every TIMESYNC_INTERVAL_S.
 */

#ifndef TIME_SYNC_WRITER_H
#define TIME_SYNC_WRITER_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>

#define TIMESYNC_PREFIX       0xFE
#define TIMESYNC_PACKET_LEN  7     /* magic(1) + utc_sec(4) + utc_ms(2) */
#define TIMESYNC_INTERVAL_S  30    /* sync every 30 seconds         */

/**
 * @brief No-op stub kept for API compatibility with http_time_sync.c.
 *
 * UTC is sourced directly from http_time_get_utc() at send time rather
 * than stored here.
 */
void time_sync_writer_set_utc(uint32_t utc_sec, uint16_t utc_ms);

/**
 * @brief Write the current UTC to a sensor node via GATT WRITE.
 *
 * Reads UTC directly from http_time_get_utc() and encodes it into a
 * TIMESYNC_PACKET_LEN byte packet prefixed with TIMESYNC_PREFIX.
 * Returns immediately if UTC is not yet valid.
 *
 * @param conn        Connection to the target sensor node.
 * @param index       Connection slot index (0..MAX_CONN-1).
 * @param char_handle GATT characteristic handle to write to.
 */
void time_sync_writer_send(struct bt_conn *conn, int index,
                           uint16_t char_handle);

/**
 * @brief Returns true if a valid UTC is available from http_time_get_utc().
 */
bool time_sync_writer_has_utc(void);

#endif /* TIME_SYNC_WRITER_H */