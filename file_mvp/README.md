# SPH0645 I2S Microphone — FFT Audio Analyser
Zephyr RTOS · ESP32 DevKit WROOM · Cooley-Tukey FFT

---

## Overview

Reads digital audio from an SPH0645 MEMS microphone over I2S, computes RMS volume and a full frequency spectrum via FFT, and prints results to the serial console. Runs on the ESP32 DevKit WROOM under Zephyr RTOS, built with `west`.

---

## Hardware

| Signal | ESP32 GPIO |
|--------|-----------|
| BCLK   | GPIO 26   |
| WS     | GPIO 25   |
| DOUT (mic → ESP32) | GPIO 34 |
| SEL    | GND (left channel, `MIC_SLOT=0`) |

> If SEL is connected to VCC instead, change `#define MIC_SLOT 1U` in `main.c`.

---

## Configuration

```c
#define SAMPLE_RATE      44100U    // Hz — satisfies Nyquist for 15 kHz mic limit
#define FFT_SIZE         2048U     // Power of 2, required by Cooley-Tukey
#define SAMPLE_BIT_WIDTH 32U       // SPH0645 uses 32-bit I2S slots
#define NUM_CHANNELS     2U        // Stereo I2S frame (mic uses one slot)
#define MIC_SLOT         0U        // 0 = left (SEL=GND), 1 = right (SEL=VCC)
```

### Derived values

| Parameter | Value | Formula |
|-----------|-------|---------|
| Frequency resolution | 21.5 Hz/bin | `fs / N = 44100 / 2048` |
| Window duration | 46.4 ms | `N / fs = 2048 / 44100` |
| Lowest bin analysed | bin 2 (~43 Hz) | `floor(50 / 21.5)` |
| Highest bin analysed | bin 696 (~14,974 Hz) | `floor(15000 / 21.5)` |
| Buffer size | 16,384 bytes | `2048 × 2ch × 4 bytes` |
| FFT compute time | ~20 ms | measured on LX6 @ 240 MHz |
| Compute headroom | ~26 ms | `46.4ms window − 20ms FFT` |

---

## Signal Processing Pipeline

Each 2048-frame buffer goes through the following steps:

```
I2S DMA buffer arrives  (2048 stereo frames = 16,384 bytes)
        │
        ▼
1. extract_pcm()
   SPH0645 packs 18-bit audio into bits [31:14] of the 32-bit slot.
   Cast to int32_t then >> 14 gives a value in ±131,072.

        │
        ▼
2. dc_block()
   Single-pole IIR high-pass filter removes SPH0645 DC bias.
   y[n] = x[n] − x[n−1] + 0.9975 · y[n−1]
   Cutoff ≈ 4 Hz — removes DC while passing all audio content.

        │
        ▼
3. RMS
   Computed on raw DC-blocked samples (before windowing).
   RMS = sqrt( (1/N) × Σ x[i]² )
   Converted to dBFS: 20 × log10(RMS / 131072)
   Full-scale reference = 131072 (2^17, 18-bit signed peak).

        │
        ▼
4. Hann window
   x_w[i] = x[i] × 0.5 × (1 − cos(2πi / (N−1)))
   Tapers signal to zero at window edges, suppressing spectral
   leakage — prevents a tone's energy bleeding into adjacent bins.

        │
        ▼
5. fft_compute()   [Cooley-Tukey radix-2, in-place]
   a. Bit-reversal permutation reorders samples.
   b. Log₂(N) = 11 butterfly stages combine results bottom-up.
   Output: fft_re[k] and fft_im[k] = complex amplitude at k × Δf Hz.
   Complexity: O(N log₂N) = 22,528 operations for N=2048.

        │
        ▼
6. Magnitude spectrum
   |X[k]| = sqrt(Re[k]² + Im[k]²) / (N/2)
   Normalised by N/2 to match the ±131,072 amplitude scale of RMS.
   Only bins BIN_LOW…BIN_HIGH (50 Hz → 15 kHz) are evaluated.

        │
        ▼
7. printk() output
   Block | RMS | dBFS | Peak frequency (Hz) | Peak magnitude

        │
        ▼
k_mem_slab_free()   — buffer returned to pool, ready for next block
```

---

## Why TX is Started Before RX

The SPH0645 is a **clock slave** — it requires BCLK and WS from the master to operate. On the ESP32, the I2S clock generator only runs when TX is active. TX is started with a silent (zero-filled) buffer purely to **generate the clocks**. RX starts 50 ms later to allow the mic's PLL to lock onto BCLK before capture begins.

---

## Memory Layout

All large buffers are declared `static` (BSS / global memory) — **not** on the thread stack. At 2048 floats × 4 bytes = 8 KB each, stack placement would cause an immediate stack overflow (ESP32 default stack ~8 KB).

| Buffer | Size | Purpose |
|--------|------|---------|
| `rx_slab` (×4 blocks) | 4 × 16,384 B | DMA receive pool |
| `tx_slab` (×2 blocks) | 2 × 16,384 B | Silent clock-keeping TX |
| `windowed[]` | 8 KB | DC-blocked then Hann-windowed samples |
| `fft_re[]` | 8 KB | FFT real part |
| `fft_im[]` | 8 KB | FFT imaginary part |
| `hann[]` | 8 KB | Pre-computed Hann coefficients (startup only) |

---

## Output Format

```
FFT config: N=2048, fs=44100 Hz, Δf=21.53 Hz/bin
Frequency range: 43.1 Hz (bin 2) → 14974.6 Hz (bin 696)
Running — BCLK=GPIO26 WS=GPIO25 DOUT=GPIO34
Block      RMS          dBFS       Peak Hz        Magnitude
Block    10 | RMS=  1842.3 |  -37.1 dBFS | Peak=  440.4 Hz ( 312.5)
Block    20 | RMS=   203.1 |  -56.2 dBFS | Peak= 1204.1 Hz (  44.1)
```

### dBFS reference levels

| Level | Meaning |
|-------|---------|
| 0 dBFS | Full scale — loudest possible signal |
| −37 dBFS | Typical speech at close range |
| −40 to −50 dBFS | Quiet room background |
| −120 dBFS | Guard value (silence / below threshold) |

---

## Overrun Handling

If the DMA buffer fills faster than the processing loop reads it, `i2s_read()` returns `-EIO`. The handler flushes (`I2S_TRIGGER_DROP`) and restarts RX, printing a warning. This should not occur under normal load given the ~26 ms compute headroom.

---

## Build

```bash
west build -b esp32_devkitc_wroom -- -DBOARD_ROOT=.
west flash
west espressif monitor   # or: screen /dev/ttyUSB0 115200
```

---

## Dependencies

- Zephyr RTOS (tested with west / Zephyr v4.x)
- No external libraries — FFT is self-contained Cooley-Tukey
- Standard C math: `<math.h>` (linked via `CONFIG_FPU=y` in `prj.conf`)

### Recommended `prj.conf`

```ini
CONFIG_I2S=y
CONFIG_FPU=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_HEAP_MEM_POOL_SIZE=8192
```