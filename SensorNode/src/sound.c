// /**
//  * @file sound.c
//  * @brief SPH0645 I2S microphone FFT analyser thread.
//  *
//  * Reads I2S audio, computes 1024-point FFT, averages ~43 windows/sec,
//  * then publishes two messages:
//  *
//  *   sound_q      — compact summary (rms, peak_freq, peak_mag)
//  *   sound_spec_q — full 348-bin spectrum for BLE chunked characteristic
//  *
//  * Hardware loopback: GPIO27 TX→RX in overlay keeps BCLK alive.
//  */

// #include "sound.h"
#include "time_sync.h"

// #include <zephyr/kernel.h>
// #include <zephyr/device.h>
// #include <zephyr/drivers/i2s.h>
// #include <zephyr/logging/log.h>
// #include <string.h>
// #include <math.h>

// LOG_MODULE_REGISTER(sound, LOG_LEVEL_INF);

// #define M_PI 3.14159265358979323846f

// /* ── Node labels ────────────────────────────────────────── */
// #if DT_NODE_EXISTS(DT_NODELABEL(i2s_rxtx))
// #define I2S_RX_NODE  DT_NODELABEL(i2s_rxtx)
// #define I2S_TX_NODE  I2S_RX_NODE
// #else
// #define I2S_RX_NODE  DT_NODELABEL(i2s_rx)
// #define I2S_TX_NODE  DT_NODELABEL(i2s_tx)
// #endif

// /* ── Audio config ───────────────────────────────────────── */
// #define SAMPLE_RATE        44100U
// #define SAMPLE_BIT_WIDTH   32U
// #define NUM_CHANNELS       2U
// #define FFT_SIZE           1024U
// #define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
// #define BUF_SIZE           (FFT_SIZE * NUM_CHANNELS * BYTES_PER_SAMPLE)
// #define NUM_RX_BUFS        4U
// #define NUM_TX_BUFS        1U
// // #define NUM_TX_BUFS  2U   /* double-buffer — enough to stay ahead of RX */
// #define TIMEOUT_MS         2000
// #define MIC_SLOT           0U   /* SEL=GND → left channel */
// #define FREQ_RES           ((float)SAMPLE_RATE / (float)FFT_SIZE)
// // #define PCM_FULL_SCALE     131072.0

// #define PCM_FULL_SCALE     131072.

// #define WINDOWS_PER_SEC    43U

// /* ── Memory slabs ───────────────────────────────────────── */
// K_MEM_SLAB_DEFINE_STATIC(snd_rx_slab, BUF_SIZE, NUM_RX_BUFS, 4);
// K_MEM_SLAB_DEFINE_STATIC(snd_tx_slab, BUF_SIZE, NUM_TX_BUFS, 4);

// /* ── Message queues ─────────────────────────────────────── */
// K_MSGQ_DEFINE(sound_q,      sizeof(struct sound_msg),      SOUND_Q_DEPTH, 4);
// K_MSGQ_DEFINE(sound_spec_q, sizeof(struct sound_spec_msg), SOUND_Q_DEPTH, 4);

// /* ── Static working buffers ─────────────────────────────── */
// static float   s_windowed[FFT_SIZE];
// static float   s_fft_re[FFT_SIZE];
// static float   s_fft_im[FFT_SIZE];
// static float   s_hann[FFT_SIZE];
// static float   s_bin_accum[FFT_SIZE / 2];
// static uint8_t s_tx_silence[BUF_SIZE];   /* BSS zero — primes TX once */

// /* ── DC blocking filter ─────────────────────────────────── */
// static float dc_block(float x)
// {
//     static float x1 = 0.0f, y1 = 0.0f;
//     float y = x - x1 + 0.9975f * y1;
//     x1 = x; y1 = y;
//     return y;
// }

// /* ── PCM extraction ─────────────────────────────────────── */
// static inline int32_t extract_pcm(uint32_t raw)
// {
//     return (int32_t)raw >> 14;
// }

// /* ── Hann window ────────────────────────────────────────── */
// static void init_hann(void)
// {
//     for (uint32_t i = 0; i < FFT_SIZE; i++) {
//         s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i
//                                          / (float)(FFT_SIZE - 1)));
//     }
// }

// /* ── Cooley-Tukey FFT ───────────────────────────────────── */
// static void fft_compute(void)
// {
//     uint32_t n = FFT_SIZE;
//     uint32_t j = 0;

//     for (uint32_t i = 1; i < n; i++) {
//         uint32_t bit = n >> 1;
//         for (; j & bit; bit >>= 1) { j ^= bit; }
//         j ^= bit;
//         if (i < j) {
//             float tmp;
//             tmp = s_fft_re[i]; s_fft_re[i] = s_fft_re[j]; s_fft_re[j] = tmp;
//             tmp = s_fft_im[i]; s_fft_im[i] = s_fft_im[j]; s_fft_im[j] = tmp;
//         }
//     }

//     for (uint32_t len = 2; len <= n; len <<= 1) {
//         float ang = -2.0f * M_PI / (float)len;
//         float wRe = cosf(ang), wIm = sinf(ang);
//         for (uint32_t i = 0; i < n; i += len) {
//             float curRe = 1.0f, curIm = 0.0f;
//             for (uint32_t k = 0; k < len / 2; k++) {
//                 uint32_t u = i + k, v = i + k + len / 2;
//                 float tRe = curRe * s_fft_re[v] - curIm * s_fft_im[v];
//                 float tIm = curRe * s_fft_im[v] + curIm * s_fft_re[v];
//                 s_fft_re[v] = s_fft_re[u] - tRe;
//                 s_fft_im[v] = s_fft_im[u] - tIm;
//                 s_fft_re[u] += tRe;
//                 s_fft_im[u] += tIm;
//                 float newRe = curRe * wRe - curIm * wIm;
//                 curIm       = curRe * wIm + curIm * wRe;
//                 curRe       = newRe;
//             }
//         }
//     }
// }

// /* ── Publish both queues ────────────────────────────────── */
// static void publish(double avg_dbfs, float peak_freq, float peak_mag)
// {
//     /* 1. Compact summary → sound_q */
//     struct sound_msg summary = {
//         .rms_dbfs_x100 = (int16_t)(avg_dbfs  * 100.0),
//         .peak_freq_hz  = (uint16_t)peak_freq,
//         .peak_mag_x10  = (uint16_t)(peak_mag * 10.0f),
//     };
//     if (k_msgq_put(&sound_q, &summary, K_NO_WAIT) != 0) {
//         struct sound_msg dump;
//         k_msgq_get(&sound_q, &dump, K_NO_WAIT);
//         k_msgq_put(&sound_q, &summary, K_NO_WAIT);
//     }

//     /* 2. Full spectrum → sound_spec_q
//      *
//      * bins[] are magnitude × 10 as uint16_t.
//      * Bins outside BIN_LOW–BIN_HIGH are already 0 from BSS/reset.
//      * sound_ble.c will chunk this into 3 BLE notifications.
//      */
//     struct sound_spec_msg spec;
//     spec.utc_sec       = time_sync_get_utc_ms(&spec.utc_ms);  /* stamp at measurement moment */
//     spec.rms_dbfs_x100 = summary.rms_dbfs_x100;

//     for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
//         uint32_t k = SOUND_BIN_LOW + i;
//         /* s_bin_accum[k] is already averaged before publish() is called */
//         float v = s_bin_accum[k] * 10.0f;
//         spec.bins[i] = (v > 65535.0f) ? 65535U : (uint16_t)v;
//     }

//     // // /* ADD THIS */
//     // printk("[BINS]");
//     // for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
//     //     printk(" %u", spec.bins[i]);
//     // }
//     // printk("\n");

    

//     if (k_msgq_put(&sound_spec_q, &spec, K_NO_WAIT) != 0) {
//         struct sound_spec_msg dump;
//         k_msgq_get(&sound_spec_q, &dump, K_NO_WAIT);
//         k_msgq_put(&sound_spec_q, &spec, K_NO_WAIT);
//     }

//     LOG_INF("Sound: %.1f dBFS  Peak %.0f Hz  (%.1f)",
//             avg_dbfs, (double)peak_freq, (double)peak_mag);
// }

// /* ── Process one window ─────────────────────────────────── */
// static void process_window(uint32_t *frames, size_t num_frames,
//                            double *rms_accum, uint32_t *accum_count)
// {
//     /* ── DIAGNOSTIC (remove after confirming raw PCM is sane) ── */
//     if (*accum_count == 0) {
//         int32_t mn = INT32_MAX, mx = INT32_MIN;
//         for (uint32_t i = 0; i < num_frames; i++) {
//             uint32_t raw = frames[i * NUM_CHANNELS + MIC_SLOT];
//             int32_t  pcm = extract_pcm(raw);
//             if (pcm < mn) mn = pcm;
//             if (pcm > mx) mx = pcm;
//         }
//         /* raw word 0 so we can see bit layout */
//         uint32_t raw0 = frames[MIC_SLOT];
//         printk("[SND] raw0=0x%08X  pcm0=%d  min=%d  max=%d  range=%d\n",
//                raw0, extract_pcm(raw0), mn, mx, mx - mn);
//         /*
//          * Healthy mic in a quiet room:  range ~ 2000–20000
//          * DC offset / no signal:        range < 100  (all values identical)
//          * Clipping:                     min near -131072, max near +131071
//          * Wrong slot/shift:             raw0 looks like 0x00XXXXXX or 0xFFXXXXXX
//          */
//     }
//     /* ── END DIAGNOSTIC ── */

//     double sum_sq = 0.0;

//     for (uint32_t i = 0; i < num_frames; i++) {
//         float s = dc_block((float)extract_pcm(
//                 frames[i * NUM_CHANNELS + MIC_SLOT]));
//         s_windowed[i] = s;
//         sum_sq += (double)(s * s);
//     }

//     *rms_accum += sqrt(sum_sq / (double)num_frames);

//     for (uint32_t i = 0; i < num_frames; i++) {
//         s_windowed[i] *= s_hann[i];
//     }

//     for (uint32_t i = 0; i < FFT_SIZE; i++) {
//         s_fft_re[i] = (i < num_frames) ? s_windowed[i] : 0.0f;
//         s_fft_im[i] = 0.0f;
//     }
//     fft_compute();

//     float norm = (float)(FFT_SIZE / 2);
//     for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
//         float mag = sqrtf(s_fft_re[k] * s_fft_re[k]
//                           + s_fft_im[k] * s_fft_im[k]) / norm;
//         s_bin_accum[k] += mag;
//     }
//     (*accum_count)++;

//     if (*accum_count < WINDOWS_PER_SEC) {
//         return;
//     }

//     /* Average */
//     float    peak_mag  = 0.0f;
//     uint32_t peak_bin  = SOUND_BIN_LOW;

//     for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
//         s_bin_accum[k] /= (float)(*accum_count);
//         if (s_bin_accum[k] > peak_mag) {
//             peak_mag = s_bin_accum[k];
//             peak_bin = k;
//         }
//     }

//     double avg_rms  = *rms_accum / (double)(*accum_count);
//     double avg_dbfs = (avg_rms > 0.5)
//                       ? 20.0 * log10(avg_rms / PCM_FULL_SCALE)
//                       : -120.0;

//     publish(avg_dbfs, (float)peak_bin * FREQ_RES, peak_mag);
    

//     /* Reset */
//     for (uint32_t k = 0; k < FFT_SIZE / 2; k++) {
//         s_bin_accum[k] = 0.0f;
//     }
//     *rms_accum   = 0.0;
//     *accum_count = 0;
// }

// /* ── Sound thread ───────────────────────────────────────── */
// void sound_thread(void)
// {
//     const struct device *i2s_rx = DEVICE_DT_GET(I2S_RX_NODE);
//     const struct device *i2s_tx = DEVICE_DT_GET(I2S_TX_NODE);

//     init_hann();

//     if (!device_is_ready(i2s_rx)) {
//         LOG_ERR("I2S device not ready");
//         return;
//     }

//     struct i2s_config cfg = {
//         .word_size      = SAMPLE_BIT_WIDTH,
//         .channels       = NUM_CHANNELS,
//         .format         = I2S_FMT_DATA_FORMAT_I2S,
//         .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
//         .frame_clk_freq = SAMPLE_RATE,
//         .mem_slab       = &snd_tx_slab,
//         .block_size     = BUF_SIZE,
//         .timeout        = TIMEOUT_MS,
//     };

//     if (i2s_configure(i2s_tx, I2S_DIR_TX, &cfg) < 0) {
//         LOG_ERR("TX configure failed"); return;
//     }

//     cfg.mem_slab = &snd_rx_slab;
//     if (i2s_configure(i2s_rx, I2S_DIR_RX, &cfg) < 0) {
//         LOG_ERR("RX configure failed"); return;
//     }

//     i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);

//     if (i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
//         LOG_ERR("TX START failed"); return;
//     }

//     k_sleep(K_MSEC(100));   /* SPH0645 PLL lock */

//     if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
//         LOG_ERR("RX START failed"); return;
//     }

//     LOG_INF("Sound thread running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34");

//     double   rms_accum   = 0.0;
//     uint32_t accum_count = 0;

//     while (1) {
//         /* Keep TX alive every iteration — one silence buf per RX buf consumed */
//         // void *tx_mem;
//         // if (k_mem_slab_alloc(&snd_tx_slab, &tx_mem, K_NO_WAIT) == 0) {
//         //     memset(tx_mem, 0, BUF_SIZE);
//         //     if (i2s_write(i2s_tx, tx_mem, BUF_SIZE) < 0) {
//         //         k_mem_slab_free(&snd_tx_slab, tx_mem);
//         //     }
//         // }
//         void    *rx_mem;
//         uint32_t rx_size;

//         int ret = i2s_read(i2s_rx, &rx_mem, (size_t *)&rx_size);

//         if (ret == -EIO) {
//             LOG_WRN("I2S overrun — full restart");
//             i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
//             i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
//             k_sleep(K_MSEC(500));   /* full PLL re-lock time */
//             i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
//             i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
//             k_sleep(K_MSEC(50));
//             i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START);
//             accum_count = 0;
//             rms_accum   = 0.0;
//             continue;
//         }

//         if (ret < 0) {
//             LOG_ERR("i2s_read error: %d", ret);
//             continue;
//         }

//         size_t num_frames = rx_size / (NUM_CHANNELS * BYTES_PER_SAMPLE);
//         process_window((uint32_t *)rx_mem, num_frames,
//                        &rms_accum, &accum_count);

//         k_mem_slab_free(&snd_rx_slab, rx_mem);
//     }
// }

// /**
//  * @file sound.c
//  * @brief SPH0645 I2S microphone FFT analyser thread.
//  *
//  * Reads I2S audio, computes 1024-point FFT, averages ~43 windows/sec,
//  * then publishes two messages:
//  *
//  *   sound_q      — compact summary (rms, peak_freq, peak_mag)
//  *   sound_spec_q — full 348-bin spectrum for BLE chunked characteristic
//  *
//  * Hardware loopback: GPIO27 TX→RX in overlay keeps BCLK alive.
//  */

// #include "sound.h"

// #include <zephyr/kernel.h>
// #include <zephyr/device.h>
// #include <zephyr/drivers/i2s.h>
// #include <zephyr/logging/log.h>
// #include <string.h>
// #include <math.h>
// #include <stdbool.h>

// LOG_MODULE_REGISTER(sound, LOG_LEVEL_INF);

// #define M_PI 3.14159265358979323846f

// /* ── Node labels ────────────────────────────────────────── */
// #if DT_NODE_EXISTS(DT_NODELABEL(i2s_rxtx))
// #define I2S_RX_NODE  DT_NODELABEL(i2s_rxtx)
// #define I2S_TX_NODE  I2S_RX_NODE
// #else
// #define I2S_RX_NODE  DT_NODELABEL(i2s_rx)
// #define I2S_TX_NODE  DT_NODELABEL(i2s_tx)
// #endif

// /* ── Audio config ───────────────────────────────────────── */
// #define SAMPLE_RATE        44100U
// #define SAMPLE_BIT_WIDTH   32U
// #define NUM_CHANNELS       2U
// #define FFT_SIZE           1024U
// #define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
// #define BUF_SIZE           (FFT_SIZE * NUM_CHANNELS * BYTES_PER_SAMPLE)
// #define NUM_RX_BUFS        4U
// #define NUM_TX_BUFS        1U
// #define TIMEOUT_MS         2000
// #define MIC_SLOT           0U   /* SEL=GND → left channel */
// #define FREQ_RES           ((float)SAMPLE_RATE / (float)FFT_SIZE)
// #define WINDOWS_PER_SEC    43U

// /* ── Memory slabs ───────────────────────────────────────── */
// K_MEM_SLAB_DEFINE_STATIC(snd_rx_slab, BUF_SIZE, NUM_RX_BUFS, 4);
// K_MEM_SLAB_DEFINE_STATIC(snd_tx_slab, BUF_SIZE, NUM_TX_BUFS, 4);

// /* ── Message queues ─────────────────────────────────────── */
// K_MSGQ_DEFINE(sound_q,      sizeof(struct sound_msg),      SOUND_Q_DEPTH, 4);
// K_MSGQ_DEFINE(sound_spec_q, sizeof(struct sound_spec_msg), SOUND_Q_DEPTH, 4);

// /* ── Static working buffers ─────────────────────────────── */
// static float   s_windowed[FFT_SIZE];
// static float   s_fft_re[FFT_SIZE];
// static float   s_fft_im[FFT_SIZE];
// static float   s_hann[FFT_SIZE];
// static float   s_bin_accum[FFT_SIZE / 2];
// static uint8_t s_tx_silence[BUF_SIZE];   /* BSS zero — primes TX once */

// /* ── DC blocking filter ─────────────────────────────────── */
// static float dc_block(float x)
// {
//     static float x1 = 0.0f, y1 = 0.0f;
//     float y = x - x1 + 0.9975f * y1;
//     x1 = x; y1 = y;
//     return y;
// }

// /* ── PCM extraction ─────────────────────────────────────── */
// /*
//  * g_pcm_shift is set by detect_and_set_shift() after RX starts.
//  * The ESP32 I2S DMA captures at an arbitrary WS phase each boot,
//  * placing audio data at different bit positions and potentially
//  * swapping left/right channels. detect_and_set_shift() measures
//  * which slot has the signal and verifies it is correctly
//  * left-justified before accepting the boot as valid.
//  *
//  * On a good boot: shift=14, full_scale=131072 (SPH0645 18-bit spec).
//  * On a bad boot: returns false → startup retry loop re-triggers I2S.
//  */
// static uint8_t g_pcm_shift  = 14;
// static double  g_full_scale = 131072.0;
// static uint8_t g_mic_slot   = MIC_SLOT;  /* 0=left or 1=right, detected at runtime */
// static bool    g_bad_lock   = false;     /* set by process_window on WS interleave detection */

// static inline int32_t extract_pcm(uint32_t raw)
// {
//     return (int32_t)raw >> g_pcm_shift;
// }

// /* ── Auto-detect bit shift and channel slot ─────────────── */
// /*
//  * The ESP32 I2S DMA starts at an arbitrary WS phase each boot,
//  * causing two problems:
//  *   1. Bit shift  — audio data lands at different bit positions
//  *   2. Slot swap  — left/right channels may be swapped
//  *
//  * Three failure modes detected and rejected (→ retry):
//  *   A. best_msb < 16  — no real signal at all (zeros boot)
//  *   B. best_msb < 31  — data not left-justified, sign bit wrong (boots 2 & 4)
//  *   C. weak_msb >= 8  — both slots active, WS boundary interleave (boots 3 & 5)
//  *
//  * On success:
//  *   g_mic_slot  = which slot has the real mic signal (0 or 1)
//  *   g_pcm_shift = right-shift to extract signed 18-bit value
//  *   g_full_scale = 2^17 = 131072 (SPH0645 18-bit signed full scale)
//  *
//  * Returns true if locked, false if caller should retry.
//  */
// static bool detect_and_set_shift(const struct device *i2s_rx)
// {
//     void    *mem;
//     uint32_t sz;

//     /* Drain 3 buffers — let DMA and SPH0645 settle */
//     for (int i = 0; i < 3; i++) {
//         if (i2s_read(i2s_rx, &mem, (size_t *)&sz) == 0) {
//             k_mem_slab_free(&snd_rx_slab, mem);
//         }
//     }

//     /* Read one buffer, OR both slots to locate the signal */
//     if (i2s_read(i2s_rx, &mem, (size_t *)&sz) != 0) {
//         LOG_WRN("Shift detect: read failed — retrying");
//         return false;
//     }

//     uint32_t *frames = (uint32_t *)mem;
//     size_t    n      = sz / (NUM_CHANNELS * BYTES_PER_SAMPLE);
//     uint32_t  or_s0  = 0;
//     uint32_t  or_s1  = 0;

//     for (size_t i = 0; i < n; i++) {
//         or_s0 |= frames[i * NUM_CHANNELS + 0];
//         or_s1 |= frames[i * NUM_CHANNELS + 1];
//     }

//     k_mem_slab_free(&snd_rx_slab, mem);

//     /* Find MSB position for each slot */
//     int msb0 = 31, msb1 = 31;
//     while (msb0 > 0 && !(or_s0 & (1U << msb0))) { msb0--; }
//     while (msb1 > 0 && !(or_s1 & (1U << msb1))) { msb1--; }

//     int     best_msb  = (msb0 >= msb1) ? msb0 : msb1;
//     int     weak_msb  = (msb0 >= msb1) ? msb1 : msb0;
//     uint8_t best_slot = (msb0 >= msb1) ? 0    : 1;

//     /* Failure A: no real signal */
//     if (best_msb < 16) {
//         LOG_WRN("Shift detect: no signal (msb0=%d msb1=%d) — retrying",
//                 msb0, msb1);
//         return false;
//     }

//     /* Failure B: data not left-justified — sign bit at wrong position.
//      * Seen as large positive pcm values (~63000) instead of ±small DC. */
//     if (best_msb < 31) {
//         LOG_WRN("Shift detect: not left-justified (msb0=%d msb1=%d) — retrying",
//                 msb0, msb1);
//         return false;
//     }

//     /* Failure C: WS boundary interleave — both slots appear active.
//      * Seen as alternating 0x80000000/0x00000000, -6 dBFS noise floor.
//      * Tightened to < 8: values of 9-15 in the weak slot still indicate
//      * interleave bleed — a truly silent channel has msb near 0. */
//     if (weak_msb >= 8) {
//         LOG_WRN("Shift detect: slots not separated (msb0=%d msb1=%d) — retrying",
//                 msb0, msb1);
//         return false;
//     }

//     /*
//      * Good boot: best_msb=31, weak_msb<16.
//      * SPH0645 18-bit value is left-justified in bits [31:14].
//      * Right-shift by 14 to get signed 18-bit, full scale = 2^17 = 131072.
//      */
//     g_mic_slot   = best_slot;
//     g_pcm_shift  = 14;
//     g_full_scale = 131072.0;

//     LOG_INF("I2S detect: slot=%u  shift=%u  (slot0_msb=%d slot1_msb=%d)",
//             g_mic_slot, g_pcm_shift, msb0, msb1);
//     return true;
// }

// /* ── Hann window ────────────────────────────────────────── */
// static void init_hann(void)
// {
//     for (uint32_t i = 0; i < FFT_SIZE; i++) {
//         s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i
//                                          / (float)(FFT_SIZE - 1)));
//     }
// }

// /* ── Cooley-Tukey FFT ───────────────────────────────────── */
// static void fft_compute(void)
// {
//     uint32_t n = FFT_SIZE;
//     uint32_t j = 0;

//     for (uint32_t i = 1; i < n; i++) {
//         uint32_t bit = n >> 1;
//         for (; j & bit; bit >>= 1) { j ^= bit; }
//         j ^= bit;
//         if (i < j) {
//             float tmp;
//             tmp = s_fft_re[i]; s_fft_re[i] = s_fft_re[j]; s_fft_re[j] = tmp;
//             tmp = s_fft_im[i]; s_fft_im[i] = s_fft_im[j]; s_fft_im[j] = tmp;
//         }
//     }

//     for (uint32_t len = 2; len <= n; len <<= 1) {
//         float ang = -2.0f * M_PI / (float)len;
//         float wRe = cosf(ang), wIm = sinf(ang);
//         for (uint32_t i = 0; i < n; i += len) {
//             float curRe = 1.0f, curIm = 0.0f;
//             for (uint32_t k = 0; k < len / 2; k++) {
//                 uint32_t u = i + k, v = i + k + len / 2;
//                 float tRe = curRe * s_fft_re[v] - curIm * s_fft_im[v];
//                 float tIm = curRe * s_fft_im[v] + curIm * s_fft_re[v];
//                 s_fft_re[v] = s_fft_re[u] - tRe;
//                 s_fft_im[v] = s_fft_im[u] - tIm;
//                 s_fft_re[u] += tRe;
//                 s_fft_im[u] += tIm;
//                 float newRe = curRe * wRe - curIm * wIm;
//                 curIm       = curRe * wIm + curIm * wRe;
//                 curRe       = newRe;
//             }
//         }
//     }
// }

// /* ── Publish both queues ────────────────────────────────── */
// static void publish(double avg_dbfs, float peak_freq, float peak_mag)
// {
//     /* 1. Compact summary → sound_q */
//     struct sound_msg summary = {
//         .rms_dbfs_x100 = (int16_t)(avg_dbfs  * 100.0),
//         .peak_freq_hz  = (uint16_t)peak_freq,
//         .peak_mag_x10  = (uint16_t)(peak_mag * 10.0f),
//     };
//     if (k_msgq_put(&sound_q, &summary, K_NO_WAIT) != 0) {
//         struct sound_msg dump;
//         k_msgq_get(&sound_q, &dump, K_NO_WAIT);
//         k_msgq_put(&sound_q, &summary, K_NO_WAIT);
//     }

//     /* 2. Full spectrum → sound_spec_q
//      *
//      * bins[] are magnitude × 10 as uint16_t.
//      * Bins outside BIN_LOW–BIN_HIGH are already 0 from BSS/reset.
//      * sound_ble.c will chunk this into 3 BLE notifications.
//      */
//     struct sound_spec_msg spec;
//     spec.timestamp_ms  = k_uptime_get_32();
//     spec.rms_dbfs_x100 = summary.rms_dbfs_x100;

//     for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
//         uint32_t k = SOUND_BIN_LOW + i;
//         /* s_bin_accum[k] is already averaged before publish() is called */
//         float v = s_bin_accum[k] * 10.0f;
//         spec.bins[i] = (v > 65535.0f) ? 65535U : (uint16_t)v;
//     }

//     if (k_msgq_put(&sound_spec_q, &spec, K_NO_WAIT) != 0) {
//         struct sound_spec_msg dump;
//         k_msgq_get(&sound_spec_q, &dump, K_NO_WAIT);
//         k_msgq_put(&sound_spec_q, &spec, K_NO_WAIT);
//     }

//     LOG_INF("Sound: %.1f dBFS  Peak %.0f Hz  (%.1f)",
//             avg_dbfs, (double)peak_freq, (double)peak_mag);
// }

// /* ── Process one window ─────────────────────────────────── */
// static void process_window(uint32_t *frames, size_t num_frames,
//                            double *rms_accum, uint32_t *accum_count)
// {
//     /* ── Diagnostic: print raw word + PCM range once per averaging window ── */
//     if (*accum_count == 0) {
//         int32_t mn = INT32_MAX, mx = INT32_MIN;
//         for (uint32_t i = 0; i < num_frames; i++) {
//             int32_t pcm = extract_pcm(frames[i * NUM_CHANNELS + g_mic_slot]);
//             if (pcm < mn) mn = pcm;
//             if (pcm > mx) mx = pcm;
//         }
//         uint32_t raw0 = frames[g_mic_slot];
//         printk("[SND] raw0=0x%08X  pcm0=%d  min=%d  max=%d  range=%d\n",
//                raw0, extract_pcm(raw0), mn, mx, mx - mn);

//         /*
//          * WS boundary interleave detection — 2-bit and 1-bit offset cases.
//          * Interleave patterns produce range ≥ 196608 (2-bit) or 261120 (1-bit).
//          * Real loud audio peaks around 80000. Threshold at 100000 catches all
//          * interleave variants while leaving headroom for loud real signals.
//          */
//         if ((mx - mn) > 100000) {
//             LOG_WRN("Bad lock: WS interleave detected (range=%d) — restarting",
//                     mx - mn);
//             g_bad_lock = true;
//         }
//     }

//     double sum_sq = 0.0;

//     for (uint32_t i = 0; i < num_frames; i++) {
//         float s = dc_block((float)extract_pcm(
//                 frames[i * NUM_CHANNELS + g_mic_slot]));
//         s_windowed[i] = s;
//         sum_sq += (double)(s * s);
//     }

//     *rms_accum += sqrt(sum_sq / (double)num_frames);

//     for (uint32_t i = 0; i < num_frames; i++) {
//         s_windowed[i] *= s_hann[i];
//     }

//     for (uint32_t i = 0; i < FFT_SIZE; i++) {
//         s_fft_re[i] = (i < num_frames) ? s_windowed[i] : 0.0f;
//         s_fft_im[i] = 0.0f;
//     }
//     fft_compute();

//     float norm = (float)(FFT_SIZE / 2);
//     for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
//         float mag = sqrtf(s_fft_re[k] * s_fft_re[k]
//                           + s_fft_im[k] * s_fft_im[k]) / norm;
//         s_bin_accum[k] += mag;
//     }
//     (*accum_count)++;

//     if (*accum_count < WINDOWS_PER_SEC) {
//         return;
//     }

//     /* Average */
//     float    peak_mag  = 0.0f;
//     uint32_t peak_bin  = SOUND_BIN_LOW;

//     for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
//         s_bin_accum[k] /= (float)(*accum_count);
//         if (s_bin_accum[k] > peak_mag) {
//             peak_mag = s_bin_accum[k];
//             peak_bin = k;
//         }
//     }

//     double avg_rms  = *rms_accum / (double)(*accum_count);
//     double avg_dbfs = (avg_rms > 0.5)
//                       ? 20.0 * log10(avg_rms / g_full_scale)
//                       : -120.0;

//     publish(avg_dbfs, (float)peak_bin * FREQ_RES, peak_mag);

//     /* Reset */
//     for (uint32_t k = 0; k < FFT_SIZE / 2; k++) {
//         s_bin_accum[k] = 0.0f;
//     }
//     *rms_accum   = 0.0;
//     *accum_count = 0;
// }

// /* ── Sound thread ───────────────────────────────────────── */
// void sound_thread(void)
// {
//     const struct device *i2s_rx = DEVICE_DT_GET(I2S_RX_NODE);
//     const struct device *i2s_tx = DEVICE_DT_GET(I2S_TX_NODE);

//     init_hann();

//     if (!device_is_ready(i2s_rx)) {
//         LOG_ERR("I2S device not ready");
//         return;
//     }

//     struct i2s_config cfg = {
//         .word_size      = SAMPLE_BIT_WIDTH,
//         .channels       = NUM_CHANNELS,
//         .format         = I2S_FMT_DATA_FORMAT_I2S,
//         .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
//         .frame_clk_freq = SAMPLE_RATE,
//         .mem_slab       = &snd_tx_slab,
//         .block_size     = BUF_SIZE,
//         .timeout        = TIMEOUT_MS,
//     };

//     if (i2s_configure(i2s_tx, I2S_DIR_TX, &cfg) < 0) {
//         LOG_ERR("TX configure failed"); return;
//     }

//     cfg.mem_slab = &snd_rx_slab;
//     if (i2s_configure(i2s_rx, I2S_DIR_RX, &cfg) < 0) {
//         LOG_ERR("RX configure failed"); return;
//     }

//     i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);

//     if (i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
//         LOG_ERR("TX START failed"); return;
//     }

//     /* ── Startup retry loop ─────────────────────────────────
//      * The ESP32 I2S DMA starts at an arbitrary WS phase each boot.
//      * If detect_and_set_shift() sees no valid signal (bad alignment),
//      * we DROP and restart TX+RX until the DMA locks correctly.
//      * Typically succeeds on the first or second attempt.
//      */
//     for (int attempt = 1; ; attempt++) {
//         /* In the retry loop, replace fixed delay with jittered delay */
//         k_sleep(K_MSEC(100 + (attempt % 7) * 11));
//         // k_sleep(K_MSEC(100));  /* SPH0645 PLL lock (spec: 50ms max) */

//         if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
//             LOG_ERR("RX START failed"); return;
//         }

//         if (detect_and_set_shift(i2s_rx)) {
//             LOG_INF("I2S locked on attempt %d", attempt);
//             break;
//         }

//         LOG_WRN("I2S bad boot (attempt %d) — retrying", attempt);
//         i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
//         i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
//         k_sleep(K_MSEC(50));
//         i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
//         i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
//     }

//     LOG_INF("Sound thread running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34");

//     double   rms_accum   = 0.0;
//     uint32_t accum_count = 0;

//     while (1) {
//         void    *rx_mem;
//         uint32_t rx_size;

//         int ret = i2s_read(i2s_rx, &rx_mem, (size_t *)&rx_size);

//         if (ret == -EIO) {
//             LOG_WRN("I2S overrun — full restart");
//             i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
//             i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
//             k_sleep(K_MSEC(100));
//             i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
//             i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
//             k_sleep(K_MSEC(100));
//             i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START);
//             detect_and_set_shift(i2s_rx);
//             accum_count = 0;
//             rms_accum   = 0.0;
//             continue;
//         }

//         if (ret < 0) {
//             LOG_ERR("i2s_read error: %d", ret);
//             continue;
//         }

//         size_t num_frames = rx_size / (NUM_CHANNELS * BYTES_PER_SAMPLE);
//         process_window((uint32_t *)rx_mem, num_frames,
//                        &rms_accum, &accum_count);

//         k_mem_slab_free(&snd_rx_slab, rx_mem);

//         /* WS interleave detected by process_window — re-enter startup retry loop */
//         if (g_bad_lock) {
//             g_bad_lock = false;
//             LOG_WRN("Bad lock restart — re-entering startup retry loop");
//             i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
//             i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
//             k_sleep(K_MSEC(50));
//             i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
//             i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);

//             for (int attempt = 1; ; attempt++) {
//                 k_sleep(K_MSEC(100));
//                 if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
//                     LOG_ERR("RX START failed in bad lock recovery"); break;
//                 }
//                 if (detect_and_set_shift(i2s_rx)) {
//                     LOG_INF("Bad lock recovered on attempt %d", attempt);
//                     break;
//                 }
//                 LOG_WRN("Bad lock retry (attempt %d)", attempt);
//                 i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
//                 i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
//                 k_sleep(K_MSEC(50));
//                 i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
//                 i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
//             }
//             accum_count = 0;
//             rms_accum   = 0.0;
//         }
//     }
// }


/**
 * @file sound.c
 * @brief SPH0645 I2S microphone FFT analyser thread.
 *
 * Reads I2S audio, computes 1024-point FFT, averages ~43 windows/sec,
 * then publishes two messages:
 *
 *   sound_q      — compact summary (rms, peak_freq, peak_mag)
 *   sound_spec_q — full 348-bin spectrum for BLE chunked characteristic
 *
 * Hardware loopback: GPIO27 TX→RX in overlay keeps BCLK alive.
 */

#include "sound.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(sound, LOG_LEVEL_INF);

#define M_PI 3.14159265358979323846f

/* ── Node labels ────────────────────────────────────────── */
#if DT_NODE_EXISTS(DT_NODELABEL(i2s_rxtx))
#define I2S_RX_NODE  DT_NODELABEL(i2s_rxtx)
#define I2S_TX_NODE  I2S_RX_NODE
#else
#define I2S_RX_NODE  DT_NODELABEL(i2s_rx)
#define I2S_TX_NODE  DT_NODELABEL(i2s_tx)
#endif

/* ── Audio config ───────────────────────────────────────── */
#define SAMPLE_RATE        44100U
#define SAMPLE_BIT_WIDTH   32U
#define NUM_CHANNELS       2U
#define FFT_SIZE           1024U
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
#define BUF_SIZE           (FFT_SIZE * NUM_CHANNELS * BYTES_PER_SAMPLE)
#define NUM_RX_BUFS        4U
#define NUM_TX_BUFS        1U
#define TIMEOUT_MS         2000
#define MIC_SLOT           0U   /* SEL=GND → left channel */
#define FREQ_RES           ((float)SAMPLE_RATE / (float)FFT_SIZE)
#define WINDOWS_PER_SEC    43U

/* ── Memory slabs ───────────────────────────────────────── */
K_MEM_SLAB_DEFINE_STATIC(snd_rx_slab, BUF_SIZE, NUM_RX_BUFS, 4);
K_MEM_SLAB_DEFINE_STATIC(snd_tx_slab, BUF_SIZE, NUM_TX_BUFS, 4);

/* ── Message queues ─────────────────────────────────────── */
K_MSGQ_DEFINE(sound_q,      sizeof(struct sound_msg),      SOUND_Q_DEPTH, 4);
K_MSGQ_DEFINE(sound_spec_q, sizeof(struct sound_spec_msg), SOUND_Q_DEPTH, 4);

/* ── Static working buffers ─────────────────────────────── */
static float   s_windowed[FFT_SIZE];
static float   s_fft_re[FFT_SIZE];
static float   s_fft_im[FFT_SIZE];
static float   s_hann[FFT_SIZE];
static float   s_bin_accum[FFT_SIZE / 2];
static uint8_t s_tx_silence[BUF_SIZE];   /* BSS zero — primes TX once */

/* ── DC blocking filter ─────────────────────────────────── */
static float dc_block(float x) {

    static float x1 = 0.0f, y1 = 0.0f;
    float y = x - x1 + 0.9975f * y1;
    x1 = x; y1 = y;
    return y;
}

/* ── PCM extraction ─────────────────────────────────────── */
/*
 * g_pcm_shift is set by detect_and_set_shift() after RX starts.
 * The ESP32 I2S DMA captures at an arbitrary WS phase each boot,
 * placing audio data at different bit positions and potentially
 * swapping left/right channels. detect_and_set_shift() measures
 * which slot has the signal and verifies it is correctly
 * left-justified before accepting the boot as valid.
 *
 * On a good boot: shift=14, full_scale=131072 (SPH0645 18-bit spec).
 * On a bad boot: returns false → startup retry loop re-triggers I2S.
 */
static uint8_t g_pcm_shift  = 14;
static double  g_full_scale = 131072.0;
static uint8_t g_mic_slot   = MIC_SLOT;  /* 0=left or 1=right, detected at runtime */

static inline int32_t extract_pcm(uint32_t raw) {

    return (int32_t)raw >> g_pcm_shift;
}

/* ── Auto-detect bit shift and channel slot ─────────────── */
/*
 * The ESP32 I2S DMA starts at an arbitrary WS phase each boot,
 * causing two problems:
 *   1. Bit shift  — audio data lands at different bit positions
 *   2. Slot swap  — left/right channels may be swapped
 *
 * Three failure modes detected and rejected (→ retry):
 *   A. best_msb < 16  — no real signal at all (zeros boot)
 *   B. best_msb < 31  — data not left-justified, sign bit wrong
 *   C. weak_msb >= 8  — both slots active, WS boundary interleave
 *
 * On success:
 *   g_mic_slot  = which slot has the real mic signal (0 or 1)
 *   g_pcm_shift = 14 (SPH0645 18-bit left-justified)
 *   g_full_scale = 131072 (2^17, 18-bit signed full scale)
 *
 * Returns true if locked, false if caller should retry.
 */
static bool detect_and_set_shift(const struct device *i2s_rx) {
    void    *mem;
    uint32_t sz;

    /* Drain 3 buffers — let DMA and SPH0645 settle */
    for (int i = 0; i < 3; i++) {
        if (i2s_read(i2s_rx, &mem, (size_t *)&sz) == 0) {
            k_mem_slab_free(&snd_rx_slab, mem);
        }
    }

    /* Read one buffer, OR both slots to locate the signal */
    if (i2s_read(i2s_rx, &mem, (size_t *)&sz) != 0) {
        LOG_WRN("Shift detect: read failed — retrying");
        return false;
    }

    uint32_t *frames = (uint32_t *)mem;
    size_t    n      = sz / (NUM_CHANNELS * BYTES_PER_SAMPLE);
    uint32_t  or_s0  = 0;
    uint32_t  or_s1  = 0;

    for (size_t i = 0; i < n; i++) {
        or_s0 |= frames[i * NUM_CHANNELS + 0];
        or_s1 |= frames[i * NUM_CHANNELS + 1];
    }

    k_mem_slab_free(&snd_rx_slab, mem);

    /* Find MSB position for each slot */
    int msb0 = 31, msb1 = 31;
    while (msb0 > 0 && !(or_s0 & (1U << msb0))) { msb0--; }
    while (msb1 > 0 && !(or_s1 & (1U << msb1))) { msb1--; }

    int     best_msb  = (msb0 >= msb1) ? msb0 : msb1;
    int     weak_msb  = (msb0 >= msb1) ? msb1 : msb0;
    uint8_t best_slot = (msb0 >= msb1) ? 0    : 1;

    /* Failure A: no real signal */
    if (best_msb < 16) {
        LOG_WRN("Shift detect: no signal (msb0=%d msb1=%d) — retrying",
                msb0, msb1);
        return false;
    }

    /* Failure B: data not left-justified — sign bit at wrong position */
    if (best_msb < 31) {
        LOG_WRN("Shift detect: not left-justified (msb0=%d msb1=%d) — retrying",
                msb0, msb1);
        return false;
    }

    /* Failure C: WS boundary interleave — both slots appear active.
     * A truly silent channel has msb near 0. Values 8–15 indicate
     * interleave bleed from DMA landing on a WS boundary. */
    if (weak_msb >= 8) {
        LOG_WRN("Shift detect: slots not separated (msb0=%d msb1=%d) — retrying",
                msb0, msb1);
        return false;
    }

    /* Good boot — set globals */
    g_mic_slot   = best_slot;
    g_pcm_shift  = 14;
    g_full_scale = 131072.0;

    LOG_INF("I2S detect: slot=%u  shift=%u  (slot0_msb=%d slot1_msb=%d)",
            g_mic_slot, g_pcm_shift, msb0, msb1);
    return true;
}

/* ── Hann window ────────────────────────────────────────── */
static void init_hann(void) {

    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i
                                         / (float)(FFT_SIZE - 1)));
    }
}

/* ── Cooley-Tukey FFT ───────────────────────────────────── */
static void fft_compute(void) {

    uint32_t n = FFT_SIZE;
    uint32_t j = 0;

    for (uint32_t i = 1; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) {
            float tmp;
            tmp = s_fft_re[i]; s_fft_re[i] = s_fft_re[j]; s_fft_re[j] = tmp;
            tmp = s_fft_im[i]; s_fft_im[i] = s_fft_im[j]; s_fft_im[j] = tmp;
        }
    }

    for (uint32_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * M_PI / (float)len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (uint32_t i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (uint32_t k = 0; k < len / 2; k++) {
                uint32_t u = i + k, v = i + k + len / 2;
                float tRe = curRe * s_fft_re[v] - curIm * s_fft_im[v];
                float tIm = curRe * s_fft_im[v] + curIm * s_fft_re[v];
                s_fft_re[v] = s_fft_re[u] - tRe;
                s_fft_im[v] = s_fft_im[u] - tIm;
                s_fft_re[u] += tRe;
                s_fft_im[u] += tIm;
                float newRe = curRe * wRe - curIm * wIm;
                curIm       = curRe * wIm + curIm * wRe;
                curRe       = newRe;
            }
        }
    }
}

/* ── Publish both queues ────────────────────────────────── */
static void publish(double avg_dbfs, float peak_freq, float peak_mag) {
    /* 1. Compact summary → sound_q */
    struct sound_msg summary = {
        .rms_dbfs_x100 = (int16_t)(avg_dbfs  * 100.0),
        .peak_freq_hz  = (uint16_t)peak_freq,
        .peak_mag_x10  = (uint16_t)(peak_mag * 10.0f),
    };
    if (k_msgq_put(&sound_q, &summary, K_NO_WAIT) != 0) {
        struct sound_msg dump;
        k_msgq_get(&sound_q, &dump, K_NO_WAIT);
        k_msgq_put(&sound_q, &summary, K_NO_WAIT);
    }

    /* 2. Full spectrum → sound_spec_q */
    struct sound_spec_msg spec;
    spec.utc_sec       = time_sync_get_utc_ms(&spec.utc_ms);  /* stamp at measurement moment */
    spec.rms_dbfs_x100 = summary.rms_dbfs_x100;

    for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
        uint32_t k = SOUND_BIN_LOW + i;
        float v = s_bin_accum[k] * 10.0f;
        spec.bins[i] = (v > 65535.0f) ? 65535U : (uint16_t)v;
    }

    if (k_msgq_put(&sound_spec_q, &spec, K_NO_WAIT) != 0) {
        struct sound_spec_msg dump;
        k_msgq_get(&sound_spec_q, &dump, K_NO_WAIT);
        k_msgq_put(&sound_spec_q, &spec, K_NO_WAIT);
    }

    LOG_INF("Sound: %.1f dBFS  Peak %.0f Hz  (%.1f)",
            avg_dbfs, (double)peak_freq, (double)peak_mag);
}

/* ── Process one window ─────────────────────────────────── */
static void process_window(uint32_t *frames, size_t num_frames,
                           double *rms_accum, uint32_t *accum_count) {
    /* ── Diagnostic: print raw word + PCM range once per averaging window ── */
    if (*accum_count == 0) {
        int32_t mn = INT32_MAX, mx = INT32_MIN;
        for (uint32_t i = 0; i < num_frames; i++) {
            int32_t pcm = extract_pcm(frames[i * NUM_CHANNELS + g_mic_slot]);
            if (pcm < mn) mn = pcm;
            if (pcm > mx) mx = pcm;
        }
        uint32_t raw0 = frames[g_mic_slot];
        printk("[SND] raw0=0x%08X  pcm0=%d  min=%d  max=%d  range=%d\n",
               raw0, extract_pcm(raw0), mn, mx, mx - mn);
    }

    double sum_sq = 0.0;

    for (uint32_t i = 0; i < num_frames; i++) {
        float s = dc_block((float)extract_pcm(
                frames[i * NUM_CHANNELS + g_mic_slot]));
        s_windowed[i] = s;
        sum_sq += (double)(s * s);
    }

    *rms_accum += sqrt(sum_sq / (double)num_frames);

    for (uint32_t i = 0; i < num_frames; i++) {
        s_windowed[i] *= s_hann[i];
    }

    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_fft_re[i] = (i < num_frames) ? s_windowed[i] : 0.0f;
        s_fft_im[i] = 0.0f;
    }
    fft_compute();

    float norm = (float)(FFT_SIZE / 2);
    for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
        float mag = sqrtf(s_fft_re[k] * s_fft_re[k]
                          + s_fft_im[k] * s_fft_im[k]) / norm;
        s_bin_accum[k] += mag;
    }
    (*accum_count)++;

    if (*accum_count < WINDOWS_PER_SEC) {
        return;
    }

    /* Average */
    float    peak_mag  = 0.0f;
    uint32_t peak_bin  = SOUND_BIN_LOW;

    for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
        s_bin_accum[k] /= (float)(*accum_count);
        if (s_bin_accum[k] > peak_mag) {
            peak_mag = s_bin_accum[k];
            peak_bin = k;
        }
    }

    double avg_rms  = *rms_accum / (double)(*accum_count);
    double avg_dbfs = (avg_rms > 0.5)
                      ? 20.0 * log10(avg_rms / g_full_scale)
                      : -120.0;

    publish(avg_dbfs, (float)peak_bin * FREQ_RES, peak_mag);

    /* Reset */
    for (uint32_t k = 0; k < FFT_SIZE / 2; k++) {
        s_bin_accum[k] = 0.0f;
    }
    *rms_accum   = 0.0;
    *accum_count = 0;
}

/* ── Sound thread ───────────────────────────────────────── */
void sound_thread(void) {
    const struct device *i2s_rx = DEVICE_DT_GET(I2S_RX_NODE);
    const struct device *i2s_tx = DEVICE_DT_GET(I2S_TX_NODE);

    init_hann();

    if (!device_is_ready(i2s_rx)) {
        LOG_ERR("I2S device not ready");
        return;
    }

    struct i2s_config cfg = {
        .word_size      = SAMPLE_BIT_WIDTH,
        .channels       = NUM_CHANNELS,
        .format         = I2S_FMT_DATA_FORMAT_I2S,
        .options        = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = SAMPLE_RATE,
        .mem_slab       = &snd_tx_slab,
        .block_size     = BUF_SIZE,
        .timeout        = TIMEOUT_MS,
    };

    if (i2s_configure(i2s_tx, I2S_DIR_TX, &cfg) < 0) {
        LOG_ERR("TX configure failed"); return;
    }

    cfg.mem_slab = &snd_rx_slab;
    if (i2s_configure(i2s_rx, I2S_DIR_RX, &cfg) < 0) {
        LOG_ERR("RX configure failed"); return;
    }

    i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);

    if (i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
        LOG_ERR("TX START failed"); return;
    }

    /* ── Startup retry loop ─────────────────────────────────
     * The ESP32 I2S DMA starts at an arbitrary WS phase each boot.
     * Jittered delay breaks the deterministic phase cycle that causes
     * long retry sequences — each attempt lands at a different WS phase.
     * Typically locks within 1–4 attempts.
     */
    for (int attempt = 1; ; attempt++) {
        k_sleep(K_MSEC(100 + (attempt % 7) * 11));

        if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
            LOG_ERR("RX START failed"); return;
        }

        if (detect_and_set_shift(i2s_rx)) {
            LOG_INF("I2S locked on attempt %d", attempt);
            break;
        }

        LOG_WRN("I2S bad boot (attempt %d) — retrying", attempt);
        i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
        i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
        k_sleep(K_MSEC(50));
        i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
        i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
    }

    LOG_INF("Sound thread running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34");

    double   rms_accum   = 0.0;
    uint32_t accum_count = 0;

    while (1) {
        void    *rx_mem;
        uint32_t rx_size;

        int ret = i2s_read(i2s_rx, &rx_mem, (size_t *)&rx_size);

        if (ret == -EIO) {
            LOG_WRN("I2S overrun — full restart");
            i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
            i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
            k_sleep(K_MSEC(100));
            i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
            i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);

            for (int attempt = 1; ; attempt++) {
                k_sleep(K_MSEC(50 + (attempt % 7) * 11));
                if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
                    LOG_ERR("RX START failed in overrun recovery"); break;
                }
                if (detect_and_set_shift(i2s_rx)) {
                    LOG_INF("Overrun recovered on attempt %d", attempt);
                    break;
                }
                LOG_WRN("Overrun retry (attempt %d)", attempt);
                i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
                i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
                k_sleep(K_MSEC(50));
                i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
                i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
            }
            accum_count = 0;
            rms_accum   = 0.0;
            continue;
        }

        if (ret < 0) {
            LOG_ERR("i2s_read error: %d", ret);
            continue;
        }

        size_t num_frames = rx_size / (NUM_CHANNELS * BYTES_PER_SAMPLE);
        process_window((uint32_t *)rx_mem, num_frames,
                       &rms_accum, &accum_count);

        k_mem_slab_free(&snd_rx_slab, rx_mem);
    }
}