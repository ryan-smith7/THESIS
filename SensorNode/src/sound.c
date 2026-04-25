/**
 * @file sound.c
 * @brief SPH0645 I2S microphone FFT analyser thread.
 *
 * PIPELINE OVERVIEW
 * -----------------
 * Each call to process_window() handles one DMA buffer (1024 stereo frames):
 *
 *   Raw I2S DMA buffer
 *       │
 *       ▼
 *   [1] PCM extraction      — right-shift 32-bit I2S word → 18-bit signed int
 *       │
 *       ▼
 *   [2] DC blocking filter  — 1st-order IIR highpass, removes mic DC bias
 *       │
 *       ▼
 *   [3] RMS accumulation    — time-domain RMS computed PRE-window for the blue
 *       │                     dBFS panel; accumulated across 43 windows/sec
 *       ▼
 *   [4] Hann window         — reduce spectral leakage before FFT
 *       │
 *       ▼
 *   [5] 1024-point FFT      — Cooley-Tukey in-place radix-2 DIT
 *       │
 *       ▼
 *   [6] Magnitude + normalise — |X[k]| / (N/2 × FS), gives normalised peak amp
 *       │
 *       ▼
 *   [7] CG + peak→RMS corr  — ×√2 net factor: undoes Hann CG (×2) then
 *       │                     peak→RMS (÷√2), result is RMS-comparable amplitude
 *       ▼
 *   [8] Power accumulation  — square a_corr and accumulate; defer log to publish
 *       │
 *       ▼  (after 43 windows)
 *   [9] Average power → dBFS — mean linear power per bin, then 10·log10
 *       │
 *       ▼
 *  [10] publish()           — pack bins into uint16 BLE message, enqueue
 *
 * KEY DESIGN DECISIONS
 * --------------------
 * • Accumulate in LINEAR POWER space (step 8), convert to dB once (step 9).
 *   Averaging in log space underestimates peaks due to Jensen's inequality —
 *   near-zero windows drag the dB mean far below the true energy level.
 *
 * • ×√2 in step 7 is the NET of two corrections:
 *     CG correction:  ×(1/0.5) = ×2.0   (Hann coherent gain = 0.5)
 *     peak → RMS:     ÷√2               (sine: A_rms = A_peak/√2)
 *     net:            ×2/√2 = ×√2
 *   This makes FFT bin amplitudes directly comparable to the time-domain
 *   RMS dBFS computed in step 3.
 *
 * • 10·log10 (not 20·log10) is used at step 9 because s_bin_accum holds
 *   POWER (amplitude²). 10·log10(P) = 20·log10(A) — using 20·log10 on power
 *   would double-count the square and read 3 dB too low.
 *
 * Hardware loopback: GPIO27 TX→RX in overlay keeps BCLK alive so RX can
 * run without an external loopback cable.
 */

#include "sound.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "time_sync.h"

LOG_MODULE_REGISTER(sound, LOG_LEVEL_INF);

#define M_PI 3.14159265358979323846f

/* ── Node labels ────────────────────────────────────────────────────────────
 * Support both combined rxtx nodes (ESP32 I2S0/I2S1 with shared BCLK/WS)
 * and split rx/tx node configurations in the devicetree overlay.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(i2s_rxtx))
#define I2S_RX_NODE  DT_NODELABEL(i2s_rxtx)
#define I2S_TX_NODE  I2S_RX_NODE
#else
#define I2S_RX_NODE  DT_NODELABEL(i2s_rx)
#define I2S_TX_NODE  DT_NODELABEL(i2s_tx)
#endif

/* ── Audio config ───────────────────────────────────────────────────────────
 * SAMPLE_RATE   : 44100 Hz — standard audio rate; SPH0645 supports up to 64kHz
 * FFT_SIZE      : 1024 points → frequency resolution = 44100/1024 ≈ 43.07 Hz/bin
 * BUF_SIZE      : one DMA buffer = 1024 frames × 2 channels × 4 bytes = 8192 bytes
 * WINDOWS_PER_SEC: at 44100 Hz with 1024 samples/window ≈ 43 windows/sec
 *                 (44100/1024 = 43.07), so accumulating 43 windows ≈ 1 second
 */
#define SAMPLE_RATE        44100U
#define SAMPLE_BIT_WIDTH   32U
#define NUM_CHANNELS       2U
#define FFT_SIZE           1024U
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
#define BUF_SIZE           (FFT_SIZE * NUM_CHANNELS * BYTES_PER_SAMPLE)
#define NUM_RX_BUFS        4U   /* DMA ping-pong depth — reduces overrun risk */
#define NUM_TX_BUFS        1U
#define TIMEOUT_MS         2000
#define MIC_SLOT           0U   /* SPH0645 SEL=GND → left channel (slot 0) */
#define FREQ_RES           ((float)SAMPLE_RATE / (float)FFT_SIZE)   /* Hz per bin */
#define WINDOWS_PER_SEC    43U

/* ── Memory slabs ───────────────────────────────────────────────────────────
 * Zephyr I2S driver requires pre-allocated fixed-size memory slabs.
 * The driver takes ownership of a slab block on read, caller must free it.
 * RX has 4 buffers to absorb scheduling jitter; TX only needs 1 (silence).
 */
K_MEM_SLAB_DEFINE_STATIC(snd_rx_slab, BUF_SIZE, NUM_RX_BUFS, 4);
K_MEM_SLAB_DEFINE_STATIC(snd_tx_slab, BUF_SIZE, NUM_TX_BUFS, 4);

/* ── Message queues ─────────────────────────────────────────────────────────
 * snd_q carries one sound_spec_msg per second (after 43-window accumulation)
 * to the BLE thread for chunked characteristic transmission.
 */
K_MSGQ_DEFINE(snd_q, sizeof(struct sound_spec_msg), SOUND_Q_DEPTH, 4);

/* ── Static working buffers ─────────────────────────────────────────────────
 * All declared static to avoid stack allocation of large float arrays
 * inside the thread function — ESP32 thread stacks are limited.
 *
 * s_windowed   : time-domain samples after DC block, before/after Hann
 * s_fft_re/im  : FFT input/output (in-place Cooley-Tukey)
 * s_hann       : pre-computed Hann window coefficients
 * s_bin_accum  : per-bin LINEAR POWER accumulator across 43 windows;
 *                converted to dBFS at publish time (not before)
 * s_tx_silence : BSS zero — primes TX FIFO so BCLK stays alive for RX
 */
static float   s_windowed[FFT_SIZE];
static float   s_fft_re[FFT_SIZE];
static float   s_fft_im[FFT_SIZE];
static float   s_hann[FFT_SIZE];
static float   s_bin_accum[FFT_SIZE / 2];
static uint8_t s_tx_silence[BUF_SIZE];

/* ═══════════════════════════════════════════════════════════════════════════
 * STEP 2 — DC BLOCKING FILTER
 * ═══════════════════════════════════════════════════════════════════════════
 * First-order IIR highpass:  y[n] = x[n] - x[n-1] + α·y[n-1]
 *
 * With α = 0.9975 and fs = 44100 Hz:
 *   cutoff ≈ fs/(2π) · (1 - α) ≈ 17.6 Hz
 *
 * This removes the SPH0645's DC offset without affecting audio content.
 * double precision state variables prevent long-term accumulation error
 * that causes slow drift with float (was a known bug, now fixed).
 */
static float dc_block(float x) {
    static double x1 = 0.0, y1 = 0.0;
    double y = (double)x - x1 + 0.9975 * y1;
    x1 = (double)x;
    y1 = y;
    return (float)y;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STEP 1 — PCM EXTRACTION
 * ═══════════════════════════════════════════════════════════════════════════
 * The SPH0645 outputs 18-bit audio LEFT-JUSTIFIED in a 32-bit I2S word:
 *
 *   Bit 31 (MSB) ... Bit 14 : 18-bit signed audio data
 *   Bit 13       ... Bit  0 : always zero (padding)
 *
 * Right-shifting by g_pcm_shift (= 14) gives a signed 18-bit value in the
 * range [-131072, +131072] = [-2^17, +2^17], which is g_full_scale.
 *
 * g_pcm_shift and g_mic_slot are set at runtime by detect_and_set_shift()
 * because the ESP32 I2S DMA can start at an arbitrary WS phase, causing
 * the audio data to land in slot 0 or slot 1 unpredictably each boot.
 */
static uint8_t g_pcm_shift  = 14;
static double  g_full_scale = 131072.0;   /* 2^17 — 18-bit signed full scale */
static uint8_t g_mic_slot   = MIC_SLOT;

static inline int32_t extract_pcm(uint32_t raw) {
    return (int32_t)raw >> g_pcm_shift;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STARTUP — AUTO-DETECT BIT SHIFT AND CHANNEL SLOT
 * ═══════════════════════════════════════════════════════════════════════════
 * The ESP32 I2S DMA starts at an arbitrary WS phase each boot, causing:
 *   1. Bit shift  — audio data lands at different bit positions in the word
 *   2. Slot swap  — left/right channels may be swapped
 *
 * Strategy: OR all samples in each slot across one full DMA buffer.
 * The slot with the real mic signal will have its MSB at bit 31 (18-bit
 * data left-justified). The silent/dummy slot will have near-zero OR result.
 *
 * Three failure modes → retry:
 *   A. best_msb < 16  — no real signal (DMA returned all zeros)
 *   B. best_msb < 31  — data not left-justified (WS phase misalignment)
 *   C. weak_msb != 0  — silent slot has ANY bits set; even weak_msb=2/3/6
 *                        causes an elevated noise floor at runtime. Only a
 *                        perfectly clean separation (weak_msb == 0) is accepted.
 *
 * On success: g_mic_slot and g_pcm_shift are set for the rest of runtime.
 * Returns true if locked, false if the caller should retry with fresh I2S start.
 */
static bool detect_and_set_shift(const struct device *i2s_rx) {
    void    *mem;
    uint32_t sz;

    /* Drain 3 buffers to let DMA and SPH0645 settle after trigger */
    for (int i = 0; i < 3; i++) {
        if (i2s_read(i2s_rx, &mem, (size_t *)&sz) == 0) {
            k_mem_slab_free(&snd_rx_slab, mem);
        }
    }

    /* Read one buffer and OR both slots independently to find signal */
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

    /* Find the position of the highest set bit in each slot's OR result */
    int msb0 = 31, msb1 = 31;
    while (msb0 > 0 && !(or_s0 & (1U << msb0))) { msb0--; }
    while (msb1 > 0 && !(or_s1 & (1U << msb1))) { msb1--; }

    int     best_msb  = (msb0 >= msb1) ? msb0 : msb1;
    int     weak_msb  = (msb0 >= msb1) ? msb1 : msb0;
    uint8_t best_slot = (msb0 >= msb1) ? 0    : 1;

    if (best_msb < 16) {    /* Failure A */
        LOG_WRN("Shift detect: no signal (msb0=%d msb1=%d) — retrying", msb0, msb1);
        return false;
    }
    if (best_msb < 31) {    /* Failure B */
        LOG_WRN("Shift detect: not left-justified (msb0=%d msb1=%d) — retrying", msb0, msb1);
        return false;
    }
    if (weak_msb != 0) {    /* Failure C — require perfect separation: silent slot must be exactly zero */
        LOG_WRN("Shift detect: imperfect separation (msb0=%d msb1=%d) — retrying", msb0, msb1);
        return false;
    }

    g_mic_slot   = best_slot;
    g_pcm_shift  = 14;
    g_full_scale = 131072.0;

    LOG_INF("I2S detect: slot=%u  shift=%u  (slot0_msb=%d slot1_msb=%d)",
            g_mic_slot, g_pcm_shift, msb0, msb1);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STEP 4 — HANN WINDOW (pre-computed)
 * ═══════════════════════════════════════════════════════════════════════════
 * w[n] = 0.5 · (1 - cos(2π·n / (N-1)))   for n = 0..N-1
 *
 * Properties relevant to this pipeline:
 *   Coherent gain (CG) = 0.5   — a pure sine is attenuated to half amplitude
 *   Power gain     (PG) = 0.375
 *
 * Using (N-1) in the denominator gives a symmetric window that goes to zero
 * at both endpoints, minimising spectral leakage from signal discontinuities
 * at the buffer boundaries.
 *
 * Pre-computing once at startup avoids 1024 cosf() calls per window.
 */
static void init_hann(void) {
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (float)(FFT_SIZE - 1)));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STEP 5 — 1024-POINT FFT (Cooley-Tukey radix-2 DIT)
 * ═══════════════════════════════════════════════════════════════════════════
 * In-place split-radix FFT operating on s_fft_re[] (real) and s_fft_im[]
 * (imaginary). Input must be loaded before calling; output overwrites input.
 *
 * Phase 1 — Bit-reversal permutation:
 *   Reorders samples so that the butterfly stages operate on contiguous pairs.
 *   Standard Cooley-Tukey requires input in bit-reversed order.
 *
 * Phase 2 — Butterfly stages (log2(N) = 10 stages for N=1024):
 *   Each stage doubles the DFT length using the twiddle factor:
 *     W = e^{-j·2π/len}
 *   The butterfly: X[u] = X[u] + W^k·X[v]
 *                  X[v] = X[u] - W^k·X[v]
 *
 * Output X[k] for k = 0..511 gives the positive-frequency spectrum.
 * Bins k=512..1023 are the conjugate mirror and are not used.
 */
static void fft_compute(void) {
    uint32_t n = FFT_SIZE;
    uint32_t j = 0;

    /* Phase 1: bit-reversal permutation */
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

    /* Phase 2: butterfly stages */
    for (uint32_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * M_PI / (float)len;
        float wRe = cosf(ang), wIm = sinf(ang);   /* base twiddle factor */
        for (uint32_t i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;     /* running twiddle W^k */
            for (uint32_t k = 0; k < len / 2; k++) {
                uint32_t u = i + k, v = i + k + len / 2;
                float tRe = curRe * s_fft_re[v] - curIm * s_fft_im[v];
                float tIm = curRe * s_fft_im[v] + curIm * s_fft_re[v];
                s_fft_re[v] = s_fft_re[u] - tRe;
                s_fft_im[v] = s_fft_im[u] - tIm;
                s_fft_re[u] += tRe;
                s_fft_im[u] += tIm;
                /* Advance twiddle: W^{k+1} = W^k · W */
                float newRe = curRe * wRe - curIm * wIm;
                curIm       = curRe * wIm + curIm * wRe;
                curRe       = newRe;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STEP 10 — PUBLISH
 * ═══════════════════════════════════════════════════════════════════════════
 * At this point s_bin_accum[k] holds the mean dBFS per bin (set by the
 * averaging loop in process_window before publish() is called).
 *
 * Encoding for BLE transmission:
 *   dBFS range is clamped to [-120, 0].
 *   Mapped to uint16: v = (db + 120) × 100
 *   This gives 0 = silence (−120 dBFS) and 12000 = full scale (0 dBFS),
 *   with 0.01 dB resolution across the full range.
 *
 * If the queue is full the oldest message is discarded and replaced,
 * ensuring the BLE thread always sees the latest spectrum rather than
 * stalling on a full queue.
 */
static void publish(double avg_dbfs, float peak_freq, float peak_mag_db) {

    struct sound_spec_msg spec;
    spec.utc_sec       = time_sync_get_utc_ms(&spec.utc_ms);
    spec.rms_dbfs_x100 = (int16_t)(avg_dbfs * 100.0);   /* time-domain RMS dBFS ×100 */
    spec.uptime_ms     = (uint64_t)k_uptime_get();

    for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
        uint32_t k  = SOUND_BIN_LOW + i;
        float    db = s_bin_accum[k];   /* mean dBFS — set by averaging loop */

        db = fmaxf(-120.0f, fminf(0.0f, db));   /* clamp to valid dBFS range */

        if (i == 0) {
            LOG_INF("bin[0]: db=%.2f", (double)db);
        }

        /* Encode: shift into [0, 12000], scale ×100 for 0.01 dB resolution */
        int32_t v = (int32_t)((db + 120.0f) * 100.0f);

        if (v < 0)          { spec.bins[i] = 0;     }
        else if (v > 65535) { spec.bins[i] = 65535;  }
        else                { spec.bins[i] = (uint16_t)v; }
    }

    /* Drop oldest if full — never block the audio thread on BLE backpressure */
    if (k_msgq_put(&snd_q, &spec, K_NO_WAIT) != 0) {
        struct sound_spec_msg dump;
        k_msgq_get(&snd_q, &dump, K_NO_WAIT);
        k_msgq_put(&snd_q, &spec, K_NO_WAIT);
    }

    LOG_INF("Sound: %.1f dBFS  Peak %.0f Hz  (%.1f dBFS)",
            avg_dbfs, (double)peak_freq, (double)peak_mag_db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STEPS 2–9 — PROCESS ONE WINDOW
 * ═══════════════════════════════════════════════════════════════════════════
 * Called once per DMA buffer (every ~23.2 ms at 44100 Hz / 1024 samples).
 * Accumulates 43 windows before publishing (~1 second of audio per publish).
 *
 * ACCUMULATOR DESIGN NOTE:
 *   s_bin_accum[] holds LINEAR POWER (amplitude²), not dBFS.
 *   Converting to dB before averaging causes Jensen's inequality error:
 *     mean(log(x)) ≤ log(mean(x))   [log is concave]
 *   For a swept sine, bins where the tone is absent contribute near-zero
 *   power. In log space those zeros pull the average far below the true
 *   energy; in linear power space they are negligible against the windows
 *   where the tone was present. The spectrogram sweep arc is only visible
 *   with power-domain averaging.
 */
static void process_window(uint32_t *frames, size_t num_frames,
                           double *rms_accum, uint32_t *accum_count) {

    /* Diagnostic printk on the first window of each accumulation cycle */
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

    /* ── STEPS 2 & 3: DC block + time-domain RMS ───────────────────────────
     * DC block is applied first so the RMS measurement is not biased by the
     * mic's DC offset. RMS is computed PRE-Hann so it reflects the true
     * signal energy, not the windowed (attenuated) signal.
     *
     * sum_sq = Σ y[n]²   →   RMS_w = √(sum_sq / N)
     *
     * We accumulate RMS_w across windows and average at publish:
     *   avg_RMS = (1/W) · Σ RMS_w
     * Then convert to dBFS: 20·log10(avg_RMS / full_scale)
     *
     * Note: 20·log10 here because avg_RMS is an AMPLITUDE quantity.
     */
    double sum_sq = 0.0;
    for (uint32_t i = 0; i < num_frames; i++) {
        float s = dc_block((float)extract_pcm(
                frames[i * NUM_CHANNELS + g_mic_slot]));
        s_windowed[i] = s;
        sum_sq += (double)(s * s);
    }
    *rms_accum += sqrt(sum_sq / (double)num_frames);   /* accumulate per-window RMS */

    /* ── STEP 4: Hann window ────────────────────────────────────────────────
     * Multiply each sample by the pre-computed Hann coefficient.
     * This tapers the signal to zero at both ends of the buffer, preventing
     * the spectral leakage that would occur if the buffer boundaries cut
     * through a waveform cycle (Gibbs phenomenon).
     *
     * Side effect: attenuates the signal by the coherent gain (CG = 0.5).
     * This is corrected later in the magnitude step (×√2 net factor).
     */
    for (uint32_t i = 0; i < num_frames; i++) {
        s_windowed[i] *= s_hann[i];
    }

    /* ── STEP 5: FFT input load ─────────────────────────────────────────────
     * Copy windowed real samples into the FFT real array; zero the imaginary
     * part (real-valued input). Pad with zeros if num_frames < FFT_SIZE
     * (should not occur in normal operation — both are 1024).
     */
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_fft_re[i] = (i < num_frames) ? s_windowed[i] : 0.0f;
        s_fft_im[i] = 0.0f;
    }
    fft_compute();   /* in-place Cooley-Tukey, see fft_compute() above */

    /* ── STEPS 6, 7, 8: Normalise → correct → accumulate power ─────────────
     *
     * STEP 6 — Normalise FFT magnitude:
     *   norm = (N/2) × full_scale = 512 × 131072 = 67,108,864
     *   mag  = |X[k]| / norm
     *
     *   For a full-scale sine (A = 131072) at a bin-aligned frequency,
     *   the Hann-windowed FFT gives |X[k]| ≈ A × CG × (N/2) = 33,554,432
     *   so mag = 33,554,432 / 67,108,864 = 0.5  (normalised peak amplitude)
     *
     * STEP 7 — Coherent gain + peak→RMS correction:
     *   a_corr = mag × √2
     *
     *   This is the NET of two corrections applied simultaneously:
     *     • Undo Hann CG:  × (1/CG) = × 2.0
     *     • Peak → RMS:    ÷ √2
     *     • Net:           × 2/√2 = × √2 ≈ 1.4142
     *
     *   Result: a_corr is an RMS-normalised amplitude. A full-scale sine:
     *     a_corr = 0.5 × 1.4142 = 0.7071
     *     20·log10(0.7071) = −3.01 dBFS  ✓  (correct RMS of a full-scale sine)
     *
     * STEP 8 — Accumulate LINEAR POWER:
     *   s_bin_accum[k] += a_corr²
     *
     *   Power is accumulated (not dB) so that the averaging in step 9 is
     *   mathematically correct. See the ACCUMULATOR DESIGN NOTE above.
     */
    float norm = (float)(FFT_SIZE / 2) * (float)g_full_scale;   /* 512 × 131072 */
    for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
        float mag    = sqrtf(s_fft_re[k] * s_fft_re[k]
                           + s_fft_im[k] * s_fft_im[k]) / norm;
        float a_corr = mag * 1.4142f;        /* net ×√2: CG correction (×2) then peak→RMS (÷√2) */
        s_bin_accum[k] += a_corr * a_corr;  /* accumulate linear power (amplitude²) */
    }
    (*accum_count)++;

    /* Return early until we have a full second of windows */
    if (*accum_count < WINDOWS_PER_SEC) {
        return;
    }

    /* ── STEP 9: Average power → dBFS ──────────────────────────────────────
     *
     *   avg_power = s_bin_accum[k] / W        (mean linear power across W windows)
     *   dBFS[k]   = 10·log10(avg_power)
     *
     *   10·log10 is used — NOT 20·log10 — because avg_power is already a
     *   POWER quantity (amplitude²). Using 20·log10 would double-count the
     *   square and read 3 dB too low on every bin.
     *
     *   Equivalently: 10·log10(A²) = 20·log10(A), so the result is
     *   identical to what 20·log10(a_corr) would give on the raw amplitude.
     *
     *   After this loop, s_bin_accum[k] holds dBFS and is read by publish().
     */
    float    peak_db  = -120.0f;
    uint32_t peak_bin = SOUND_BIN_LOW;

    for (uint32_t k = SOUND_BIN_LOW; k <= SOUND_BIN_HIGH; k++) {
        float avg_power = s_bin_accum[k] / (float)(*accum_count);
        s_bin_accum[k]  = (avg_power > 1e-14f) ? 10.0f * log10f(avg_power) : -120.0f;
        if (s_bin_accum[k] > peak_db) {
            peak_db  = s_bin_accum[k];
            peak_bin = k;
        }
    }

    /* Time-domain RMS dBFS for the blue Grafana panel (20·log10 — amplitude) */
    double avg_rms  = *rms_accum / (double)(*accum_count);
    double avg_dbfs = (avg_rms > 0.5)
                      ? 20.0 * log10(avg_rms / g_full_scale)
                      : -120.0;

    publish(avg_dbfs, (float)peak_bin * FREQ_RES, peak_db);

    /* Reset accumulators for the next 1-second window */
    for (uint32_t k = 0; k < FFT_SIZE / 2; k++) {
        s_bin_accum[k] = 0.0f;
    }
    *rms_accum   = 0.0;
    *accum_count = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SOUND THREAD ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════
 * Initialises I2S TX (silence loopback) and RX, then enters the startup
 * retry loop to lock the WS phase before beginning audio processing.
 *
 * STARTUP RETRY LOOP:
 *   The ESP32 I2S DMA starts at an arbitrary WS phase each boot. A jittered
 *   delay (100 + (attempt % 7) × 11 ms) breaks the deterministic phase cycle
 *   that would otherwise cause long retry sequences. Typically locks in 1–4
 *   attempts.
 *
 * OVERRUN RECOVERY:
 *   -EIO from i2s_read() indicates a DMA overrun (audio thread starved).
 *   Full I2S restart is performed, including re-running detect_and_set_shift()
 *   because WS phase may have changed. s_bin_accum is explicitly zeroed to
 *   prevent a partial accumulation from producing a spurious spike on the
 *   next publish.
 *
 * MAIN LOOP:
 *   Reads one DMA buffer per iteration (~23.2 ms), calls process_window(),
 *   then frees the slab back to the driver.
 */
void sound_thread(void) {
    const struct device *i2s_rx = DEVICE_DT_GET(I2S_RX_NODE);
    const struct device *i2s_tx = DEVICE_DT_GET(I2S_TX_NODE);

    init_hann();   /* pre-compute Hann coefficients once */

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

    /* Prime TX with silence so BCLK is live before RX starts */
    i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);

    if (i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
        LOG_ERR("TX START failed"); return;
    }

    /* Startup retry loop — jittered delay to randomise WS phase */
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
            /* DMA overrun — audio thread was starved. Full I2S restart.
             * CRITICAL: zero s_bin_accum here to prevent partial accumulation
             * from producing a spurious spike on the next publish cycle. */
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

            /* Reset all accumulators — s_bin_accum MUST be zeroed here */
            accum_count = 0;
            rms_accum   = 0.0;
            for (uint32_t k = 0; k < FFT_SIZE / 2; k++) {
                s_bin_accum[k] = 0.0f;
            }
            continue;
        }

        if (ret < 0) {
            LOG_ERR("i2s_read error: %d", ret);
            continue;
        }

        size_t num_frames = rx_size / (NUM_CHANNELS * BYTES_PER_SAMPLE);
        process_window((uint32_t *)rx_mem, num_frames, &rms_accum, &accum_count);

        k_mem_slab_free(&snd_rx_slab, rx_mem);
    }
}