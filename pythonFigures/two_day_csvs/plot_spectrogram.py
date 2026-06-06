"""
Sound Spectrogram Plot
======================
Plots the full sound spectrogram from sound_bins.csv matching the Grafana
panel style: yellow (0 dB) → green → dark purple (120 dB).

Usage:
    python3 plot_spectrogram.py

Output:
    ./plots/spectrogram.png
"""

import os
import json
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.colors import LinearSegmentedColormap

# ── Config ────────────────────────────────────────────────────────────────────
CSV_PATH = './sound_bins_combined.csv'
OUT_DIR  = './plots'
OUT_FILE = 'spectrogram.png'

# Frequency axis
BIN_LOW_HZ = 43
BIN_RES_HZ = 43
MIN_DB     = 0
MAX_DB     = 120

os.makedirs(OUT_DIR, exist_ok=True)

# ── Grafana-matching colourmap ────────────────────────────────────────────────
# Yellow (low) → green (mid) → dark purple (high) — matches Grafana Viridis-like
grafana_colors = [
    (0.0,  '#FDE725'),   # yellow   — 0 dB
    (0.25, '#5DC863'),   # green
    (0.50, '#21908C'),   # teal
    (0.75, '#3B528B'),   # blue-purple
    (1.0,  '#440154'),   # dark purple — 120 dB
]
cmap = LinearSegmentedColormap.from_list(
    'grafana_viridis',
    [(v, c) for v, c in grafana_colors]
)

# ── Load data ─────────────────────────────────────────────────────────────────
print("Loading sound_bins.csv...", flush=True)
df = pd.read_csv(CSV_PATH, parse_dates=['received_time'])
df = df.set_index('received_time').sort_index()
print(f"  {len(df):,} rows  [{df.index[0]}  →  {df.index[-1]}]", flush=True)

# ── Decode bins ───────────────────────────────────────────────────────────────
print("Decoding bins_json...", flush=True)

def decode(row):
    try:
        return np.array(json.loads(row), dtype=np.float32)
    except Exception:
        return None

df['bins_decoded'] = df['bins_json'].apply(decode)
df = df[df['bins_decoded'].notna()].copy()
print(f"  {len(df):,} valid rows after decode", flush=True)

# Stack into 2D array: rows=time, cols=frequency bins
spectrogram = np.stack(df['bins_decoded'].values)   # shape (N_time, N_bins)
n_bins = spectrogram.shape[1]

# Frequency axis
bin_freqs = BIN_LOW_HZ + np.arange(n_bins) * BIN_RES_HZ  # Hz

print(f"  Spectrogram shape: {spectrogram.shape}  "
      f"({n_bins} bins, {len(df)} time steps)", flush=True)
print(f"  Frequency range: {bin_freqs[0]} Hz — {bin_freqs[-1]} Hz", flush=True)

# ── Plot ──────────────────────────────────────────────────────────────────────
print("Plotting spectrogram...", flush=True)

# Figure width scales with time extent — cap at 24 inches
n_times = len(df)
fig_w = min(24, max(14, n_times / 500))
fig_h = 6

fig, ax = plt.subplots(figsize=(fig_w, fig_h))

# pcolormesh: x=time, y=frequency, colour=dB SPL
# Transpose so rows=frequency, cols=time
times = mdates.date2num(df.index.to_pydatetime())

im = ax.pcolormesh(
    times,
    bin_freqs,
    spectrogram.T,
    cmap=cmap,
    vmin=MIN_DB,
    vmax=MAX_DB,
    shading='nearest',
    rasterized=True,
)

# ── Axes formatting ───────────────────────────────────────────────────────────
ax.set_ylim(bin_freqs[0], bin_freqs[-1])
ax.set_ylabel('Frequency (Hz)')
ax.set_xlabel('Time (UTC)')

# Date formatting on x-axis
ax.xaxis_date()
ax.xaxis.set_major_formatter(mdates.DateFormatter('%d %b\n%H:%M'))
ax.xaxis.set_major_locator(mdates.AutoDateLocator())
plt.setp(ax.get_xticklabels(), rotation=0, ha='center', fontsize=8)

# Colorbar
cbar = fig.colorbar(im, ax=ax, pad=0.01)
cbar.set_label('dB SPL', fontsize=9)
cbar.set_ticks([0, 20, 40, 60, 80, 100, 120])

date_start = df.index[0].strftime('%Y-%m-%d %H:%M')
date_end   = df.index[-1].strftime('%Y-%m-%d %H:%M')
ax.set_title(
    f'Sound Spectrogram — {date_start} to {date_end} UTC',
    fontsize=11, fontweight='bold'
)

plt.tight_layout()
out = os.path.join(OUT_DIR, OUT_FILE)
plt.savefig(out, dpi=200, bbox_inches='tight')
plt.close()
print(f"  Saved: {out}", flush=True)
print("Done.", flush=True)
