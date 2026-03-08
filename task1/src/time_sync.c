/**
 * @file time_sync.c
 * @brief UTC wall-clock with drift correction for Zephyr sensor nodes.
 *
 * Sync packets are delivered via GATT write from the gateway.
 * Two sync points are stored; once both are populated a linear drift
 * model is built and applied when getCorrectedUTC() is called.
 *
 * Thread safety: all state is protected by a Zephyr spinlock so
 * time_sync_get_utc() can safely be called from any thread context
 * (combiner_thread, tracker_thread, etc.).
 */

#include "time_sync.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(time_sync, LOG_LEVEL_INF);

/* ── Constants ──────────────────────────────────────────── */
#define SYNC_WINDOW_SIZE  8    /* number of sync points for regression */

/* ── Sync point ─────────────────────────────────────────── */
struct sync_point {
    uint32_t local_uptime_ms;
    int64_t  utc_ms;          /* UTC in milliseconds since epoch */
};

/* ── Module state ───────────────────────────────────────── */
static struct sync_point  points[SYNC_WINDOW_SIZE];
static uint8_t            point_count;   /* 0..SYNC_WINDOW_SIZE        */
static uint8_t            point_head;    /* index of oldest entry       */
static int32_t            drift_ppm;
static int64_t            base_utc_ms;   /* regression anchor (oldest) — UTC ms */
static uint32_t           base_local_ms; /* regression anchor (oldest)  */
static struct k_spinlock  lock;

/*
 * Least-squares linear regression over the sync window.
 *
 * We model:  utc_elapsed = (1 + ε) × local_elapsed
 * i.e. fit a line through the origin of (local_elapsed, utc_elapsed)
 * pairs, where both are measured relative to the oldest point in the
 * window to keep the numbers small and avoid 64-bit overflow.
 *
 * Least-squares slope through the origin:
 *   slope = Σ(x_i × y_i) / Σ(x_i²)
 *
 * slope > 1  → local clock runs fast (positive drift_ppm)
 * slope < 1  → local clock runs slow (negative drift_ppm)
 */
static void update_drift(void)
{
    if (point_count < 2) {
        return;
    }

    /* Anchor = oldest point in the circular buffer */
    uint8_t oldest = point_head % point_count;  /* valid once count >= 1 */
    base_utc_ms   = points[oldest].utc_ms;
    base_local_ms = points[oldest].local_uptime_ms;

    int64_t sum_xx = 0;   /* Σ local_elapsed² */
    int64_t sum_xy = 0;   /* Σ local_elapsed × utc_elapsed_ms */

    for (uint8_t i = 0; i < point_count; i++) {
        uint8_t idx = (point_head + i) % SYNC_WINDOW_SIZE;

        /* x = local elapsed ms from anchor */
        int64_t x = (int64_t)points[idx].local_uptime_ms
                  - (int64_t)base_local_ms;

        /* y = UTC elapsed ms from anchor (already in ms) */
        int64_t y = points[idx].utc_ms - base_utc_ms;

        sum_xx += x * x;
        sum_xy += x * y;
    }

    if (sum_xx == 0) {
        return;  /* all points at same local time — shouldn't happen */
    }

    /*
     * slope = sum_xy / sum_xx
     * drift_ppm = (slope - 1) × 1e6
     *           = (sum_xy - sum_xx) / sum_xx × 1e6
     */
    int64_t ppm = ((sum_xy - sum_xx) * 1000000LL) / sum_xx;

    if (ppm >  1000) ppm =  1000;
    if (ppm < -1000) ppm = -1000;
    drift_ppm = (int32_t)ppm;

    LOG_INF("time_sync: regression over %u points  drift=%" PRId32
            " PPM  (%.2f sec/day)",
            point_count, drift_ppm,
            (double)drift_ppm * 86400.0 / 1e6);
}

/* ── Public API ─────────────────────────────────────────── */

bool time_sync_handle_write(const void *buf, uint16_t len)
{
    if (len < TIMESYNC_PACKET_LEN) {
        return false;
    }

    const uint8_t *b = (const uint8_t *)buf;

    if (b[0] != TIMESYNC_MAGIC) {
        return false;
    }

    uint32_t utc_sec = ((uint32_t)b[1] << 24) |
                       ((uint32_t)b[2] << 16) |
                       ((uint32_t)b[3] <<  8) |
                        (uint32_t)b[4];

    uint16_t utc_ms_part = ((uint16_t)b[5] << 8) | b[6];
    if (utc_ms_part > 999) utc_ms_part = 999;

    int64_t utc_ms = (int64_t)utc_sec * 1000LL + utc_ms_part;

    uint32_t local_now = k_uptime_get_32();

    k_spinlock_key_t key = k_spin_lock(&lock);

    uint8_t slot = (point_head + point_count) % SYNC_WINDOW_SIZE;

    if (point_count < SYNC_WINDOW_SIZE) {
        points[slot].local_uptime_ms = local_now;
        points[slot].utc_ms          = utc_ms;
        point_count++;
    } else {
        points[point_head].local_uptime_ms = local_now;
        points[point_head].utc_ms          = utc_ms;
        point_head = (point_head + 1) % SYNC_WINDOW_SIZE;
    }

    update_drift();

    k_spin_unlock(&lock, key);

    LOG_INF("time_sync: synced UTC=%" PRIu32 ".%03u  local_ms=%" PRIu32
            "  drift=%" PRId32 " PPM",
            utc_sec, utc_ms_part, local_now, drift_ppm);

    return true;
}

uint32_t time_sync_get_utc_ms(uint16_t *out_ms)
{
    k_spinlock_key_t key = k_spin_lock(&lock);

    if (point_count == 0) {
        k_spin_unlock(&lock, key);
        if (out_ms) *out_ms = 0;
        return 0;
    }

    uint32_t local_elapsed_ms = k_uptime_get_32() - base_local_ms;

    /* Apply regression slope: real_ms = local_ms × (1 - drift_ppm/1e6) */
    int64_t real_elapsed_ms = (int64_t)local_elapsed_ms
                            - ((int64_t)local_elapsed_ms * drift_ppm / 1000000LL);

    int64_t total_utc_ms = base_utc_ms + real_elapsed_ms;

    uint32_t utc_sec = (uint32_t)(total_utc_ms / 1000LL);
    uint16_t utc_ms  = (uint16_t)(total_utc_ms % 1000LL);

    k_spin_unlock(&lock, key);

    if (out_ms) *out_ms = utc_ms;
    return utc_sec;
}

uint32_t time_sync_get_utc(void)
{
    return time_sync_get_utc_ms(NULL);
}

/* Replace the broken time_sync_is_valid(): */
bool time_sync_is_valid(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool valid = (point_count > 0);
    k_spin_unlock(&lock, key);
    return valid;
}

int32_t time_sync_get_drift_ppm(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    int32_t ppm = drift_ppm;
    k_spin_unlock(&lock, key);
    return ppm;
}