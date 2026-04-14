#!/usr/bin/env python3
"""
parse_sound.py — decode SD card binary sound log from sensor node.

Usage:
    python3 parse_sound.py sound.bin
    python3 parse_sound.py sound.bin --csv > sound.csv
    python3 parse_sound.py sound.bin --plot

Binary format (700 bytes per record):
    [0-3]   timestamp_ms    uint32  little-endian (Zephyr k_uptime_get_32)
    [4-5]   rms_dbfs_x100   int16   little-endian  e.g. -3820 = -38.20 dBFS
    [6-699] bins[0..347]    uint16  little-endian  magnitude × 10, bins 1-348

Bin frequency: bin i → Hz = (1 + i) × 43.1 Hz  (approx)
"""

import sys
import struct
import argparse

RECORD_SIZE   = 700          # sizeof(sound_spec_msg)
NUM_BINS      = 348
BIN_LOW       = 1            # first bin index (SOUND_BIN_LOW)
BIN_HZ        = 43.1         # Hz per bin

HEADER_FMT = '<Ih'           # uint32 timestamp, int16 rms_dbfs_x100
HEADER_SIZE = struct.calcsize(HEADER_FMT)
BINS_FMT    = f'<{NUM_BINS}H'


def bin_to_hz(bin_idx: int) -> float:
    return (BIN_LOW + bin_idx) * BIN_HZ


def parse_file(path: str):
    records = []
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(RECORD_SIZE)
            if len(chunk) < RECORD_SIZE:
                break
            ts_ms, rms_x100 = struct.unpack_from(HEADER_FMT, chunk, 0)
            bins = struct.unpack_from(BINS_FMT, chunk, HEADER_SIZE)
            records.append({
                'timestamp_ms':   ts_ms,
                'rms_dbfs':       rms_x100 / 100.0,
                'bins':           bins,
                'peak_bin':       bins.index(max(bins)),
                'peak_hz':        bin_to_hz(bins.index(max(bins))),
                'peak_mag':       max(bins) / 10.0,
            })
    return records


def print_summary(records):
    print(f"Records: {len(records)}")
    if not records:
        return
    duration_s = (records[-1]['timestamp_ms'] - records[0]['timestamp_ms']) / 1000.0
    print(f"Duration: {duration_s:.1f}s")
    print(f"{'timestamp_ms':>14}  {'rms_dBFS':>9}  {'peak_hz':>9}  {'peak_mag':>9}")
    print("-" * 50)
    for r in records:
        print(f"{r['timestamp_ms']:>14}  {r['rms_dbfs']:>9.2f}  "
              f"{r['peak_hz']:>9.1f}  {r['peak_mag']:>9.1f}")


def print_csv(records):
    header = "timestamp_ms,rms_dbfs," + ",".join(
        f"{bin_to_hz(i):.0f}hz" for i in range(NUM_BINS))
    print(header)
    for r in records:
        bins_str = ",".join(f"{v/10.0:.1f}" for v in r['bins'])
        print(f"{r['timestamp_ms']},{r['rms_dbfs']:.2f},{bins_str}")


def plot_spectrogram(records):
    try:
        import numpy as np
        import matplotlib.pyplot as plt
    except ImportError:
        print("Install matplotlib and numpy: pip install matplotlib numpy")
        sys.exit(1)

    freqs = [bin_to_hz(i) for i in range(NUM_BINS)]
    times = [r['timestamp_ms'] / 1000.0 for r in records]
    data  = np.array([r['bins'] for r in records], dtype=float) / 10.0

    fig, axes = plt.subplots(3, 1, figsize=(12, 10))

    # Spectrogram
    ax = axes[0]
    im = ax.imshow(data.T, aspect='auto', origin='lower',
                   extent=[times[0], times[-1], freqs[0], freqs[-1]],
                   cmap='viridis')
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_title('Sound Spectrogram')
    plt.colorbar(im, ax=ax, label='Magnitude')

    # RMS over time
    ax = axes[1]
    rms = [r['rms_dbfs'] for r in records]
    ax.plot(times, rms, color='cyan', linewidth=1)
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('dBFS')
    ax.set_title('RMS Level')
    ax.set_ylim(-120, 0)
    ax.grid(True, alpha=0.3)

    # Peak frequency over time
    ax = axes[2]
    peak_hz = [r['peak_hz'] for r in records]
    ax.plot(times, peak_hz, color='orange', linewidth=1)
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Hz')
    ax.set_title('Peak Frequency')
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Parse sensor node sound binary log')
    parser.add_argument('file', help='Binary sound log file (e.g. sound.bin)')
    parser.add_argument('--csv',  action='store_true', help='Output full CSV with all bins')
    parser.add_argument('--plot', action='store_true', help='Plot spectrogram (requires matplotlib)')
    args = parser.parse_args()

    records = parse_file(args.file)

    if args.csv:
        print_csv(records)
    elif args.plot:
        plot_spectrogram(records)
    else:
        print_summary(records)


if __name__ == '__main__':
    main()
