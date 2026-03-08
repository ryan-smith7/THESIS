/**
 * @file time_sync.h
 * @brief UTC wall-clock with BLE-write-based sync and drift correction.
 *
 * The gateway writes a 5-byte packet to the sensor characteristic:
 *   Byte 0:    TIMESYNC_MAGIC (0xFE)
 *   Bytes 1-4: UTC unix timestamp (uint32_t, big-endian)
 *
 * The node accumulates up to SYNC_WINDOW_SIZE sync points and applies
 * least-squares linear regression to estimate clock skew (drift_ppm).
 * time_sync_get_utc() applies this correction at any time, including
 * during BLE disconnection.
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <stdbool.h>

#define TIMESYNC_MAGIC       0xFE
#define TIMESYNC_PACKET_LEN  7     /* magic(1) + utc_sec(4) + utc_ms(2) */

/**
 * @brief Process an incoming GATT write buffer for a time sync packet.
 *
 * Validates the magic byte (0xFE), parses UTC seconds + milliseconds,
 * adds to the regression window, and updates the drift estimate.
 *
 * @param buf  Pointer to the write buffer (must be >= TIMESYNC_PACKET_LEN bytes).
 * @param len  Length of the write buffer.
 * @return     true if this was a valid time sync packet and was consumed.
 */
bool time_sync_handle_write(const void *buf, uint16_t len);

/**
 * @brief Get drift-corrected UTC with millisecond resolution.
 *
 * @param[out] out_ms  Sub-second milliseconds (0–999). May be NULL.
 * @return             UTC unix seconds, or 0 if not yet synced.
 */
uint32_t time_sync_get_utc_ms(uint16_t *out_ms);

/**
 * @brief Get drift-corrected UTC seconds (convenience wrapper).
 *
 * Equivalent to time_sync_get_utc_ms(NULL).
 *
 * @return UTC unix timestamp (seconds since epoch), or 0 if not yet synced.
 */
uint32_t time_sync_get_utc(void);

/**
 * @brief Check whether at least one valid sync has been received.
 *
 * @return true if time_sync_get_utc() will return a non-zero value.
 */
bool time_sync_is_valid(void);

/**
 * @brief Get the current estimated clock drift in PPM.
 *
 * Positive = local clock runs fast, negative = runs slow.
 * Returns 0 until at least two sync points have been received.
 *
 * @return Drift estimate in parts-per-million.
 */
int32_t time_sync_get_drift_ppm(void);

#endif /* TIME_SYNC_H */