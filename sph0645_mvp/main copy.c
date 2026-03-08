#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/printk.h>
#include <soc/i2s_struct.h>
#include <string.h>
#include <math.h>

/* ── Node labels ────────────────────────────────────────── */
#if DT_NODE_EXISTS(DT_NODELABEL(i2s_rxtx))
#define I2S_RX_NODE  DT_NODELABEL(i2s_rxtx)
#define I2S_TX_NODE  I2S_RX_NODE
#else
#define I2S_RX_NODE  DT_NODELABEL(i2s_rx)
#define I2S_TX_NODE  DT_NODELABEL(i2s_tx)
#endif

/* ── Audio config ───────────────────────────────────────── */
#define SAMPLE_RATE        44100U   /* Hz */
#define SAMPLE_BIT_WIDTH   32U      /* 32-bit slots */
#define NUM_CHANNELS       2U       /* stereo I2S frame */
#define SAMPLES_PER_BUF    512U     /* frames per buffer */
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
#define BUF_SIZE           (SAMPLES_PER_BUF * NUM_CHANNELS * BYTES_PER_SAMPLE)
#define NUM_BUFS           4U
#define TIMEOUT_MS         1000

/*
 * Which interleaved slot contains mic data.
 * SEL=GND → left channel → slot 0 in each stereo pair.
 * SEL=VCC → right channel → slot 1. Change to 1 if no signal on 0.
 */
#define MIC_SLOT           0U

/* ── Memory slabs ───────────────────────────────────────── */
K_MEM_SLAB_DEFINE_STATIC(rx_slab, BUF_SIZE, NUM_BUFS, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BUF_SIZE, 2U, 4);

/* ── DC blocking filter ─────────────────────────────────── */
/*
 * Single-pole high-pass filter removes the large DC offset that the
 * SPH0645 outputs at rest. Without this, RMS reflects the bias
 * (~250–8000 counts) rather than actual audio energy.
 * Coefficient 0.9975 gives ~4Hz cutoff at 44100Hz — removes DC
 * while passing all audio content above ~4Hz.
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
 * SPH0645 places 18-bit audio in bits [31:14] of the 32-bit slot.
 * Right-shifting the raw uint32 by 14 as a signed int32 gives a
 * value in the range ±131072 (18-bit signed).
 * Using >> 8 instead gives ±8388608 (24-bit) but the lower 6 bits
 * are noise — either works for RMS, just changes the scale.
 */
static inline int32_t extract_pcm(uint32_t raw)
{
	return (int32_t)raw >> 14;   /* 18-bit signed value */
}

/* ── RMS + dBFS ─────────────────────────────────────────── */
/*
 * Computes RMS of the DC-blocked mic samples and converts to dBFS.
 * Full scale for 18-bit signed = 131072.
 * 0 dBFS = full scale sine wave.
 * Typical quiet room = -50 to -40 dBFS.
 * Speech = -30 to -10 dBFS.
 */
static void compute_and_print(uint32_t *frames, size_t num_frames,
			      uint32_t block_count)
{
	double sum_sq = 0.0;

	for (size_t i = 0; i < num_frames; i++) {
		float s = dc_block((float)extract_pcm(
				frames[i * NUM_CHANNELS + MIC_SLOT]));
		sum_sq += (double)(s * s);
	}

	double rms  = sqrt(sum_sq / (double)num_frames);
	double dbfs = (rms > 0.5) ? 20.0 * log10(rms / 131072.0) : -120.0;

	printk("Block %5u | RMS=%8.1f | %6.1f dBFS\n",
		block_count, rms, dbfs);
}

/* ── Main ───────────────────────────────────────────────── */
int main(void)
{
	const struct device *const i2s_dev_rx = DEVICE_DT_GET(I2S_RX_NODE);
	const struct device *const i2s_dev_tx = DEVICE_DT_GET(I2S_TX_NODE);

	printk("SPH0645 base code — %uHz %u-bit ch=%u\n",
		SAMPLE_RATE, SAMPLE_BIT_WIDTH, MIC_SLOT);

	if (!device_is_ready(i2s_dev_rx)) {
		printk("I2S device not ready\n");
		return -ENODEV;
	}

	/* ── Configure TX (clock source for BCLK/WS) ─────── */
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

	/* ── Configure RX ───────────────────────────────────  */
	cfg.mem_slab = &rx_slab;
	if (i2s_configure(i2s_dev_rx, I2S_DIR_RX, &cfg) < 0) {
		printk("RX configure failed\n");
		return -EIO;
	}

	/* ── Pre-queue silent TX buffer ─────────────────────  */
	/*
	 * TX start_transfer pulls from its message queue immediately.
	 * Must have at least one buffer queued before START.
	 * With i2s_ll_tx_stop_on_fifo_empty=false patched, BCLK
	 * continues after this buffer drains.
	 */
	void *tx_mem;

	if (k_mem_slab_alloc(&tx_slab, &tx_mem, K_NO_WAIT) < 0) {
		printk("TX slab alloc failed\n");
		return -ENOMEM;
	}
	memset(tx_mem, 0, BUF_SIZE);
	i2s_write(i2s_dev_tx, tx_mem, BUF_SIZE);

	/* ── Start TX — activates BCLK and WS ───────────────  */
	if (i2s_trigger(i2s_dev_tx, I2S_DIR_TX, I2S_TRIGGER_START) < 0) {
		printk("TX START failed\n");
		return -EIO;
	}

	/* Give clocks 50ms to stabilise before starting RX */
	k_sleep(K_MSEC(50));

	/* ── Start RX ───────────────────────────────────────  */
	if (i2s_trigger(i2s_dev_rx, I2S_DIR_RX, I2S_TRIGGER_START) < 0) {
		printk("RX START failed\n");
		return -EIO;
	}

	printk("Running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34\n");
	printk("%-10s %-12s %s\n", "Block", "RMS", "dBFS");

	/* ── Read loop ──────────────────────────────────────  */
	uint32_t block_count = 0;

	while (1) {
		void    *rx_mem;
		uint32_t rx_size;

		int ret = i2s_read(i2s_dev_rx, &rx_mem,
				   (size_t *)&rx_size);

		if (ret == -EIO) {
			/* Overrun — drain and restart RX */
			printk("Overrun — restarting RX\n");
			i2s_trigger(i2s_dev_rx, I2S_DIR_RX,
				    I2S_TRIGGER_DROP);
			k_sleep(K_MSEC(10));
			i2s_trigger(i2s_dev_rx, I2S_DIR_RX,
				    I2S_TRIGGER_START);
			continue;
		}

		if (ret < 0) {
			printk("i2s_read error: %d\n", ret);
			continue;
		}

		block_count++;

		/* Print every 10 blocks (~every 30ms at 44100Hz/512) */
		if (block_count % 10 == 0) {
			uint32_t *frames = (uint32_t *)rx_mem;
			size_t num_frames = rx_size /
					(NUM_CHANNELS * BYTES_PER_SAMPLE);

			compute_and_print(frames, num_frames, block_count);
		}

		k_mem_slab_free(&rx_slab, rx_mem);
	}

	return 0;
}