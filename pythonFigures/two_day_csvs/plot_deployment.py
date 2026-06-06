"""
Full Deployment Plot Script v3
================================
Plots every modality across the full timestamp range in the CSVs.
Put this script in the same folder as the CSVs and run:
    python3 plot_deployment_v3.py

Outputs:
    ./plots/bme280.png
    ./plots/ens160.png
    ./plots/soil.png
    ./plots/soiltemp.png
    ./plots/spectrum.png
    ./plots/spectrum_overlay.png
    ./plots/sound_spl.png
    ./plots/all_modalities.png
"""

import os
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

# ── Config ────────────────────────────────────────────────────────────────────
CSV_DIR  = '.'
OUT_DIR  = './plots'
os.makedirs(OUT_DIR, exist_ok=True)

plt.rcParams.update({
    'font.family':  'DejaVu Sans',
    'font.size':    10,
    'figure.dpi':   150,
    'savefig.dpi':  200,
    'savefig.bbox': 'tight',
    'axes.grid':    True,
    'grid.alpha':   0.3,
})

DATE_FMT = '%d %b\n%H:%M'

CHANNELS = {
    'AS7343_405nm': ('#7B00FF', '405 nm'),
    'AS7343_425nm': ('#6600FF', '425 nm'),
    'AS7343_450nm': ('#0000FF', '450 nm'),
    'AS7343_475nm': ('#0055FF', '475 nm'),
    'AS7343_515nm': ('#00AAFF', '515 nm'),
    'AS7343_550nm': ('#00CC44', '550 nm'),
    'AS7343_555nm': ('#44BB00', '555 nm'),
    'AS7343_600nm': ('#AACC00', '600 nm'),
    'AS7343_640nm': ('#FFAA00', '640 nm'),
    'AS7343_690nm': ('#FF5500', '690 nm'),
    'AS7343_745nm': ('#FF0000', '745 nm'),
    'AS7343_855nm': ('#880000', '855 nm'),
}

# ── Helpers ───────────────────────────────────────────────────────────────────
def load(filename, time_col):
    path = os.path.join(CSV_DIR, filename)
    print(f"Loading {filename}...", flush=True)
    df = pd.read_csv(path, parse_dates=[time_col])
    df = df.set_index(time_col).sort_index()
    print(f"  {len(df):,} rows  [{df.index[0]}  →  {df.index[-1]}]", flush=True)
    return df


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

    # Window size for median filter (must be odd) — 
    # larger catches multi-sample spikes
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
        # IQR fence
        if col not in skip_iqr:
            q1, q3 = df[col].quantile(0.25), df[col].quantile(0.75)
            iqr = q3 - q1
            if iqr > 0:
                lo, hi = q1 - k * iqr, q3 + k * iqr
                n = ((df[col] < lo) | (df[col] > hi)).sum()
                if n:
                    print(f"  IQR [{col}]: {n} fenced", flush=True)
                df.loc[(df[col] < lo) | (df[col] > hi), col] = float('nan')

        # Median filter spike removal — compares each point to local median
        # Catches multi-sample spikes that bilateral diff misses
        win = median_window.get(col, 0)
        if win > 0:
            rolling_median = df[col].rolling(win, center=True, min_periods=3).median()
            deviation = (df[col] - rolling_median).abs()
            # Threshold: point must deviate from local median by more than
            # the typical local spread
            local_mad = deviation.rolling(win * 10, center=True, min_periods=10).median()
            spike_mask = deviation > (local_mad * 10).clip(lower=0.5)
            n = spike_mask.sum()
            if n:
                print(f"  Median filter [{col}]: {n} removed", flush=True)
            df.loc[spike_mask, col] = float('nan')
        else:
            # Fallback bilateral spike filter for non-windowed columns
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

def fmt_ax(ax):
    ax.xaxis.set_major_formatter(mdates.DateFormatter(DATE_FMT))
    ax.xaxis.set_major_locator(mdates.AutoDateLocator())
    plt.setp(ax.get_xticklabels(), rotation=0, ha='center', fontsize=8)


def save(fig, name):
    out = os.path.join(OUT_DIR, name)
    fig.savefig(out)
    plt.close(fig)
    print(f"  Saved: {out}", flush=True)


def plot_series(ax, series, color, linewidth=0.6, label=None, alpha=1.0):
    """Plot series with real NaN gaps preserved plus a thin dashed bridge across gaps."""
    ax.plot(series.index, series.values,
            color=color, linewidth=linewidth, label=label, alpha=alpha)
    filled = series.interpolate(method='time', limit=300)
    ax.plot(filled.index, filled.values,
            color=color, linewidth=0.3, alpha=0.35, linestyle='--')


# ── BME280 ────────────────────────────────────────────────────────────────────
def plot_bme280(df):
    print("Plotting BME280...", flush=True)
    fig, axes = plt.subplots(3, 1, figsize=(14, 8), sharex=True)
    fig.suptitle('BME280 — Air Temperature, Humidity, Pressure', fontsize=12, fontweight='bold')
    fig.subplots_adjust(top=0.96)
    plot_series(axes[0], df['temperature_c'], 'tomato')
    axes[0].set_ylabel('Temperature (°C)')
    t = df['temperature_c'].dropna()
    tmin = t.quantile(0.001)
    tmax = t.quantile(0.999)
    pad  = (tmax - tmin) * 0.15
    axes[0].set_ylim(tmin - pad, tmax + pad)

    plot_series(axes[1], df['humidity_percent'], 'steelblue')
    axes[1].set_ylabel('Humidity (%)')
    axes[1].margins(y=0.08)

    plot_series(axes[2], df['pressure_hpa'] * 10, 'seagreen')
    axes[2].set_ylabel('Pressure (hPa)')
    axes[2].margins(y=0.08)

    for ax in axes:
        fmt_ax(ax)
    axes[-1].set_xlabel('Time (UTC)')
    plt.tight_layout()
    save(fig, 'bme280.png')


# ── ENS160 ───────────────────────────────────────────────────────────────────
def plot_ens160(df):
    print("Plotting ENS160...", flush=True)
    fig, axes = plt.subplots(3, 1, figsize=(14, 8), sharex=True)
    fig.suptitle('ENS160 — eCO₂, TVOC, AQI', fontsize=12, fontweight='bold')

    plot_series(axes[0], df['eco2_ppm'], 'darkorange')
    axes[0].set_ylabel('eCO₂ (ppm)')
    axes[0].margins(y=0.08)

    plot_series(axes[1], df['tvoc_ppb'], 'purple')
    axes[1].set_ylabel('TVOC (ppb)')
    axes[1].margins(y=0.08)

    axes[2].plot(df.index, df['aqi'], color='crimson', linewidth=0.6, drawstyle='steps-post')
    axes[2].set_ylabel('AQI')
    axes[2].set_yticks([1, 2, 3, 4, 5])
    axes[2].set_yticklabels(['1 Excellent', '2 Good', '3 Moderate', '4 Poor', '5 Unhealthy'], fontsize=7)
    axes[2].margins(y=0.08)

    for ax in axes:
        fmt_ax(ax)
    axes[-1].set_xlabel('Time (UTC)')
    plt.tight_layout()
    save(fig, 'ens160.png')


# ── Soil moisture ─────────────────────────────────────────────────────────────
def plot_soil(df):
    print("Plotting soil moisture...", flush=True)
    fig, ax = plt.subplots(figsize=(14, 4))
    fig.suptitle('Capacitive Soil Moisture — VWC %', fontsize=12, fontweight='bold')
    plot_series(ax, df['soil_vwc_percent'], 'saddlebrown')
    ax.set_ylabel('VWC (%)')
    ax.set_xlabel('Time (UTC)')
    ax.margins(y=0.08)
    fmt_ax(ax)
    plt.tight_layout()
    save(fig, 'soil.png')


# ── Substrate temperature ─────────────────────────────────────────────────────
def plot_soiltemp(df):
    print("Plotting substrate temperature...", flush=True)
    fig, ax = plt.subplots(figsize=(14, 4))
    fig.suptitle('DS18B20 — Substrate Temperature', fontsize=12, fontweight='bold')
    plot_series(ax, df['substrate_temp_c'], 'goldenrod')
    ax.set_ylabel('Temperature (°C)')
    ax.set_xlabel('Time (UTC)')
    ax.margins(y=0.08)
    fmt_ax(ax)
    plt.tight_layout()
    save(fig, 'soiltemp.png')


# ── AS7343 spectrum ───────────────────────────────────────────────────────────
def plot_spectrum(df):
    print("Plotting AS7343 spectrum...", flush=True)

    # ── 4x3 grid — one subplot per channel ──
    fig, axes = plt.subplots(4, 3, figsize=(16, 12), sharex=True)
    fig.suptitle('AS7343 — Per-Channel Spectral Irradiance (mW m⁻²)', fontsize=12, fontweight='bold')
    axes_flat = axes.flatten()

    for idx, (col, (colour, label)) in enumerate(CHANNELS.items()):
        ax = axes_flat[idx]
        if col in df.columns:
            s = df[col].replace(0, float('nan'))
            plot_series(ax, s, colour)
        safe_colour = colour if colour not in ('#00FF00', '#00CC44', '#44BB00', '#AACC00') else '#227700'
        ax.set_title(label, fontsize=9, fontweight='bold', color=safe_colour)
        ax.set_ylabel('mW m⁻²', fontsize=7)
        ax.margins(y=0.08)
        fmt_ax(ax)

    for ax in axes[-1]:
        ax.set_xlabel('Time (UTC)', fontsize=8)
    plt.tight_layout()
    save(fig, 'spectrum.png')

    # ── All channels + visible overlaid on one plot ──
    fig2, ax2 = plt.subplots(figsize=(14, 6))
    fig2.suptitle('AS7343 — All Channels Overlaid (mW m⁻²)', fontsize=12, fontweight='bold')

    for col, (colour, label) in CHANNELS.items():
        if col in df.columns:
            s = df[col].replace(0, float('nan'))
            filled = s.interpolate(method='time', limit=300)
            safe_colour = colour if colour not in ('#00FF00',) else '#009900'
            ax2.plot(filled.index, filled.values,
                     color=safe_colour, linewidth=0.7, alpha=0.85, label=label)

    if 'AS7343_visible' in df.columns:
        vis = df['AS7343_visible'].replace(0, float('nan'))
        filled_vis = vis.interpolate(method='time', limit=300)
        ax2.plot(filled_vis.index, filled_vis.values,
                 color='dimgray', linewidth=1.4, alpha=0.95,
                 label='Visible (broadband)', zorder=5)

    ax2.set_ylabel('mW m⁻²')
    ax2.set_xlabel('Time (UTC)')
    ax2.margins(y=0.08)
    ax2.legend(fontsize=7, ncol=4, loc='upper left', framealpha=0.7)
    fmt_ax(ax2)
    plt.tight_layout()
    save(fig2, 'spectrum_overlay.png')


# ── Sound SPL ─────────────────────────────────────────────────────────────────
def plot_sound(df):
    print("Plotting sound SPL...", flush=True)
    fig, ax = plt.subplots(figsize=(14, 4))
    fig.suptitle('SPH0645 — Broadband RMS SPL (dBFS)', fontsize=12, fontweight='bold')
    plot_series(ax, df['rms_dbfs'], '#00C8FF', linewidth=0.4, alpha=0.8)
    ax.set_ylabel('RMS dBFS')
    ax.set_xlabel('Time (UTC)')
    ax.margins(y=0.08)
    fmt_ax(ax)
    plt.tight_layout()
    save(fig, 'sound_spl.png')


# ── Combined overview ─────────────────────────────────────────────────────────
def plot_combined(bme, ens, soil, soilt, spec, snd):
    print("Plotting combined overview...", flush=True)
    fig, axes = plt.subplots(7, 1, figsize=(16, 22), sharex=True)
    fig.suptitle('Full Deployment Overview — All Modalities', fontsize=13, fontweight='bold')

    # Temperature — air + substrate
    plot_series(axes[0], bme['temperature_c'],       'tomato',    label='Air temp')
    plot_series(axes[0], soilt['substrate_temp_c'],  'goldenrod', label='Substrate temp')
    axes[0].set_ylabel('Temp (°C)')
    axes[0].legend(fontsize=8, loc='upper right')
    all_temps = pd.concat([bme['temperature_c'].dropna(), soilt['substrate_temp_c'].dropna()])
    tmin = all_temps.quantile(0.001)
    tmax = all_temps.quantile(0.999)
    pad  = (tmax - tmin) * 0.15
    axes[0].set_ylim(tmin - pad, tmax + pad)     

    # Humidity
    plot_series(axes[1], bme['humidity_percent'], 'steelblue')
    axes[1].set_ylabel('RH (%)')
    axes[1].margins(y=0.08)

    # Pressure
    plot_series(axes[2], bme['pressure_hpa'] * 10, 'seagreen')
    axes[2].set_ylabel('Pressure (hPa)')
    axes[2].margins(y=0.08)

    # eCO2 + TVOC
    plot_series(axes[3], ens['eco2_ppm'], 'darkorange', label='eCO₂')
    plot_series(axes[3], ens['tvoc_ppb'], 'purple',     label='TVOC')
    axes[3].set_ylabel('ppm / ppb')
    axes[3].margins(y=0.08)
    axes[3].legend(fontsize=8, loc='upper right')

    # AQI — on a twin axis so it doesn't compress eCO2/TVOC scale
    ax3b = axes[3].twinx()
    ax3b.plot(ens.index, ens['aqi'], color='crimson', linewidth=0.5,
              alpha=0.6, drawstyle='steps-post', label='AQI')
    ax3b.set_ylabel('AQI', color='crimson', fontsize=8)
    ax3b.set_yticks([1, 2, 3, 4, 5])
    ax3b.set_yticklabels(['1', '2', '3', '4', '5'], fontsize=7, color='crimson')
    ax3b.set_ylim(0, 8)
    ax3b.legend(fontsize=8, loc='upper left')

    # Soil VWC
    plot_series(axes[4], soil['soil_vwc_percent'], 'saddlebrown')
    axes[4].set_ylabel('VWC (%)')
    axes[4].margins(y=0.08)

    # All spectral channels + visible broadband
    for col, (colour, label) in CHANNELS.items():
        if col in spec.columns:
            s = spec[col].replace(0, float('nan'))
            filled = s.interpolate(method='time', limit=300)
            safe_colour = colour if colour not in ('#00FF00',) else '#009900'
            axes[5].plot(filled.index, filled.values,
                         color=safe_colour, linewidth=0.5, alpha=0.7, label=label)
    if 'AS7343_visible' in spec.columns:
        vis = spec['AS7343_visible'].replace(0, float('nan'))
        filled_vis = vis.interpolate(method='time', limit=300)
        axes[5].plot(filled_vis.index, filled_vis.values,
                     color='dimgray', linewidth=1.0, alpha=0.95,
                     label='Visible', zorder=5)
    axes[5].set_ylabel('Irradiance\n(mW m⁻²)')
    axes[5].margins(y=0.08)
    axes[5].legend(fontsize=6, ncol=4, loc='upper left', framealpha=0.6)

    # Sound SPL
    plot_series(axes[6], snd['rms_dbfs'], '#00C8FF', linewidth=0.4, alpha=0.8)
    axes[6].set_ylabel('SPL (dBFS)')
    axes[6].margins(y=0.08)

    for ax in axes:
        fmt_ax(ax)
    axes[-1].set_xlabel('Time (UTC)')
    plt.tight_layout()
    save(fig, 'all_modalities.png')


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    # bme   = remove_outliers(load('bme280.csv',   'timestamp'))
    bme   = remove_outliers(load('bme280.csv',   'timestamp'))
    ens   = remove_outliers(load('ens160.csv',   'timestamp'))
    soil  = remove_outliers(load('soil.csv',     'timestamp'))
    soilt = remove_outliers(load('soiltemp.csv', 'timestamp'))
    spec  = remove_outliers(load('spectrum.csv', 'timestamp'))
    snd   = remove_outliers(load('sound.csv',    'received_time'))

    plot_bme280(bme)
    plot_ens160(ens)
    plot_soil(soil)
    plot_soiltemp(soilt)
    plot_spectrum(spec)
    plot_sound(snd)
    plot_combined(bme, ens, soil, soilt, spec, snd)

    print("\n=== Done ===")
    print(f"All plots saved to {OUT_DIR}/")


if __name__ == '__main__':
    main()