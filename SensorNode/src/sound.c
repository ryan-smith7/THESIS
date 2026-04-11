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
// #define NUM_TX_BUFS  2U   /* double-buffer — enough to stay ahead of RX */
#define TIMEOUT_MS         2000
#define MIC_SLOT           0U   /* SEL=GND → left channel */
#define FREQ_RES           ((float)SAMPLE_RATE / (float)FFT_SIZE)
// #define PCM_FULL_SCALE     131072.0

#define PCM_FULL_SCALE     131072.

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
static float dc_block(float x)
{
    static float x1 = 0.0f, y1 = 0.0f;
    float y = x - x1 + 0.9975f * y1;
    x1 = x; y1 = y;
    return y;
}

/* ── PCM extraction ─────────────────────────────────────── */
static inline int32_t extract_pcm(uint32_t raw)
{
    return (int32_t)raw >> 14;
}

/* ── Hann window ────────────────────────────────────────── */
static void init_hann(void)
{
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i
                                         / (float)(FFT_SIZE - 1)));
    }
}

/* ── Cooley-Tukey FFT ───────────────────────────────────── */
static void fft_compute(void)
{
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
static void publish(double avg_dbfs, float peak_freq, float peak_mag)
{
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

    /* 2. Full spectrum → sound_spec_q
     *
     * bins[] are magnitude × 10 as uint16_t.
     * Bins outside BIN_LOW–BIN_HIGH are already 0 from BSS/reset.
     * sound_ble.c will chunk this into 3 BLE notifications.
     */
    struct sound_spec_msg spec;
    spec.timestamp_ms  = k_uptime_get_32();
    spec.rms_dbfs_x100 = summary.rms_dbfs_x100;

    for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
        uint32_t k = SOUND_BIN_LOW + i;
        /* s_bin_accum[k] is already averaged before publish() is called */
        float v = s_bin_accum[k] * 10.0f;
        spec.bins[i] = (v > 65535.0f) ? 65535U : (uint16_t)v;
    }

    // // /* ADD THIS */
    // printk("[BINS]");
    // for (uint32_t i = 0; i < SOUND_NUM_BINS; i++) {
    //     printk(" %u", spec.bins[i]);
    // }
    // printk("\n");

    

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
                           double *rms_accum, uint32_t *accum_count)
{
    /* ── DIAGNOSTIC (remove after confirming raw PCM is sane) ── */
    if (*accum_count == 0) {
        int32_t mn = INT32_MAX, mx = INT32_MIN;
        for (uint32_t i = 0; i < num_frames; i++) {
            uint32_t raw = frames[i * NUM_CHANNELS + MIC_SLOT];
            int32_t  pcm = extract_pcm(raw);
            if (pcm < mn) mn = pcm;
            if (pcm > mx) mx = pcm;
        }
        /* raw word 0 so we can see bit layout */
        uint32_t raw0 = frames[MIC_SLOT];
        printk("[SND] raw0=0x%08X  pcm0=%d  min=%d  max=%d  range=%d\n",
               raw0, extract_pcm(raw0), mn, mx, mx - mn);
        /*
         * Healthy mic in a quiet room:  range ~ 2000–20000
         * DC offset / no signal:        range < 100  (all values identical)
         * Clipping:                     min near -131072, max near +131071
         * Wrong slot/shift:             raw0 looks like 0x00XXXXXX or 0xFFXXXXXX
         */
    }
    /* ── END DIAGNOSTIC ── */

    double sum_sq = 0.0;

    for (uint32_t i = 0; i < num_frames; i++) {
        float s = dc_block((float)extract_pcm(
                frames[i * NUM_CHANNELS + MIC_SLOT]));
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
                      ? 20.0 * log10(avg_rms / PCM_FULL_SCALE)
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
void sound_thread(void)
{
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

    k_sleep(K_MSEC(500));   /* SPH0645 PLL lock */

    if (i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
        LOG_ERR("RX START failed"); return;
    }

    LOG_INF("Sound thread running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34");

    double   rms_accum   = 0.0;
    uint32_t accum_count = 0;

    while (1) {
        /* Keep TX alive every iteration — one silence buf per RX buf consumed */
        // void *tx_mem;
        // if (k_mem_slab_alloc(&snd_tx_slab, &tx_mem, K_NO_WAIT) == 0) {
        //     memset(tx_mem, 0, BUF_SIZE);
        //     if (i2s_write(i2s_tx, tx_mem, BUF_SIZE) < 0) {
        //         k_mem_slab_free(&snd_tx_slab, tx_mem);
        //     }
        // }
        void    *rx_mem;
        uint32_t rx_size;

        int ret = i2s_read(i2s_rx, &rx_mem, (size_t *)&rx_size);

        if (ret == -EIO) {
            LOG_WRN("I2S overrun — full restart");
            i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
            i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
            k_sleep(K_MSEC(500));   /* full PLL re-lock time */
            i2s_write(i2s_tx, s_tx_silence, BUF_SIZE);
            i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
            k_sleep(K_MSEC(50));
            i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START);
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