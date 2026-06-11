#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include <stdint.h>
/**
 * @brief Start the SNTP sync thread.
 *
 * Queries server immediately after WiFi is ready, then re-syncs
 * every SNTP_RESYNC_INTERVAL_S seconds.
 *
 * Call after WiFi is confirmed up
 */
void sntp_sync_start(void);

/**
 * @brief Get the last successfully synced UTC timestamp, extrapolated to now.
 *
 * @param out_ms  Sub-second milliseconds
 * @return Unix UTC seconds, or 0 if no sync has occurred yet.
 */
uint32_t sntp_get_last_utc(uint16_t *out_ms);

#endif /* SNTP_SYNC_H */