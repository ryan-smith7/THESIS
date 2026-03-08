#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/printk.h>
#include <soc/i2s_struct.h>
#include <string.h>
#include <math.h>

/*
 * M_PI is not guaranteed by all toolchains/libc configs.
 * Define it explicitly to avoid 'undeclared' errors under Zephyr's
 * picolibc or newlib-nano configurations.
 */
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
/*
 * Sample rate satisfies Nyquist: fs > 2 × fmax = 2 × 15000 = 30000 Hz.
 * 44100 Hz gives Nyquist ceiling of 22050 Hz — beyond SPH0645's 15 kHz limit.
 */
#define SAMPLE_RATE        44100U
#define SAMPLE_BIT_WIDTH   32U
#define NUM_CHANNELS       2U

/*
 * FFT_SIZE reduced from 2048 → 1024 to fix DRAM overflow.
 *
 * Memory saved vs 2048:
 *   4 float buffers:  4 × (2048-1024) × 4 = 16,384 bytes saved
 *   rx_slab (2 bufs): 2 × (16384-8192) = 16,384 bytes saved
 *   tx_slab (2 bufs): 2 × (16384-8192) = 16,384 bytes saved
 *   Total saved: ~49 KB — well clear of the 6 KB overflow
 *
 * Frequency resolution at 1024:
 *   Δf = 44100 / 1024 = 43.1 Hz per bin
 *   Window duration = 1024 / 44100 = 23.2 ms
 *   Bins in range: floor(50/43.1)=1 to floor(15000/43.1)=348
 *
 * 43 Hz resolution is sufficient for environmental noise monitoring —
 * frequency bands of interest (speech, HVAC, traffic) are all wider
 * than 43 Hz.
 */
#define FFT_SIZE           1024U
#define SAMPLES_PER_BUF    FFT_SIZE
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
#define BUF_SIZE           (SAMPLES_PER_BUF * NUM_CHANNELS * BYTES_PER_SAMPLE)

/*
 * NUM_BUFS reduced from 4 → 2.
 * One buffer fills via DMA while the other is being processed.
 * At 23.2 ms window and ~10 ms FFT compute time there is ample
 * headroom — a third/fourth buffer adds no benefit and costs 16 KB each.
 */
#define NUM_BUFS           2U
#define TIMEOUT_MS         2000

/*
 * MIC_SLOT: SPH0645 SEL=GND → left channel → index 0.
 * Change to 1 if SEL is tied to VCC.
 */
#define MIC_SLOT           0U

/*
 * Frequency range of interest: 50 Hz – 15000 Hz (SPH0645 datasheet).
 *   Δf = 43.07 Hz/bin  (44100 / 1024)
 *   bin_low  = floor(50    / 43.07) = 1
 *   bin_high = floor(15000 / 43.07) = 348
 */
#define FREQ_RES           ((float)SAMPLE_RATE / (float)FFT_SIZE)
#define BIN_LOW            ((uint32_t)(50.0f    / FREQ_RES))
#define BIN_HIGH           ((uint32_t)(15000.0f / FREQ_RES))

/* ── Memory slabs ───────────────────────────────────────── */
K_MEM_SLAB_DEFINE_STATIC(rx_slab, BUF_SIZE, NUM_BUFS, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BUF_SIZE, 2U, 4);

/* ── Working buffers — static BSS, never on stack ───────── */
/*
 * At 1024 floats × 4 bytes = 4 KB each these are still too large
 * for the thread stack (default 4 KB total). Static placement is required.
 *
 * Total static buffer usage:
 *   windowed + fft_re + fft_im + hann = 4 × 4096 = 16 KB
 */
static float windowed[FFT_SIZE];  /* DC-blocked samples, then Hann-windowed */
static float fft_re[FFT_SIZE];    /* FFT real part (in-place)               */
static float fft_im[FFT_SIZE];    /* FFT imaginary part (in-place)          */
static float hann[FFT_SIZE];      /* pre-computed Hann coefficients         */

/*
 * Accumulator for 1-second averaging.
 * ~43 FFT windows fit in 1 second (1000 ms / 23.2 ms per window).
 * Each bin's magnitude is summed across windows then divided to get
 * the 1-second average spectrum sent over UART.
 */
#define WINDOWS_PER_SECOND  43U
static float   bin_accum[FFT_SIZE / 2]; /* magnitude accumulator per bin   */
static uint32_t accum_count;            /* how many windows accumulated     */

/* ── DC blocking filter ─────────────────────────────────── */
/*
 * Single-pole IIR high-pass filter: y[n] = x[n] − x[n-1] + α·y[n-1]
 * α = 0.9975 → cutoff ≈ 4 Hz at 44100 Hz.
 * Removes the large DC bias the SPH0645 outputs at rest.
 */
static float dc_block(float x)
{
	static float x1 = 0.0f, y1 = 0.0f;
	float y = x - x1 + 0.9975f * y1;

	x1 = x;
	y1 = y;
	return y;
}

/* ── PCM extraction ─────────────────────────────────────── */
/*
 * SPH0645 packs 18-bit audio into bits [31:14] of the 32-bit slot.
 * int32_t cast preserves sign, >> 14 yields ±131072.
 */
static inline int32_t extract_pcm(uint32_t raw)
{
	return (int32_t)raw >> 14;
}

/* ── Hann window initialisation ─────────────────────────── */
/*
 * w[i] = 0.5 × (1 − cos(2π·i / (N−1)))
 * Pre-computed once at startup and reused every window.
 * Suppresses spectral leakage — prevents a tone bleeding into
 * adjacent FFT bins.
 */
static void init_hann(void)
{
	for (uint32_t i = 0; i < FFT_SIZE; i++) {
		hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i
					       / (float)(FFT_SIZE - 1)));
	}

	printk("=== SPH0645 FFT Analyser ===\n");
	printk("FFT size     : %u\n", FFT_SIZE);
	printk("Sample rate  : %u Hz\n", SAMPLE_RATE);
	printk("Freq res     : %.2f Hz/bin\n", (double)FREQ_RES);
	printk("Window       : %.1f ms\n",
	       (double)FFT_SIZE / (double)SAMPLE_RATE * 1000.0);
	printk("Freq range   : %.1f Hz (bin %u) to %.1f Hz (bin %u)\n",
	       (double)BIN_LOW  * (double)FREQ_RES, BIN_LOW,
	       (double)BIN_HIGH * (double)FREQ_RES, BIN_HIGH);
	printk("Avg interval : %u windows (~1 sec)\n\n", WINDOWS_PER_SECOND);
}

/* ── In-place radix-2 Cooley-Tukey FFT ─────────────────── */
/*
 * Iterative FFT on separate real/imaginary arrays.
 * Input:  fft_re[] = windowed samples, fft_im[] = 0.
 * Output: fft_re[k], fft_im[k] = complex DFT at bin k (frequency k×Δf Hz).
 * Complexity: O(N log₂N) = 1024 × 10 = 10,240 ops for N=1024.
 */
static void fft_compute(void)
{
	uint32_t n = FFT_SIZE;

	/* Bit-reversal permutation */
	uint32_t j = 0;

	for (uint32_t i = 1; i < n; i++) {
		uint32_t bit = n >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float tmp;

			tmp = fft_re[i]; fft_re[i] = fft_re[j]; fft_re[j] = tmp;
			tmp = fft_im[i]; fft_im[i] = fft_im[j]; fft_im[j] = tmp;
		}
	}

	/* Butterfly stages */
	for (uint32_t len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * M_PI / (float)len;
		float wRe = cosf(ang);
		float wIm = sinf(ang);

		for (uint32_t i = 0; i < n; i += len) {
			float curRe = 1.0f, curIm = 0.0f;

			for (uint32_t k = 0; k < len / 2; k++) {
				uint32_t u = i + k;
				uint32_t v = i + k + len / 2;

				float tRe = curRe * fft_re[v] - curIm * fft_im[v];
				float tIm = curRe * fft_im[v] + curIm * fft_re[v];

				fft_re[v] = fft_re[u] - tRe;
				fft_im[v] = fft_im[u] - tIm;
				fft_re[u] += tRe;
				fft_im[u] += tIm;

				float newRe = curRe * wRe - curIm * wIm;
				curIm       = curRe * wIm + curIm * wRe;
				curRe       = newRe;
			}
		}
	}
}

/* ── Print 1-second averaged spectrum ───────────────────── */
/*
 * Called every WINDOWS_PER_SECOND windows (~1 sec).
 * Prints a compact text report readable by a host parser or terminal.
 *
 * Output format (one line per second):
 *   SPEC <rms_dbfs> <bin_low> <bin_high> <mag_bin1> <mag_bin2> ... <mag_binN>
 *
 * The SPEC prefix lets a host-side parser (Python, Node, etc.) reliably
 * identify spectrum lines among any debug output.
 * All magnitudes are on the ±131072 scale (same as RMS).
 *
 * Peak bin is also printed separately for quick human reading.
 */
static void print_spectrum(double rms_dbfs, uint32_t second_count)
{
	/* Find peak bin in range */
	float    peak_mag = 0.0f;
	uint32_t peak_bin = BIN_LOW;

	for (uint32_t k = BIN_LOW; k <= BIN_HIGH; k++) {
		if (bin_accum[k] > peak_mag) {
			peak_mag = bin_accum[k];
			peak_bin = k;
		}
	}

	float peak_freq = (float)peak_bin * FREQ_RES;

	/* Human-readable summary line */
	printk("--- Second %u | %.1f dBFS | Peak %.1f Hz (%.1f) ---\n",
	       second_count,
	       rms_dbfs,
	       (double)peak_freq,
	       (double)peak_mag);

	/*
	 * Machine-readable spectrum line.
	 * Format: SPEC <dBFS> <bin_low> <bin_high> <mag> <mag> ... <mag>
	 * One magnitude value per bin from BIN_LOW to BIN_HIGH.
	 * This is what gets parsed and sent to the database later.
	 */
	printk("SPEC %.2f %u %u", rms_dbfs, BIN_LOW, BIN_HIGH);
	for (uint32_t k = BIN_LOW; k <= BIN_HIGH; k++) {
		printk(" %.1f", (double)bin_accum[k]);
	}
	printk("\n");
}

/* ── Process one audio window ───────────────────────────── */
/*
 * Full pipeline per 1024-sample window (23.2 ms of audio):
 *
 * 1. Extract 18-bit PCM from mic's I2S slot.
 * 2. DC-block (remove SPH0645 bias).
 * 3. RMS on raw DC-blocked samples (pre-window = true energy).
 *    RMS = sqrt( (1/N) × Σ x[i]² )
 * 4. Hann window: x_w[i] = x[i] × w[i]
 * 5. FFT (Cooley-Tukey in-place).
 * 6. Magnitude: |X[k]| = sqrt(Re²+Im²) / (N/2), normalised to ±131072 scale.
 * 7. Accumulate magnitudes into bin_accum[].
 * 8. Every WINDOWS_PER_SECOND windows, average and print.
 */
static void process_window(uint32_t *frames, size_t num_frames,
			   uint32_t block_count, uint32_t *second_count)
{
	/* Steps 1–2: Extract and DC-block, accumulate RMS sum */
	static double rms_accum = 0.0;
	double sum_sq = 0.0;

	for (uint32_t i = 0; i < num_frames; i++) {
		float s = dc_block((float)extract_pcm(
				frames[i * NUM_CHANNELS + MIC_SLOT]));
		windowed[i] = s;
		sum_sq += (double)(s * s);
	}

	/* Step 3: RMS for this window */
	double rms = sqrt(sum_sq / (double)num_frames);

	rms_accum += rms;

	/* Step 4: Apply Hann window */
	for (uint32_t i = 0; i < num_frames; i++) {
		windowed[i] *= hann[i];
	}

	/* Step 5: Load FFT input and compute */
	for (uint32_t i = 0; i < FFT_SIZE; i++) {
		fft_re[i] = (i < num_frames) ? windowed[i] : 0.0f;
		fft_im[i] = 0.0f;
	}
	fft_compute();

	/* Step 6 & 7: Magnitude + accumulate into bin_accum */
	float norm = (float)(FFT_SIZE / 2);

	for (uint32_t k = BIN_LOW; k <= BIN_HIGH; k++) {
		float mag = sqrtf(fft_re[k] * fft_re[k]
				  + fft_im[k] * fft_im[k]) / norm;
		bin_accum[k] += mag;
	}
	accum_count++;

	/* Step 8: Every ~1 second, average and print */
	if (accum_count >= WINDOWS_PER_SECOND) {

		/* Average each bin over the accumulated windows */
		for (uint32_t k = BIN_LOW; k <= BIN_HIGH; k++) {
			bin_accum[k] /= (float)accum_count;
		}

		/* Average RMS → dBFS */
		double avg_rms  = rms_accum / (double)accum_count;
		double avg_dbfs = (avg_rms > 0.5)
				  ? 20.0 * log10(avg_rms / 131072.0)
				  : -120.0;

		(*second_count)++;
		print_spectrum(avg_dbfs, *second_count);

		/* Reset accumulators */
		for (uint32_t k = 0; k < FFT_SIZE / 2; k++) {
			bin_accum[k] = 0.0f;
		}
		rms_accum   = 0.0;
		accum_count = 0;
	}
}

/* ── Main ───────────────────────────────────────────────── */
int main(void)
{
	const struct device *const i2s_dev_rx = DEVICE_DT_GET(I2S_RX_NODE);
	const struct device *const i2s_dev_tx = DEVICE_DT_GET(I2S_TX_NODE);

	init_hann();

	if (!device_is_ready(i2s_dev_rx)) {
		printk("I2S device not ready\n");
		return -ENODEV;
	}

	/* ── Configure TX (clock source for BCLK/WS) ───────── */
	struct i2s_config cfg = {
		.word_size      = SAMPLE_BIT_WIDTH,
		.channels       = NUM_CHANNELS,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_BIT_CLK_MASTER
				| I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab       = &tx_slab,
		.block_size     = BUF_SIZE,
		.timeout        = TIMEOUT_MS,
	};

	if (i2s_configure(i2s_dev_tx, I2S_DIR_TX, &cfg) < 0) {
		printk("TX configure failed\n");
		return -EIO;
	}

	/* ── Configure RX ───────────────────────────────────── */
	cfg.mem_slab = &rx_slab;
	if (i2s_configure(i2s_dev_rx, I2S_DIR_RX, &cfg) < 0) {
		printk("RX configure failed\n");
		return -EIO;
	}

	/* ── Pre-queue silent TX buffer ─────────────────────── */
	void *tx_mem;

	if (k_mem_slab_alloc(&tx_slab, &tx_mem, K_NO_WAIT) < 0) {
		printk("TX slab alloc failed\n");
		return -ENOMEM;
	}
	memset(tx_mem, 0, BUF_SIZE);
	i2s_write(i2s_dev_tx, tx_mem, BUF_SIZE);

	/* ── Start TX — activates BCLK and WS ───────────────── */
	if (i2s_trigger(i2s_dev_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
		printk("TX START failed\n");
		return -EIO;
	}

	/* 50 ms for SPH0645 PLL to lock onto BCLK */
	k_sleep(K_MSEC(50));

	/* ── Start RX ───────────────────────────────────────── */
	if (i2s_trigger(i2s_dev_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
		printk("RX START failed\n");
		return -EIO;
	}

	printk("Running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34\n\n");

	/* ── Read loop ──────────────────────────────────────── */
	uint32_t block_count  = 0;
	uint32_t second_count = 0;

	while (1) {
		void    *rx_mem;
		uint32_t rx_size;

		int ret = i2s_read(i2s_dev_rx, &rx_mem, (size_t *)&rx_size);

		if (ret == -EIO) {
			printk("Overrun — restarting RX\n");
			i2s_trigger(i2s_dev_rx, I2S_DIR_RX, I2S_TRIGGER_DROP);
			k_sleep(K_MSEC(10));
			i2s_trigger(i2s_dev_rx, I2S_DIR_RX, I2S_TRIGGER_START);
			accum_count = 0;   /* discard partial accumulation */
			continue;
		}

		if (ret < 0) {
			printk("i2s_read error: %d\n", ret);
			continue;
		}

		block_count++;

		uint32_t *frames     = (uint32_t *)rx_mem;
		size_t    num_frames = rx_size
				       / (NUM_CHANNELS * BYTES_PER_SAMPLE);

		process_window(frames, num_frames, block_count, &second_count);

		k_mem_slab_free(&rx_slab, rx_mem);
	}

	return 0;
}