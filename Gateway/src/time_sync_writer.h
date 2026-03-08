/**
 * @file time_sync_writer.h
 * @brief Gateway-side: periodically writes UTC time to all connected sensor nodes.
 *
 * The gateway receives UTC from the WiFi board over UART as a dedicated
 * frame type (magic 0xCC). Once received it writes a TIMESYNC_PACKET to
 * each connected node's writable characteristic every TIMESYNC_INTERVAL_S.
 */

#ifndef TIME_SYNC_WRITER_H
#define TIME_SYNC_WRITER_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>

#define TIMESYNC_MAGIC       0xFE
#define TIMESYNC_PACKET_LEN  7     /* magic(1) + utc_sec(4) + utc_ms(2) */
#define TIMESYNC_INTERVAL_S  30    /* sync every 30 seconds         */

/* UART frame magic for UTC frames inbound from WiFi board */
#define FRAME_MAGIC_UTC      0xCC
#define FRAME_UTC_LEN        6     /* utc_sec(4) + utc_ms(2) — bytes after magic */

/**
 * @brief Called from UART RX handler when a 0xCC UTC frame arrives.
 * Stores the UTC value (seconds + milliseconds) for distribution to nodes.
 *
 * @param utc_sec  UTC unix seconds received from WiFi board.
 * @param utc_ms   UTC sub-second milliseconds (0–999).
 */
void time_sync_writer_set_utc(uint32_t utc_sec, uint16_t utc_ms);

/**
 * @brief Write current UTC time to a single connection.
 * Call this after subscribing to a node (inside sensor_cccd_disc_func)
 * for an immediate first sync, and periodically thereafter.
 *
 * @param conn   Active BLE connection to write to
 * @param index  Connection index (for write_params_array slot)
 * @param char_handle  Value handle of the writable characteristic
 */
void time_sync_writer_send(struct bt_conn *conn, int index,
                           uint16_t char_handle);

/**
 * @brief Check whether the gateway has a valid UTC reference.
 */
bool time_sync_writer_has_utc(void);

#endif /* TIME_SYNC_WRITER_H */