#ifndef SOUND_H
#define SOUND_H

/**
 * @file sound.h
 * @brief SPH0645 I2S microphone FFT analyser — public interface.
 *
 * The sound module runs as a Zephyr thread, continuously sampling the
 * SPH0645 MEMS microphone via I2S, computing a 1024-point FFT every
 * ~23 ms, and publishing two messages per second:
 *
 *   sound_q     — compact 3-field summary for the existing sensor_blk
 *                 (rms, peak_freq, peak_mag) used by combiner + main BLE char
 *
 *   sound_spec_q — full 348-bin spectrum for the dedicated sound BLE
 *                  characteristic, chunked into 3 notifications by sound_ble.c
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>

/* ── Thread config ──────────────────────────────────────── */
#define SOUND_STACK_SIZE   4096
#define SOUND_PRIORITY     4

/* ── Bin range (50 Hz – 15 kHz at Δf=43.1 Hz) ──────────── */
#define SOUND_BIN_LOW    1U
#define SOUND_BIN_HIGH   348U
#define SOUND_NUM_BINS   (SOUND_BIN_HIGH - SOUND_BIN_LOW + 1U)   /* 348 */

/* ── Full spectrum message (→ sound_spec_q) ─────────────── */
/*
 * Published once per second to sound_spec_q.
 * Consumed by sound_ble.c which chunks it into 3 BLE notifications.
 *
 * bins[i] = averaged magnitude for bin (SOUND_BIN_LOW + i), scaled ×10
 * so uint16_t gives 0–6553.5 range — sufficient for ±131072 normalised mag.
 *
 * utc_sec + utc_ms: millisecond-precision UTC at measurement, stamped by
 * sound_thread via time_sync_get_utc_ms(). utc_sec is 0 if unsynced.
 * Used by gateway as reassembly key and forwarded to Azure as timestamp.
 */
struct sound_spec_msg {
    uint32_t utc_sec;                    /* offset 0,  size 4  */
    uint16_t utc_ms;                     /* offset 4,  size 2  */
    int16_t  rms_dbfs_x100;              /* offset 6,  size 2  */
    uint64_t uptime_ms;                  /* offset 8,  size 8 ✓ */
    uint16_t bins[SOUND_NUM_BINS];       /* offset 16, size 696 */
};                                       /* total: 712 bytes */

/* ── Message queues ─────────────────────────────────────── */
#define SOUND_Q_DEPTH  4
extern struct k_msgq snd_q;  /* full spectrum    */

/* ── Thread entry point ─────────────────────────────────── */
extern void sound_thread(void);

#endif /* SOUND_H */