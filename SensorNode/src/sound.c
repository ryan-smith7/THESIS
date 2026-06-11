/**
 * @file sound.c
 * @brief SPH0645 I2S microphone FFT analyser thread.
 *
 * process_window() pipeline (1024 stereo frames per DMA buffer):
 *   [1]  PCM extraction      — right-shift 32-bit I2S word → 18-bit signed
 *   [2]  DC blocking filter  — 1st-order IIR highpass, removes mic DC bias
 *   [3]  RMS accumulation    — time-domain RMS computed pre-window
 *   [4]  Hann window         — reduce spectral leakage before FFT
 *   [5]  1024-point FFT      — Cooley-Tukey in-place radix-2 DIT
 *   [6]  Magnitude normalise — |X[k]| / (N/2 × FS)
 *   [7]  CG + peak→RMS corr  — ×√2 net: undoes Hann CG (×2), peak→RMS (÷√2)
 *   [8]  Power accumulation  — accumulate a_corr² across 43 windows
 *   [9]  Average power→dBFS  — mean linear power per bin, then 10·log10
 *   [10] publish()           — pack bins into uint16 BLE message, enqueue
 *
 * Linear power is accumulated (step 8) and converted to dB once (step 9)
 * to avoid Jensen's inequality error from averaging in log space.
 *
 * 10·log10 is used at step 9 (not 20·log10) because s_bin_accum holds
 * power (amplitude²); using 20·log10 on power would read 3 dB too low.
 *
 * Hardware loopback: GPIO27 TX→RX keeps BCLK alive without an external cable.
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

/* ── Node labels 
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

/* ── Audio config 
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

/* ── Memory slabs 
 * Zephyr I2S driver requires pre-allocated fixed-size memory slabs.
 * The driver takes ownership of a slab block on read, caller must free it.
 * RX has 4 buffers to absorb scheduling jitter; TX only needs 1 .
 */
K_MEM_SLAB_DEFINE_STATIC(snd_rx_slab, BUF_SIZE, NUM_RX_BUFS, 4);
K_MEM_SLAB_DEFINE_STATIC(snd_tx_slab, BUF_SIZE, NUM_TX_BUFS, 4);

/* ── Message queues 
 * snd_q carries one sound_spec_msg per second (after 43-window accumulation)
 * to the BLE thread for chunked characteristic transmission.
 */
K_MSGQ_DEFINE(snd_q, sizeof(struct sound_spec_msg), SOUND_Q_DEPTH, 4);

/* ── Static working buffers
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


/**
 * @brief First-order IIR DC blocking filter (α=0.9975, cutoff ≈17.6 Hz).
 *
 * Removes the SPH0645 DC offset without affecting audio content.
 * Uses double-precision state to prevent long-term drift accumulation.
 */
static float dc_block(float x) {
    static double x1 = 0.0, y1 = 0.0;
    double y = (double)x - x1 + 0.9975 * y1;
    x1 = (double)x;
    y1 = y;
    return (float)y;
}


static uint8_t g_pcm_shift  = 14;
static double  g_full_scale = 131072.0;   /* 2^17 — 18-bit signed full scale */
static uint8_t g_mic_slot   = MIC_SLOT;

/**
 * @brief Extract a signed PCM sample from a raw 32-bit I2S word.
 *
 * Right-shifts by g_pcm_shift to align the 18-bit left-justified audio
 * data to a signed integer in [-2^17, +2^17].
 */
static inline int32_t extract_pcm(uint32_t raw) {
    return (int32_t)raw >> g_pcm_shift;
}

/**
 * @brief Detect the correct PCM bit shift and mic channel slot at boot.
 *
 * ORs all samples in each I2S slot across one DMA buffer and compares MSB
 * positions to identify which slot carries the real mic signal. Sets
 * g_mic_slot and g_pcm_shift on success.
 *
 * Retries are required if the DMA returns zeros (A), data is not
 * left-justified (B), or the silent slot has any bits set (C).
 *
 * @return true if slot and shift were locked successfully.
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

/**
 * @brief Pre-compute the Hann window coefficients into s_hann[].
 *
 * Called once at startup. Avoids 1024 cosf() calls per window at runtime.
 */
static void init_hann(void) {
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (float)(FFT_SIZE - 1)));
    }
}

/**
 * @brief In-place 1024-point Cooley-Tukey radix-2 DIT FFT.
 *
 * Operates on s_fft_re[] and s_fft_im[]. Input must be loaded before
 * calling. Positive-frequency bins are in indices 0..511 on output.
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

/**
 * @brief Encode the averaged spectrum into a sound_spec_msg and enqueue it.
 *
 * Clamps each bin to [-120, 0] dBFS and maps to uint16 as (db+120)×100.
 * Drops the oldest queue entry if snd_q is full.
 *
 * @param avg_dbfs     Time-domain RMS dBFS across the accumulation window.
 * @param peak_freq    Frequency (Hz) of the peak spectral bin.
 * @param peak_mag_db  Magnitude (dBFS) of the peak spectral bin.
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

        if (v < 0) {
            spec.bins[i] = 0;
        }
        else if (v > 65535) {
            spec.bins[i] = 65535;
        }
        else {
            spec.bins[i] = (uint16_t)v;
        }
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

/**
 * @brief Process one DMA buffer through the full DSP pipeline
 *
 * Accumulates linear power across WINDOWS_PER_SEC (~43) windows, then
 * converts to dBFS and calls publish(). Resets all accumulators after
 * each publish cycle.
 *
 * @param frames       Pointer to interleaved stereo I2S DMA buffer.
 * @param num_frames   Number of stereo frames in the buffer (1024).
 * @param rms_accum    Accumulated per-window RMS across the current cycle.
 * @param accum_count  Number of windows accumulated so far.
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

    /* ── time-domain RMS ─────
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

    /* ── Hann window 
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

    /* ── FFT input load 
     * Copy windowed real samples into the FFT real array; zero the imaginary
     * part (real-valued input). Pad with zeros if num_frames < FFT_SIZE
     * (should not occur in normal operation — both are 1024).
     */
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_fft_re[i] = (i < num_frames) ? s_windowed[i] : 0.0f;
        s_fft_im[i] = 0.0f;
    }
    fft_compute();   /* in-place Cooley-Tukey, see fft_compute() above */

    /* ──Normalise → correct → accumulate power
     *
     *— Normalise FFT magnitude:
     *   norm = (N/2) × full_scale = 512 × 131072 = 67,108,864
     *   mag  = |X[k]| / norm
     *
     *   For a full-scale sine (A = 131072) at a bin-aligned frequency,
     *   the Hann-windowed FFT gives |X[k]| ≈ A × CG × (N/2) = 33,554,432
     *   so mag = 33,554,432 / 67,108,864 = 0.5  (normalised peak amplitude)
     *
     * — Coherent gain + peak→RMS correction:
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
     * — Accumulate LINEAR POWER:
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

    /* ── average power → dBFS 
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

/**
 * @brief Sound thread entry point — initialises I2S, locks WS phase, then
 * reads DMA buffers and calls process_window() in a continuous loop.
 *
 * Performs a jittered startup retry to lock the WS phase, and a full I2S
 * restart with accumulator reset on DMA overrun (-EIO).
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
        LOG_ERR("TX configure failed");
        return;
    }

    cfg.mem_slab = &snd_rx_slab;
    if (i2s_configure(i2s_rx, I2S_DIR_RX, &cfg) < 0) {
        LOG_ERR("RX configure failed");
        return;
    }

    /* Prime TX with silence so BCLK is live before RX starts */
    i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);

    if (i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
        LOG_ERR("TX START failed");
        return;
    }

    /* Startup retry loop — jittered delay to randomise WS phase */
    for (int attempt = 1; ; attempt++) {
        k_sleep(K_MSEC(100 + (attempt % 7) * 11));

        if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
            LOG_ERR("RX START failed");
            return;
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
                    LOG_ERR("RX START failed in overrun recovery");
                    break;
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