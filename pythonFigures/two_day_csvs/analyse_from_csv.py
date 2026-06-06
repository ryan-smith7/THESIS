# """
# Section 5.5 Analysis Script — CSV Version
# ==========================================
# Loads from locally exported CSVs rather than querying Azure SQL.
# All figures written to ./figures_csv/
# All data written to ./data_csv/

# Expected CSV files (put in same folder as this script or set CSV_DIR):
#     bme280.csv      — timestamp, temperature_c, humidity_percent, pressure_hpa
#     ens160.csv      — timestamp, eco2_ppm, tvoc_ppb, aqi
#     spectrum.csv    — timestamp, AS7343_405nm ... AS7343_visible
#     soil.csv        — timestamp, soil_vwc_percent
#     soiltemp.csv    — timestamp, substrate_temp_c
#     sound.csv       — received_time, rms_dbfs
#     sound_bins.csv  — received_time, rms_dbfs, bins_json  (1 row/min sample)

# Usage:
#     python3 analyse_from_csv.py
# """

# import os
# import json
# import warnings
# import numpy as np
# import pandas as pd
# import matplotlib
# matplotlib.use('Agg')
# import matplotlib.pyplot as plt
# import matplotlib.dates as mdates
# from matplotlib.gridspec import GridSpec
# from scipy import signal, stats
# from scipy.fft import rfft, rfftfreq

# warnings.filterwarnings('ignore')

# # ── MACROS — edit these ───────────────────────────────────────────────────────

# CSV_DIR    = '.'
# OUTPUT_DIR = './figures_csv'
# DATA_DIR   = './data_csv'

# # Full stats window — match whatever is in your CSVs
# STATS_START = '2026-05-21 00:00:00'
# STATS_END   = '2026-05-27 00:00:00'

# # 1-hour coherence window for the FFT and zoomed plots
# FFT_START   = '2026-05-21 00:00:00'
# FFT_END     = '2026-05-27 00:00:00'

# SPECTRAL_START = '2026-05-23 00:00:00'
# SPECTRAL_END   = '2026-05-27 00:00:00'
# # Outlier IQR fence multiplier (3.0 = conservative, keeps most data)
# OUTLIER_IQR_K = 3.0

# # Resample resolution for correlation work
# RESAMPLE_FREQ = '5s'

# os.makedirs(OUTPUT_DIR, exist_ok=True)
# os.makedirs(DATA_DIR,   exist_ok=True)

# plt.rcParams.update({
#     'font.family':  'DejaVu Sans',
#     'font.size':    10,
#     'figure.dpi':   150,
#     'savefig.dpi':  200,
#     'savefig.bbox': 'tight',
# })

# # ── Load CSVs ─────────────────────────────────────────────────────────────────

# def load_csv(filename, time_col, parse_dates=True):
#     path = os.path.join(CSV_DIR, filename)
#     print(f"Loading {filename}...", flush=True)
#     df = pd.read_csv(path, parse_dates=[time_col] if parse_dates else False)
#     df[time_col] = pd.to_datetime(df[time_col])
#     df = df.set_index(time_col).sort_index()
#     print(f"  {len(df):,} rows  [{df.index[0]}  →  {df.index[-1]}]", flush=True)
#     return df

# def load_all():
#     bme   = load_csv('bme280.csv',   'timestamp')
#     ens   = load_csv('ens160.csv',   'timestamp')
#     spec  = load_csv('spectrum.csv', 'timestamp')
#     soil  = load_csv('soil.csv',     'timestamp')
#     soilt = load_csv('soiltemp.csv', 'timestamp')
#     snd   = load_csv('sound.csv',    'received_time')
#     bins  = load_csv('sound_bins_1h.csv', 'received_time')
#     return bme, ens, spec, soil, soilt, snd, bins

# # ── Helpers ───────────────────────────────────────────────────────────────────

# def window(df, start, end):
#     return df.loc[start:end]

# def remove_outliers(df, k=OUTLIER_IQR_K):
#     df = df.copy()
#     for col in df.select_dtypes(include=np.number).columns:
#         q1, q3 = df[col].quantile(0.25), df[col].quantile(0.75)
#         iqr = q3 - q1
#         if iqr == 0:
#             continue
#         lo, hi = q1 - k * iqr, q3 + k * iqr
#         n = ((df[col] < lo) | (df[col] > hi)).sum()
#         if n:
#             print(f"  Outliers [{col}]: {n} fenced", flush=True)
#         df.loc[(df[col] < lo) | (df[col] > hi), col] = np.nan
#     return df

# def resample_median(df, freq=RESAMPLE_FREQ):
#     return df.resample(freq).median()

# def fmt_xaxis(ax, fmt='%H:%M'):
#     ax.xaxis.set_major_formatter(mdates.DateFormatter(fmt))
#     plt.setp(ax.get_xticklabels(), rotation=0)

# # ── 5.5.1  Descriptive statistics table ──────────────────────────────────────

# def compute_descriptive(bme, ens, spec, soil, soilt, snd):
#     print("\n=== 5.5.1 Descriptive Statistics ===", flush=True)

#     parts = {
#         'Air temperature (°C)':      bme['temperature_c'],
#         'Relative humidity (%)':     bme['humidity_percent'],
#         'Pressure (hPa)':            bme['pressure_hpa'],
#         'eCO₂ (ppm)':               ens['eco2_ppm'],
#         'TVOC (ppb)':                ens['tvoc_ppb'],
#         'Substrate VWC (%)':         soil['soil_vwc_percent'],
#         'Substrate temp (°C)':       soilt['substrate_temp_c'],
#         'Broadband SPL (dBFS)':      snd['rms_dbfs'],
#         'AS7343 visible (mW m⁻²)':  spec['AS7343_visible'],
#         'AS7343 450 nm (mW m⁻²)':   spec['AS7343_450nm'],
#         'AS7343 515 nm (mW m⁻²)':   spec['AS7343_515nm'],
#         'AS7343 640 nm (mW m⁻²)':   spec['AS7343_640nm'],
#         'AS7343 690 nm (mW m⁻²)':   spec['AS7343_690nm'],
#     }

#     rows = []
#     for name, series in parts.items():
#         s = window(series, STATS_START, STATS_END).dropna()
#         rows.append({
#             'Modality': name,
#             'N':        len(s),
#             'Mean':     round(s.mean(), 3),
#             'Std':      round(s.std(),  3),
#             'Min':      round(s.min(),  3),
#             'P5':       round(s.quantile(0.05), 3),
#             'Median':   round(s.median(), 3),
#             'P95':      round(s.quantile(0.95), 3),
#             'Max':      round(s.max(),  3),
#         })

#     df_stats = pd.DataFrame(rows).set_index('Modality')
#     df_stats.to_csv(os.path.join(DATA_DIR, 'descriptive_stats.csv'))
#     print(df_stats.to_string(), flush=True)

#     # ── Figure: styled table ──
#     fig, ax = plt.subplots(figsize=(14, 5))
#     ax.axis('off')
#     tbl = ax.table(
#         cellText=df_stats.reset_index().values,
#         colLabels=['Modality'] + list(df_stats.columns),
#         loc='center',
#         cellLoc='center',
#     )
#     tbl.auto_set_font_size(False)
#     tbl.set_fontsize(8)
#     tbl.scale(1, 1.4)
#     # Header row styling
#     for j in range(len(df_stats.columns) + 1):
#         tbl[0, j].set_facecolor('#2c3e50')
#         tbl[0, j].set_text_props(color='white', fontweight='bold')
#     # Alternating row colours
#     for i in range(1, len(df_stats) + 1):
#         colour = '#f2f2f2' if i % 2 == 0 else 'white'
#         for j in range(len(df_stats.columns) + 1):
#             tbl[i, j].set_facecolor(colour)
#     ax.set_title(
#         f'Table 5.1. Deployment descriptive statistics '
#         f'({STATS_START[:10]} to {STATS_END[:10]})',
#         fontsize=10, pad=12, fontweight='bold'
#     )
#     plt.tight_layout()
#     out = os.path.join(OUTPUT_DIR, 'descriptive_stats_table.png')
#     plt.savefig(out)
#     plt.close()
#     print(f"  Saved: {out}", flush=True)
#     return df_stats

# # ── 5.5.2  Humidity FFT — actuator period ─────────────────────────────────────

# def actuator_period_fft(bme):
#     print("\n=== 5.5.2 Actuator Period FFT ===", flush=True)

#     # Use the 1-hour coherence window for the FFT
#     rh_win = window(bme['humidity_percent'], FFT_START, FFT_END).dropna()
#     # Resample to uniform 1-second grid then detrend
#     rh_1s   = rh_win.resample('1s').interpolate(method='time').ffill().bfill()
#     rh_vals = signal.detrend(rh_1s.values.astype(float))
#     dt_s    = 1.0
#     n       = len(rh_vals)
#     freqs   = rfftfreq(n, d=dt_s)
#     power   = np.abs(rfft(rh_vals)) ** 2

#     freqs_nz    = freqs[1:]
#     power_nz    = power[1:]
#     periods_min = (1.0 / freqs_nz) / 60.0

#     # Focus on 2–30 min (actuator plausible range for grow tent)
#     mask = (periods_min >= 2) & (periods_min <= 30)
#     if mask.sum() == 0:
#         print("  WARNING: no peaks in 2–30 min range", flush=True)
#         return None

#     pm       = periods_min[mask]
#     pw       = power_nz[mask]
#     peak_idx = np.argmax(pw)
#     dom_period = pm[peak_idx]
#     print(f"  Dominant actuator period: {dom_period:.1f} min", flush=True)

#     fig, axes = plt.subplots(2, 1, figsize=(9, 6))

#     # Top: raw RH time series over the 1-hour window
#     ax0 = axes[0]
#     ax0.plot(rh_win.index, rh_win.values, color='steelblue', linewidth=0.9)
#     ax0.set_ylabel('Relative humidity (%)')
#     ax0.set_title(f'BME280 Relative Humidity — {FFT_START[11:16]}–{FFT_END[11:16]} AEST  '
#                   f'({FFT_START[:10]})')
#     fmt_xaxis(ax0)
#     ax0.grid(True, alpha=0.3)

#     # Bottom: FFT power spectrum
#     ax1 = axes[1]
#     ax1.vlines(pm, 0, pw / pw.max(), color='steelblue', linewidth=0.8, alpha=0.7)
#     ax1.plot(pm, pw / pw.max(), 'o', color='steelblue', markersize=2)
#     ax1.axvline(dom_period, color='crimson', linestyle='--',
#                 label=f'Dominant period: {dom_period:.1f} min')
#     ax1.set_xlabel('Period (min)')
#     ax1.set_ylabel('Normalised power')
#     ax1.set_title('FFT Power Spectrum — Actuator Cycle Period Identification')
#     ax1.legend(fontsize=9)
#     ax1.set_xlim(2, 35)
#     ax1.grid(True, alpha=0.3)

#     plt.tight_layout()
#     out = os.path.join(OUTPUT_DIR, 'actuator_period_fft.png')
#     plt.savefig(out)
#     plt.close()
#     print(f"  Saved: {out}", flush=True)
#     return dom_period

# # ── 5.5.2  Acoustic actuator signature from bins ──────────────────────────────

# def decode_bins_df(bins_df):
#     """Decode bins_json column to numpy array per row. Returns df with 'bins' column."""
#     def _decode(row):
#         try:
#             b = json.loads(row['bins_json'])
#             return np.array(b, dtype=np.float32)  # already in dB SPL
#         except Exception:
#             return None
#     bins_df = bins_df.copy()
#     bins_df['bins_decoded'] = bins_df.apply(_decode, axis=1)
#     return bins_df[bins_df['bins_decoded'].notna()]

# def acoustic_actuator_signature(bme, bins_df):
#     print("\n=== 5.5.2 Acoustic Actuator Frequency Signature ===", flush=True)

#     # Work within the 1-hour coherence window
#     bme_w  = window(bme,     FFT_START, FFT_END)
#     bins_w = window(bins_df, FFT_START, FFT_END)

#     if len(bins_w) == 0:
#         print("  WARNING: no sound_bins rows in coherence window", flush=True)
#         return

#     bins_w = decode_bins_df(bins_w)
#     if len(bins_w) == 0:
#         print("  WARNING: all bins_json failed to decode", flush=True)
#         return

#     n_bins    = len(bins_w['bins_decoded'].iloc[0])
#     bin_freqs = 43 + np.arange(n_bins) * 43   # Hz

#     # Identify actuation events via rolling RH std
#     rh_std   = bme_w['humidity_percent'].resample('1min').std()
#     thresh   = rh_std.quantile(0.75)
#     act_times = rh_std[rh_std > thresh].index

#     # Label each bins row as actuation or ambient
#     is_act = pd.Series(False, index=bins_w.index)
#     for t in act_times:
#         is_act.loc[
#             (bins_w.index >= t - pd.Timedelta('90s')) &
#             (bins_w.index <= t + pd.Timedelta('90s'))
#         ] = True

#     act_rows = bins_w[is_act.values]
#     amb_rows = bins_w[~is_act.values]

#     print(f"  Actuation frames: {len(act_rows)}  |  Ambient frames: {len(amb_rows)}", flush=True)

#     if len(act_rows) == 0 or len(amb_rows) == 0:
#         print("  WARNING: insufficient frames for comparison", flush=True)
#         return

#     act_mean = np.mean(np.stack(act_rows['bins_decoded'].values), axis=0)
#     amb_mean = np.mean(np.stack(amb_rows['bins_decoded'].values), axis=0)
#     diff     = act_mean - amb_mean

#     # Dominant elevated frequency during actuation
#     dom_idx      = np.argmax(diff)
#     dom_freq_hz  = bin_freqs[dom_idx]
#     dom_elev_db  = diff[dom_idx]

#     # Peak frequency across individual actuation frames
#     peak_freqs = bin_freqs[
#         np.argmax(np.stack(act_rows['bins_decoded'].values), axis=1)
#     ]
#     print(f"  Dominant elevated frequency: {dom_freq_hz} Hz  "
#           f"(mean elevation: {dom_elev_db:.1f} dB above ambient)", flush=True)
#     print(f"  Peak freq stats: mean={peak_freqs.mean():.0f} Hz  "
#           f"std={peak_freqs.std():.0f} Hz  "
#           f"median={np.median(peak_freqs):.0f} Hz", flush=True)

#     # ── Figure: ambient vs actuation mean spectrum ──
#     fig, axes = plt.subplots(2, 1, figsize=(9, 7))

#     ax0 = axes[0]
#     ax0.plot(bin_freqs / 1000, amb_mean, color='steelblue',
#              linewidth=0.9, label='Ambient (non-actuation)')
#     ax0.plot(bin_freqs / 1000, act_mean, color='crimson',
#              linewidth=0.9, label='Actuation events')
#     ax0.axvline(dom_freq_hz / 1000, color='darkorange', linestyle='--',
#                 linewidth=0.9, label=f'Dominant elevated: {dom_freq_hz} Hz')
#     ax0.set_ylabel('Mean dB SPL')
#     ax0.set_title('Acoustic Frequency Profile: Actuation Events vs Ambient\n'
#                 f'(2026-05-24  20:45–21:45 AEST)')
#     ax0.legend(fontsize=9)
#     ax0.grid(True, alpha=0.3)
#     ax0.set_xlim(0, bin_freqs.max() / 1000)

#     ax1 = axes[1]
#     pos_mask = diff > 0
#     ax1.bar(bin_freqs[pos_mask] / 1000, diff[pos_mask],
#             width=0.035, color='crimson', alpha=0.8)
#     ax1.axhline(0, color='black', linewidth=0.6)
#     ax1.axvline(dom_freq_hz / 1000, color='darkorange', linestyle='--',
#                 linewidth=0.9, label=f'{dom_freq_hz} Hz  (+{dom_elev_db:.1f} dB)')
#     ax1.set_xlabel('Frequency (kHz)')
#     ax1.set_ylabel('SPL elevation (dB)\nActuation − Ambient')
#     ax1.set_title('Per-Bin SPL Elevation During Actuation Events')
#     ax1.legend(fontsize=9)
#     ax1.grid(True, alpha=0.3)
#     ax1.set_xlim(0, bin_freqs.max() / 1000)

#     plt.tight_layout()
#     out = os.path.join(OUTPUT_DIR, 'actuator_acoustic_signature.png')
#     plt.savefig(out)
#     plt.close()
#     print(f"  Saved: {out}", flush=True)

#     # Save stats
#     with open(os.path.join(DATA_DIR, 'actuator_acoustic_stats.txt'), 'w') as f:
#         f.write(f"Dominant elevated frequency: {dom_freq_hz} Hz\n")
#         f.write(f"Mean SPL elevation at dominant bin: {dom_elev_db:.2f} dB\n")
#         f.write(f"Peak freq across actuation frames: "
#                 f"mean={peak_freqs.mean():.0f} Hz  "
#                 f"std={peak_freqs.std():.0f} Hz  "
#                 f"median={np.median(peak_freqs):.0f} Hz\n")

#     return dom_freq_hz, dom_elev_db, peak_freqs.mean(), peak_freqs.std()

# # ── 5.5.2  Spectral channel disturbance profile ───────────────────────────────

# def spectral_actuator_signature(spec, bme):
#     print("\n=== 5.5.2 Spectral Actuator Disturbance — Channel Comparison ===", flush=True)

#     # spec_w = window(spec, FFT_START, FFT_END)
#     # bme_w  = window(bme,  FFT_START, FFT_END)
    
#     spec_w = window(spec, SPECTRAL_START, SPECTRAL_END)
#     bme_w  = window(bme,  SPECTRAL_START, SPECTRAL_END)
    
#     # SPECTRAL_START, SPECTRAL_END these are my new macros

#     channels = {
#         '405 nm': 'AS7343_405nm', '425 nm': 'AS7343_425nm',
#         '450 nm': 'AS7343_450nm', '475 nm': 'AS7343_475nm',
#         '515 nm': 'AS7343_515nm', '550 nm': 'AS7343_550nm',
#         '555 nm': 'AS7343_555nm', '600 nm': 'AS7343_600nm',
#         '640 nm': 'AS7343_640nm', '690 nm': 'AS7343_690nm',
#         '745 nm': 'AS7343_745nm', '855 nm': 'AS7343_855nm',
#     }
#     colours = [
#         '#7B00FF','#6600FF','#0000FF','#0055FF','#00AAFF',
#         '#00FF55','#00FF00','#AAFF00','#FFAA00','#FF5500',
#         '#FF0000','#880000',
#     ]

#     # Identify actuation events via rolling RH std
#     rh_std    = bme_w['humidity_percent'].resample('1min').std()
#     thresh    = rh_std.quantile(0.75)
#     act_times = rh_std[rh_std > thresh].index

#     is_act = pd.Series(False, index=spec_w.index)
#     for t in act_times:
#         is_act.loc[
#             (spec_w.index >= t - pd.Timedelta('90s')) &
#             (spec_w.index <= t + pd.Timedelta('90s'))
#         ] = True

#     act_rows = spec_w[is_act.values]
#     amb_rows = spec_w[~is_act.values]

#     print(f"  Actuation frames: {len(act_rows)}  |  Ambient frames: {len(amb_rows)}", flush=True)

#     labels    = list(channels.keys())
#     act_means = [act_rows[col].mean() for col in channels.values()]
#     amb_means = [amb_rows[col].mean() for col in channels.values()]
#     pct_elev  = [(a - b) / b * 100 if b > 0 else 0
#                  for a, b in zip(act_means, amb_means)]

#     for lbl, pct in zip(labels, pct_elev):
#         print(f"  {lbl}: {pct:+.2f}% elevation during actuation", flush=True)

#     fig, axes = plt.subplots(2, 1, figsize=(11, 8))

#     x     = np.arange(len(labels))
#     width = 0.35
#     ax0   = axes[0]
#     ax0.bar(x - width/2, amb_means, width, label='Ambient',
#             color=[c + '99' for c in colours], edgecolor=colours, linewidth=0.8)
#     ax0.bar(x + width/2, act_means, width, label='Actuation',
#             color=colours, edgecolor=colours, linewidth=0.8, alpha=0.9)
#     ax0.set_xticks(x)
#     ax0.set_xticklabels(labels, rotation=45, ha='right', fontsize=8)
#     ax0.set_ylabel('Mean irradiance (mW m⁻²)')
#     ax0.set_title('AS7343 Per-Channel Mean Irradiance: Actuation vs Ambient\n'
#               f'({SPECTRAL_START[:10]} to {SPECTRAL_END[:10]})')
#     ax0.legend(fontsize=9)
#     ax0.grid(True, alpha=0.3, axis='y')

#     ax1 = axes[1]
#     bar_colours = ['crimson' if p > 0 else 'steelblue' for p in pct_elev]
#     ax1.bar(x, pct_elev, width=0.6, color=bar_colours, alpha=0.85)
#     ax1.axhline(0, color='black', linewidth=0.7)
#     ax1.set_xticks(x)
#     ax1.set_xticklabels(labels, rotation=45, ha='right', fontsize=8)
#     ax1.set_ylabel('Irradiance elevation (%)\nActuation − Ambient')
#     ax1.set_title('Per-Channel Irradiance Elevation During Actuation Events')
#     ax1.grid(True, alpha=0.3, axis='y')

#     plt.tight_layout()
#     out = os.path.join(OUTPUT_DIR, 'spectral_actuator_channels.png')
#     plt.savefig(out)
#     plt.close()
#     print(f"  Saved: {out}", flush=True)

# # ── Spearman correlation heatmap ──────────────────────────────────────────────

# def correlation_heatmap(bme, ens, spec, soil, soilt, snd):
#     print("\n=== Spearman Correlation Heatmaps ===", flush=True)

#     # ── Heatmap 1: cross-modality ──
#     combined = pd.concat([
#         window(bme['temperature_c'],      STATS_START, STATS_END).rename('Air temp'),
#         window(bme['humidity_percent'],   STATS_START, STATS_END).rename('RH'),
#         window(bme['pressure_hpa'],       STATS_START, STATS_END).rename('Pressure'),
#         window(ens['eco2_ppm'],           STATS_START, STATS_END).rename('eCO₂'),
#         window(ens['tvoc_ppb'],           STATS_START, STATS_END).rename('TVOC'),
#         window(soil['soil_vwc_percent'],  STATS_START, STATS_END).rename('VWC'),
#         window(soilt['substrate_temp_c'], STATS_START, STATS_END).rename('Substrate temp'),
#         window(spec['AS7343_visible'],    STATS_START, STATS_END).rename('Visible irr.'),
#         window(snd['rms_dbfs'],           STATS_START, STATS_END).rename('SPL (dBFS)'),
#     ], axis=1).resample(RESAMPLE_FREQ).median().dropna()

#     corr1 = combined.corr(method='spearman')
#     corr1.to_csv(os.path.join(DATA_DIR, 'spearman_cross_modality.csv'))

#     _plot_heatmap(
#         corr1,
#         title=f'Spearman Correlation — Cross-Modality\n'
#               f'({STATS_START[:10]} to {STATS_END[:10]}, 5 s resampled)',
#         outfile='correlation_heatmap_crossmodality.png',
#         figsize=(9, 7),
#         fontsize=9,
#     )

#     # ── Heatmap 2: AS7343 inter-channel ──
#     spec_channels = {
#         '405 nm': 'AS7343_405nm', '425 nm': 'AS7343_425nm',
#         '450 nm': 'AS7343_450nm', '475 nm': 'AS7343_475nm',
#         '515 nm': 'AS7343_515nm', '550 nm': 'AS7343_550nm',
#         '555 nm': 'AS7343_555nm', '600 nm': 'AS7343_600nm',
#         '640 nm': 'AS7343_640nm', '690 nm': 'AS7343_690nm',
#         '745 nm': 'AS7343_745nm', '855 nm': 'AS7343_855nm',
#     }
#     spec_combined = pd.concat([
#         window(spec[col], STATS_START, STATS_END).rename(label)
#         for label, col in spec_channels.items()
#     ], axis=1).resample(RESAMPLE_FREQ).median().dropna()

#     corr2 = spec_combined.corr(method='spearman')  

#     _plot_heatmap(
#         corr2,
#         title=f'Spearman Correlation — AS7343 Spectral Channels\n'
#             f'({STATS_START[:10]} to {STATS_END[:10]}, 5 s resampled)',
#         outfile='correlation_heatmap_spectral.png',
#         figsize=(18, 15),
#         fontsize=8,
#     )

#     print("  Saved both heatmaps.", flush=True)
#     return corr1


# def _plot_heatmap(corr, title, outfile, figsize=(10, 8), fontsize=9):
#     try:
#         import seaborn as sns
#         fig, ax = plt.subplots(figsize=figsize)
#         mask = corr.isnull()
#         sns.heatmap(
#             corr, annot=True, fmt='.2f', cmap='RdBu_r',
#             center=0, vmin=-1, vmax=1,
#             linewidths=0.5, ax=ax,
#             annot_kws={'size': fontsize},
#             square=False,
#             mask=mask,
#         )
#         # Force annotation text colour based on cell intensity
#         for text in ax.texts:
#             try:
#                 val = float(text.get_text())
#                 text.set_color('white' if abs(val) > 0.6 else 'black')
#             except ValueError:
#                 pass
#         ax.set_title(title, fontsize=10)
#         plt.tight_layout()
#         plt.savefig(os.path.join(OUTPUT_DIR, outfile), dpi=200, bbox_inches='tight')
#         plt.close()
#     except ImportError:
#         print("  seaborn not found — pip install seaborn", flush=True)

# # ── Summary stats text ────────────────────────────────────────────────────────

# def write_summary(stats_df, dom_period, corr):
#     lines = [
#         "=" * 60,
#         "SECTION 5.5 ANALYSIS SUMMARY",
#         f"Stats window:     {STATS_START}  →  {STATS_END}",
#         f"FFT/coherence window: {FFT_START}  →  {FFT_END}",
#         "=" * 60,
#         "",
#         "5.5.1  DESCRIPTIVE STATISTICS",
#         stats_df.to_string(),
#         "",
#         "5.5.2  ACTUATOR PERIOD",
#         f"  Dominant actuator cycle period (humidity FFT): "
#         f"{dom_period:.1f} min" if dom_period else "  FFT: no dominant period found",
#         "",
#         "SPEARMAN CORRELATIONS (selected):",
#     ]
#     highlights = [
#         ('Air temp',      'Substrate temp'),
#         ('RH',            'eCO₂'),
#         ('RH',            'TVOC'),
#         ('RH',            'Visible irr.'),
#         ('Visible irr.',  '690 nm'),
#         ('SPL (dBFS)',     'eCO₂'),
#     ]
#     for a, b in highlights:
#         if a in corr.columns and b in corr.columns:
#             lines.append(f"  {a:20s} vs {b:20s}  r={corr.loc[a,b]:.3f}")
#     lines += [
#         "",
#         "OUTPUT FILES",
#         f"  {OUTPUT_DIR}/descriptive_stats_table.png",
#         f"  {OUTPUT_DIR}/actuator_period_fft.png",
#         f"  {OUTPUT_DIR}/actuator_acoustic_signature.png",
#         f"  {OUTPUT_DIR}/spectral_actuator_channels.png",
#         f"  {OUTPUT_DIR}/correlation_heatmap.png",
#         f"  {DATA_DIR}/descriptive_stats.csv",
#         f"  {DATA_DIR}/spearman_correlation_matrix.csv",
#         f"  {DATA_DIR}/actuator_acoustic_stats.txt",
#         "=" * 60,
#     ]
#     summary = "\n".join(lines)
#     with open(os.path.join(DATA_DIR, 'summary_stats.txt'), 'w') as f:
#         f.write(summary)
#     print("\n" + summary, flush=True)

# # ── Main ──────────────────────────────────────────────────────────────────────

# def main():
#     print(f"Stats window:         {STATS_START}  →  {STATS_END}")
#     print(f"FFT/coherence window: {FFT_START}  →  {FFT_END}")

#     bme, ens, spec, soil, soilt, snd, bins = load_all()

#     print("\n--- Outlier removal ---", flush=True)
#     bme   = remove_outliers(bme)
#     ens   = remove_outliers(ens)
#     spec  = remove_outliers(spec)
#     soil  = remove_outliers(soil)
#     soilt = remove_outliers(soilt)
#     snd   = remove_outliers(snd)

#     stats_df   = compute_descriptive(bme, ens, spec, soil, soilt, snd)
#     dom_period = actuator_period_fft(bme)
#     acoustic_actuator_signature(bme, bins)
#     spectral_actuator_signature(spec, bme)
#     corr = correlation_heatmap(bme, ens, spec, soil, soilt, snd)
#     write_summary(stats_df, dom_period, corr)

#     print("\n=== Complete ===", flush=True)
#     print(f"Figures → {OUTPUT_DIR}/")
#     print(f"Data    → {DATA_DIR}/")

# if __name__ == '__main__':
#     main()

"""
Section 5.5 Analysis Script — CSV Version
==========================================
Loads from locally exported CSVs rather than querying Azure SQL.
All figures written to ./figures_csv/
All data written to ./data_csv/

Expected CSV files (put in same folder as this script or set CSV_DIR):
    bme280.csv      — timestamp, temperature_c, humidity_percent, pressure_hpa
    ens160.csv      — timestamp, eco2_ppm, tvoc_ppb, aqi
    spectrum.csv    — timestamp, AS7343_405nm ... AS7343_visible
    soil.csv        — timestamp, soil_vwc_percent
    soiltemp.csv    — timestamp, substrate_temp_c
    sound.csv       — received_time, rms_dbfs
    sound_bins.csv  — received_time, rms_dbfs, bins_json  (1 row/min sample)

Usage:
    python3 analyse_from_csv.py
"""

import os
import json
import warnings
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.gridspec import GridSpec
from scipy import signal, stats
from scipy.fft import rfft, rfftfreq

warnings.filterwarnings('ignore')

# ── MACROS — edit these ───────────────────────────────────────────────────────

CSV_DIR    = '.'
OUTPUT_DIR = './figures_csv'
DATA_DIR   = './data_csv'

# Full stats window — match whatever is in your CSVs
STATS_START = '2026-05-21 00:00:00'
STATS_END   = '2026-05-27 00:00:00'

# 1-hour coherence window for the FFT and zoomed plots
# FFT_START   = '2026-05-24 10:45:00'
# FFT_END     = '2026-05-24 11:45:00'å

FFT_START = '2026-05-21 00:00:00'
FFT_END   = '2026-05-27 00:00:00'

# Add at the top with the other macros
SPECTRAL_START = '2026-05-21 00:00:00'
SPECTRAL_END   = '2026-05-27 00:00:00'

# Outlier IQR fence multiplier (3.0 = conservative, keeps most data)
OUTLIER_IQR_K = 3.0

# Resample resolution for correlation work
RESAMPLE_FREQ = '5s'

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(DATA_DIR,   exist_ok=True)

plt.rcParams.update({
    'font.family':  'DejaVu Sans',
    'font.size':    10,
    'figure.dpi':   150,
    'savefig.dpi':  200,
    'savefig.bbox': 'tight',
})

# ── Load CSVs ─────────────────────────────────────────────────────────────────

def load_csv(filename, time_col, parse_dates=True):
    path = os.path.join(CSV_DIR, filename)
    print(f"Loading {filename}...", flush=True)
    df = pd.read_csv(path, parse_dates=[time_col] if parse_dates else False)
    df[time_col] = pd.to_datetime(df[time_col])
    df = df.set_index(time_col).sort_index()
    print(f"  {len(df):,} rows  [{df.index[0]}  →  {df.index[-1]}]", flush=True)
    return df

def load_all():
    bme   = load_csv('bme280.csv',   'timestamp')
    ens   = load_csv('ens160.csv',   'timestamp')
    spec  = load_csv('spectrum.csv', 'timestamp')
    soil  = load_csv('soil.csv',     'timestamp')
    soilt = load_csv('soiltemp.csv', 'timestamp')
    snd   = load_csv('sound.csv',    'received_time')
    bins  = load_csv('sound_bins_1h.csv', 'received_time')
    return bme, ens, spec, soil, soilt, snd, bins

# ── Helpers ───────────────────────────────────────────────────────────────────

def window(df, start, end):
    return df.loc[start:end]

def remove_outliers(df, k=3.0):
    skip_iqr = {
        'temperature_c', 'substrate_temp_c',
        'humidity_percent', 'soil_vwc_percent', 'rms_dbfs',
    }

    spike_multiplier = {
        'temperature_c':    10.0,
        'substrate_temp_c': 10.0,
        'pressure_hpa':     10.0,
        'humidity_percent': 20.0,
        'soil_vwc_percent': 20.0,
        'rms_dbfs':         20.0,
    }
    default_multiplier = 50.0

    median_window = {
        'temperature_c':    11,
        'substrate_temp_c': 11,
        'pressure_hpa':     11,
        'humidity_percent': 11,
        'soil_vwc_percent': 11,
        'rms_dbfs':          7,
    }

    df = df.copy()
    for col in df.select_dtypes(include='number').columns:
        # IQR fence — skipped for slow-drifting physical channels
        if col not in skip_iqr:
            q1, q3 = df[col].quantile(0.25), df[col].quantile(0.75)
            iqr = q3 - q1
            if iqr > 0:
                lo, hi = q1 - k * iqr, q3 + k * iqr
                n = ((df[col] < lo) | (df[col] > hi)).sum()
                if n:
                    print(f"  IQR [{col}]: {n} fenced", flush=True)
                df.loc[(df[col] < lo) | (df[col] > hi), col] = float('nan')

        # Median filter — catches multi-sample spikes
        win = median_window.get(col, 0)
        if win > 0:
            rolling_median = df[col].rolling(win, center=True, min_periods=3).median()
            deviation = (df[col] - rolling_median).abs()
            local_mad = deviation.rolling(win * 10, center=True, min_periods=10).median()
            spike_mask = deviation > (local_mad * 10).clip(lower=0.5)
            n = spike_mask.sum()
            if n:
                print(f"  Median filter [{col}]: {n} removed", flush=True)
            df.loc[spike_mask, col] = float('nan')
        else:
            # Bilateral spike filter for non-windowed columns
            s = df[col]
            jump_from_prev = s.diff().abs()
            jump_to_next   = s.diff(-1).abs()
            median_jump    = jump_from_prev.median()
            if median_jump > 0:
                mult      = spike_multiplier.get(col, default_multiplier)
                threshold = median_jump * mult
                is_spike  = (jump_from_prev > threshold) & (jump_to_next > threshold)
                n = is_spike.sum()
                if n:
                    print(f"  Spike [{col}]: {n} removed", flush=True)
                df.loc[is_spike, col] = float('nan')

    return df

def resample_median(df, freq=RESAMPLE_FREQ):
    return df.resample(freq).median()

def fmt_xaxis(ax, fmt='%H:%M'):
    ax.xaxis.set_major_formatter(mdates.DateFormatter(fmt))
    plt.setp(ax.get_xticklabels(), rotation=0)

# ── 5.5.1  Descriptive statistics table ──────────────────────────────────────
def compute_descriptive(bme, ens, spec, soil, soilt, snd, bins):
    print("\n=== 5.5.1 Descriptive Statistics ===", flush=True)

    # ── Peak frequency from bins ──
    def get_peak_freq(row):
        try:
            b = json.loads(row['bins_json'])
            arr = np.array(b, dtype=np.float32)
            return float(43 + np.argmax(arr) * 43)
        except Exception:
            return np.nan

    bins_w = window(bins, STATS_START, STATS_END).copy()
    bins_w['peak_freq_hz'] = bins_w.apply(get_peak_freq, axis=1)

    parts = {
        'Air temperature (°C)':       bme['temperature_c'],
        'Relative humidity (%)':      bme['humidity_percent'],
        'Pressure (hPa)':            bme['pressure_hpa'] * 10,
        'eCO₂ (ppm)':                ens['eco2_ppm'],
        'TVOC (ppb)':                 ens['tvoc_ppb'],
        'Substrate VWC (%)':          soil['soil_vwc_percent'],
        'Substrate temp (°C)':        soilt['substrate_temp_c'],
        'Broadband SPL (dBFS)':       snd['rms_dbfs'],
        'Peak frequency (Hz)':        bins_w['peak_freq_hz'],
        'AS7343 405 nm (mW m⁻²)':    spec['AS7343_405nm'],
        'AS7343 425 nm (mW m⁻²)':    spec['AS7343_425nm'],
        'AS7343 450 nm (mW m⁻²)':    spec['AS7343_450nm'],
        'AS7343 475 nm (mW m⁻²)':    spec['AS7343_475nm'],
        'AS7343 515 nm (mW m⁻²)':    spec['AS7343_515nm'],
        'AS7343 550 nm (mW m⁻²)':    spec['AS7343_550nm'],
        'AS7343 555 nm (mW m⁻²)':    spec['AS7343_555nm'],
        'AS7343 600 nm (mW m⁻²)':    spec['AS7343_600nm'],
        'AS7343 640 nm (mW m⁻²)':    spec['AS7343_640nm'],
        'AS7343 690 nm (mW m⁻²)':    spec['AS7343_690nm'],
        'AS7343 745 nm (mW m⁻²)':    spec['AS7343_745nm'],
        'AS7343 855 nm (mW m⁻²)':    spec['AS7343_855nm'],
        'AS7343 visible (mW m⁻²)':   spec['AS7343_visible'],
    }

    rows = []
    for name, series in parts.items():
        s = window(series, STATS_START, STATS_END).dropna()
        rows.append({
            'Modality': name,
            'N':        len(s),
            'Mean':     round(s.mean(), 3),
            'Std':      round(s.std(),  3),
            'Min':      round(s.min(),  3),
            'P5':       round(s.quantile(0.05), 3),
            'Median':   round(s.median(), 3),
            'P95':      round(s.quantile(0.95), 3),
            'Max':      round(s.max(),  3),
        })

    df_stats = pd.DataFrame(rows).set_index('Modality')
    df_stats.to_csv(os.path.join(DATA_DIR, 'descriptive_stats.csv'))
    print(df_stats.to_string(), flush=True)

    # ── Figure: styled table — split into two panels for readability ──
    fig, axes = plt.subplots(2, 1, figsize=(16, 10))
    splits = [df_stats.iloc[:9], df_stats.iloc[9:]]
    subtitles = ['Environmental and acoustic modalities', 'AS7343 spectral channels']

    for ax, subset, subtitle in zip(axes, splits, subtitles):
        ax.axis('off')
        tbl = ax.table(
            cellText=subset.reset_index().values,
            colLabels=['Modality'] + list(subset.columns),
            loc='center',
            cellLoc='center',
        )
        tbl.auto_set_font_size(False)
        tbl.set_fontsize(8)
        tbl.scale(1, 1.5)
        for j in range(len(subset.columns) + 1):
            tbl[0, j].set_facecolor('#2c3e50')
            tbl[0, j].set_text_props(color='white', fontweight='bold')
        for i in range(1, len(subset) + 1):
            colour = '#f2f2f2' if i % 2 == 0 else 'white'
            for j in range(len(subset.columns) + 1):
                tbl[i, j].set_facecolor(colour)
        ax.set_title(subtitle, fontsize=9, pad=6, style='italic')

    fig.suptitle(
        f'Table 5.1. Deployment descriptive statistics '
        f'({STATS_START[:10]} to {STATS_END[:10]})',
        fontsize=11, fontweight='bold', y=1.01
    )
    plt.tight_layout()
    out = os.path.join(OUTPUT_DIR, 'descriptive_stats_table.png')
    plt.savefig(out, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {out}", flush=True)
    return df_stats
# ── 5.5.2  Humidity FFT — actuator period ─────────────────────────────────────

def actuator_period_fft(bme):
    print("\n=== 5.5.2 Actuator Period FFT ===", flush=True)

    import pytz
    aest = pytz.timezone('Australia/Brisbane')

    # Use the 1-hour coherence window for the FFT
    rh_win = window(bme['humidity_percent'], FFT_START, FFT_END).dropna()
    # Resample to uniform 1-second grid then detrend
    rh_1s   = rh_win.resample('1s').interpolate(method='time').ffill().bfill()
    rh_vals = signal.detrend(rh_1s.values.astype(float))
    dt_s    = 1.0
    n       = len(rh_vals)
    freqs   = rfftfreq(n, d=dt_s)
    power   = np.abs(rfft(rh_vals)) ** 2

    freqs_nz    = freqs[1:]
    power_nz    = power[1:]
    periods_min = (1.0 / freqs_nz) / 60.0

    # Focus on 2–15 min — restricts to periods with 4+ cycles in the window
    mask = (periods_min >= 2) & (periods_min <= 15)
    if mask.sum() == 0:
        print("  WARNING: no peaks in 2–15 min range", flush=True)
        return None

    pm         = periods_min[mask]
    pw         = power_nz[mask]
    peak_idx   = np.argmax(pw)
    dom_period = pm[peak_idx]
    print(f"  Dominant actuator period: {dom_period:.1f} min", flush=True)

    # Convert index to AEST for display
    rh_aest = rh_win.copy()
    rh_aest.index = rh_win.index.tz_localize('UTC').tz_convert(aest)

    # AEST start/end for title
    t_start = rh_aest.index[0].strftime('%H:%M')
    t_end   = rh_aest.index[-1].strftime('%H:%M')
    date_str = rh_aest.index[0].strftime('%Y-%m-%d')

    fig, axes = plt.subplots(2, 1, figsize=(9, 6))

    # Top: raw RH time series in AEST
    ax0 = axes[0]
    ax0.plot(rh_aest.index, rh_aest.values, color='steelblue', linewidth=0.9)
    ax0.set_ylabel('Relative humidity (%)')
    ax0.set_xlabel('Time (UTC)')
    ax0.set_title(f'BME280 Relative Humidity — {date_str}  {t_start}–{t_end} AEST')
    ax0.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    ax0.xaxis.set_major_locator(mdates.AutoDateLocator())
    plt.setp(ax0.get_xticklabels(), rotation=0, ha='center', fontsize=8)
    ax0.grid(True, alpha=0.3)

    # Bottom: FFT power spectrum
    ax1 = axes[1]
    ax1.vlines(pm, 0, pw / pw.max(), color='steelblue', linewidth=0.8, alpha=0.7)
    ax1.plot(pm, pw / pw.max(), 'o', color='steelblue', markersize=2)
    ax1.axvline(dom_period, color='crimson', linestyle='--',
                label=f'Dominant period: {dom_period:.1f} min')
    ax1.set_xlabel('Period (min)')
    ax1.set_ylabel('Normalised power')
    ax1.set_title('FFT Power Spectrum — Actuator Cycle Period Identification')
    ax1.legend(fontsize=9)
    ax1.set_xlim(2, 15)
    ax1.grid(True, alpha=0.3)

    plt.tight_layout()
    out = os.path.join(OUTPUT_DIR, 'actuator_period_fft.png')
    plt.savefig(out)
    plt.close()
    print(f"  Saved: {out}", flush=True)
    return dom_period

# ── 5.5.2  Acoustic actuator signature from bins ──────────────────────────────

def decode_bins_df(bins_df):
    """Decode bins_json column to numpy array per row. Returns df with 'bins' column."""
    def _decode(row):
        try:
            b = json.loads(row['bins_json'])
            return np.array(b, dtype=np.float32)  # already in dB SPL
        except Exception:
            return None
    bins_df = bins_df.copy()
    bins_df['bins_decoded'] = bins_df.apply(_decode, axis=1)
    return bins_df[bins_df['bins_decoded'].notna()]

def acoustic_actuator_signature(bme, bins_df):
    print("\n=== 5.5.2 Acoustic Actuator Frequency Signature ===", flush=True)

    # Work within the 1-hour coherence window
    bme_w  = window(bme,     FFT_START, FFT_END)
    bins_w = window(bins_df, FFT_START, FFT_END)

    if len(bins_w) == 0:
        print("  WARNING: no sound_bins rows in coherence window", flush=True)
        return

    bins_w = decode_bins_df(bins_w)
    if len(bins_w) == 0:
        print("  WARNING: all bins_json failed to decode", flush=True)
        return

    n_bins    = len(bins_w['bins_decoded'].iloc[0])
    bin_freqs = 43 + np.arange(n_bins) * 43   # Hz

    # Identify actuation events via rolling RH std
    rh_std   = bme_w['humidity_percent'].resample('1min').std()
    thresh   = rh_std.quantile(0.75)
    act_times = rh_std[rh_std > thresh].index

    # Label each bins row as actuation or ambient
    is_act = pd.Series(False, index=bins_w.index)
    for t in act_times:
        is_act.loc[
            (bins_w.index >= t - pd.Timedelta('90s')) &
            (bins_w.index <= t + pd.Timedelta('90s'))
        ] = True

    act_rows = bins_w[is_act.values]
    amb_rows = bins_w[~is_act.values]

    print(f"  Actuation frames: {len(act_rows)}  |  Ambient frames: {len(amb_rows)}", flush=True)

    if len(act_rows) == 0 or len(amb_rows) == 0:
        print("  WARNING: insufficient frames for comparison", flush=True)
        return

    act_mean = np.mean(np.stack(act_rows['bins_decoded'].values), axis=0)
    amb_mean = np.mean(np.stack(amb_rows['bins_decoded'].values), axis=0)
    diff     = act_mean - amb_mean

    # Dominant elevated frequency during actuation
    dom_idx      = np.argmax(diff)
    dom_freq_hz  = bin_freqs[dom_idx]
    dom_elev_db  = diff[dom_idx]

    # Peak frequency across individual actuation frames
    peak_freqs = bin_freqs[
        np.argmax(np.stack(act_rows['bins_decoded'].values), axis=1)
    ]
    print(f"  Dominant elevated frequency: {dom_freq_hz} Hz  "
          f"(mean elevation: {dom_elev_db:.1f} dB above ambient)", flush=True)
    print(f"  Peak freq stats: mean={peak_freqs.mean():.0f} Hz  "
          f"std={peak_freqs.std():.0f} Hz  "
          f"median={np.median(peak_freqs):.0f} Hz", flush=True)

    # ── Figure: ambient vs actuation mean spectrum ──
    fig, axes = plt.subplots(2, 1, figsize=(9, 7))

    ax0 = axes[0]
    ax0.plot(bin_freqs / 1000, amb_mean, color='steelblue',
             linewidth=0.9, label='Ambient (non-actuation)')
    ax0.plot(bin_freqs / 1000, act_mean, color='crimson',
             linewidth=0.9, label='Actuation events')
    ax0.axvline(dom_freq_hz / 1000, color='darkorange', linestyle='--',
                linewidth=0.9, label=f'Dominant elevated: {dom_freq_hz} Hz')
    ax0.set_ylabel('Mean dB SPL')
    ax0.set_title('Acoustic Frequency Profile: Actuation Events vs Ambient\n'
                f'(2026-05-21 to 2026-05-27)')
    ax0.legend(fontsize=9)
    ax0.grid(True, alpha=0.3)
    ax0.set_xlim(0, bin_freqs.max() / 1000)

    ax1 = axes[1]
    pos_mask = diff > 0
    ax1.bar(bin_freqs[pos_mask] / 1000, diff[pos_mask],
            width=0.035, color='crimson', alpha=0.8)
    ax1.axhline(0, color='black', linewidth=0.6)
    ax1.axvline(dom_freq_hz / 1000, color='darkorange', linestyle='--',
                linewidth=0.9, label=f'{dom_freq_hz} Hz  (+{dom_elev_db:.1f} dB)')
    ax1.set_xlabel('Frequency (kHz)')
    ax1.set_ylabel('SPL elevation (dB)\nActuation − Ambient')
    ax1.set_title('Per-Bin SPL Elevation During Actuation Events')
    ax1.legend(fontsize=9)
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(0, bin_freqs.max() / 1000)

    plt.tight_layout()
    out = os.path.join(OUTPUT_DIR, 'actuator_acoustic_signature.png')
    plt.savefig(out)
    plt.close()
    print(f"  Saved: {out}", flush=True)

    # Save stats
    with open(os.path.join(DATA_DIR, 'actuator_acoustic_stats.txt'), 'w') as f:
        f.write(f"Dominant elevated frequency: {dom_freq_hz} Hz\n")
        f.write(f"Mean SPL elevation at dominant bin: {dom_elev_db:.2f} dB\n")
        f.write(f"Peak freq across actuation frames: "
                f"mean={peak_freqs.mean():.0f} Hz  "
                f"std={peak_freqs.std():.0f} Hz  "
                f"median={np.median(peak_freqs):.0f} Hz\n")

    return dom_freq_hz, dom_elev_db, peak_freqs.mean(), peak_freqs.std()

# ── 5.5.2  Spectral channel disturbance profile ───────────────────────────────

def spectral_actuator_signature(spec):
    print("\n=== 5.5.2 Spectral Actuator Disturbance ===", flush=True)

    spec_w = window(spec, SPECTRAL_START, SPECTRAL_END)

    channels = {
        '405 nm': 'AS7343_405nm', '425 nm': 'AS7343_425nm',
        '450 nm': 'AS7343_450nm', '475 nm': 'AS7343_475nm',
        '515 nm': 'AS7343_515nm', '550 nm': 'AS7343_550nm',
        '555 nm': 'AS7343_555nm', '600 nm': 'AS7343_600nm',
        '640 nm': 'AS7343_640nm', '690 nm': 'AS7343_690nm',
        '745 nm': 'AS7343_745nm', '855 nm': 'AS7343_855nm',
    }
    colours = [
        '#7B00FF','#6600FF','#0000FF','#0055FF','#00AAFF',
        '#00FF55','#00FF00','#AAFF00','#FFAA00','#FF5500',
        '#FF0000','#880000',
    ]

    fig, axes = plt.subplots(4, 3, figsize=(13, 10), sharex=True)
    axes_flat = axes.flatten()

    for idx, (label, col) in enumerate(channels.items()):
        ax = axes_flat[idx]
        if col not in spec_w.columns:
            ax.set_visible(False)
            continue
        s = spec_w[col].dropna()
        ax.plot(s.index, s.values, color=colours[idx], linewidth=0.8)
        ax.set_title(label, fontsize=9, fontweight='bold')
        ax.set_ylabel('mW m⁻²', fontsize=7)
        ax.grid(True, alpha=0.25)
        fmt_xaxis(ax)

    fig.suptitle(
        f'AS7343 Per-Channel Irradiance During Actuator Events\n'
        f'{FFT_START[:10]}  {FFT_START[11:16]}–{FFT_END[11:16]} AEST',
        fontsize=11, fontweight='bold'
    )
    plt.tight_layout()
    out = os.path.join(OUTPUT_DIR, 'spectral_actuator_channels.png')
    plt.savefig(out)
    plt.close()
    print(f"  Saved: {out}", flush=True)

# ── Spearman correlation heatmap ──────────────────────────────────────────────

def correlation_heatmap(bme, ens, spec, soil, soilt, snd):
    print("\n=== Spearman Correlation Heatmap ===", flush=True)

    CORR_FREQ = '30s'

    def prep(series, start, end):
        return window(series, start, end).resample(CORR_FREQ).median()

    combined = pd.concat([
        prep(bme['temperature_c'],      STATS_START, STATS_END).rename('Air temp'),
        prep(bme['humidity_percent'],   STATS_START, STATS_END).rename('RH'),
        prep(bme['pressure_hpa'],       STATS_START, STATS_END).rename('Pressure'),
        prep(ens['eco2_ppm'],           STATS_START, STATS_END).rename('eCO2'),
        prep(ens['tvoc_ppb'],           STATS_START, STATS_END).rename('TVOC'),
        prep(soil['soil_vwc_percent'],  STATS_START, STATS_END).rename('VWC'),
        prep(soilt['substrate_temp_c'], STATS_START, STATS_END).rename('Substrate temp'),
        prep(spec['AS7343_visible'],    STATS_START, STATS_END).rename('Visible irr.'),
        prep(snd['rms_dbfs'],           STATS_START, STATS_END).rename('SPL (dB)'),
    ], axis=1)

    print(f"  Combined shape: {combined.shape}", flush=True)
    print(f"  Non-null counts:\n{combined.count().to_string()}", flush=True)

    corr1 = combined.corr(method='spearman', min_periods=50)
    corr1.to_csv(os.path.join(DATA_DIR, 'spearman_cross_modality.csv'))
    print(corr1.round(2).to_string(), flush=True)

    _plot_heatmap(
        corr1,
        title=f'Spearman Correlation - Cross-Modality\n'
              f'({STATS_START[:10]} to {STATS_END[:10]})',
        outfile='correlation_heatmap_crossmodality.png',
        figsize=(13, 10),
        fontsize=9,
    )

    print("  Saved heatmap.", flush=True)
    return corr1


def _plot_heatmap(corr, title, outfile, figsize=(10, 8), fontsize=9):
    try:
        import seaborn as sns

        fig, ax = plt.subplots(figsize=figsize)

        # Draw heatmap without annotations — we add them manually
        sns.heatmap(
            corr, annot=False, cmap='RdBu_r',
            center=0, vmin=-1, vmax=1,
            linewidths=0.5, ax=ax,
            square=False,
        )

        # Manually annotate every cell
        n = len(corr)
        for i in range(n):
            for j in range(n):
                val = corr.iloc[i, j]
                if pd.isna(val):
                    continue
                colour = 'white' if abs(val) > 0.55 else 'black'
                ax.text(
                    j + 0.5, i + 0.5,
                    f'{val:.2f}',
                    ha='center', va='center',
                    fontsize=fontsize,
                    color=colour,
                )

        ax.set_title(title, fontsize=11, pad=12)
        plt.tight_layout()
        plt.savefig(os.path.join(OUTPUT_DIR, outfile), dpi=200, bbox_inches='tight')
        plt.close()
        print(f"  Saved: {outfile}", flush=True)
    except ImportError:
        print("  seaborn not found - pip install seaborn", flush=True)


# ── Summary stats text ────────────────────────────────────────────────────────

def write_summary(stats_df, dom_period, corr):
    lines = [
        "=" * 60,
        "SECTION 5.5 ANALYSIS SUMMARY",
        f"Stats window:     {STATS_START}  →  {STATS_END}",
        f"FFT/coherence window: {FFT_START}  →  {FFT_END}",
        "=" * 60,
        "",
        "5.5.1  DESCRIPTIVE STATISTICS",
        stats_df.to_string(),
        "",
        "5.5.2  ACTUATOR PERIOD",
        f"  Dominant actuator cycle period (humidity FFT): "
        f"{dom_period:.1f} min" if dom_period else "  FFT: no dominant period found",
        "",
        "SPEARMAN CORRELATIONS (selected):",
    ]
    highlights = [
        ('Air temp',      'Substrate temp'),
        ('RH',            'eCO₂'),
        ('RH',            'TVOC'),
        ('RH',            'Visible irr.'),
        ('Visible (irr).',  '690 nm'),
        ('SPL (dB)',     'eCO₂'),
    ]
    for a, b in highlights:
        if a in corr.columns and b in corr.columns:
            lines.append(f"  {a:20s} vs {b:20s}  r={corr.loc[a,b]:.3f}")
    lines += [
        "",
        "OUTPUT FILES",
        f"  {OUTPUT_DIR}/descriptive_stats_table.png",
        f"  {OUTPUT_DIR}/actuator_period_fft.png",
        f"  {OUTPUT_DIR}/actuator_acoustic_signature.png",
        f"  {OUTPUT_DIR}/spectral_actuator_channels.png",
        f"  {OUTPUT_DIR}/correlation_heatmap.png",
        f"  {DATA_DIR}/descriptive_stats.csv",
        f"  {DATA_DIR}/spearman_correlation_matrix.csv",
        f"  {DATA_DIR}/actuator_acoustic_stats.txt",
        "=" * 60,
    ]
    summary = "\n".join(lines)
    with open(os.path.join(DATA_DIR, 'summary_stats.txt'), 'w') as f:
        f.write(summary)
    print("\n" + summary, flush=True)

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print(f"Stats window:         {STATS_START}  →  {STATS_END}")
    print(f"FFT/coherence window: {FFT_START}  →  {FFT_END}")

    bme, ens, spec, soil, soilt, snd, bins = load_all()

    print("\n--- Outlier removal ---", flush=True)
    bme   = remove_outliers(bme)
    ens   = remove_outliers(ens)
    spec  = remove_outliers(spec)
    soil  = remove_outliers(soil)
    soilt = remove_outliers(soilt)
    snd   = remove_outliers(snd)

    stats_df = compute_descriptive(bme, ens, spec, soil, soilt, snd, bins)
    dom_period = actuator_period_fft(bme)
    acoustic_actuator_signature(bme, bins)
    spectral_actuator_signature(spec)
    corr = correlation_heatmap(bme, ens, spec, soil, soilt, snd)
    write_summary(stats_df, dom_period, corr)

    print("\n=== Complete ===", flush=True)
    print(f"Figures → {OUTPUT_DIR}/")
    print(f"Data    → {DATA_DIR}/")

if __name__ == '__main__':
    main()