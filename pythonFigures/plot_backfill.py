"""
plot_backfill.py
================
Plots BME280 SD acquisition timestamp vs bridge.log wall-clock receive time
to visualise the UQ network outage and SD card backfill event.

Input:  bme_times.csv   (two ISO-format columns: wall_receive_time, sd_timestamp)
Output: backfill_plot.png

Usage:
    python plot_backfill.py
    python plot_backfill.py --csv /path/to/bme_times.csv --out my_figure.png
"""

import argparse
import datetime
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.dates  as mdates

# ── CONFIG ───────────────────────────────────────────────────────────────────

# Known outage boundaries (UTC) — adjust if query results differ
OUTAGE_START_UTC  = datetime.datetime(2026, 5, 28, 13,  0, 19)
RECONNECT_UTC     = datetime.datetime(2026, 5, 29,  0, 57, 34)

# Offset for AEST display (+10 h)
AEST = datetime.timezone(datetime.timedelta(hours=10))

# Delay threshold separating live from backfill records (seconds).
# Live:     wall_receive - sd_timestamp ≤ LIVE_THRESHOLD
# Backfill: wall_receive - sd_timestamp  > LIVE_THRESHOLD
LIVE_THRESHOLD_S = 30

# Downsample for plotting — keeps the scatter readable without 92 K dots
# Set to 1 to plot every record (slow), higher to thin the data
DOWNSAMPLE = 10

# Output figure DPI (160 for thesis quality)
DPI = 160

# ── COLOURS (match existing thesis plot style) ────────────────────────────────
C_LIVE     = "#2E5C8A"   # blue  — normal live operation
C_BACKFILL = "#E05A2B"   # orange — backfill burst
C_OUTAGE   = "#FF8C00"   # amber  — outage shading
C_GRID     = "#DDDDDD"

# ─────────────────────────────────────────────────────────────────────────────

def load(csv_path: str) -> pd.DataFrame:
    df = pd.read_csv(
        csv_path,
        header=None,
        names=["wall", "sd"],
    )
    df = df.dropna()
    df["wall"] = pd.to_datetime(df["wall"], format="mixed")
    df["sd"]   = pd.to_datetime(df["sd"],   format="mixed")
    df["delay_s"] = (df["wall"] - df["sd"]).dt.total_seconds()

    # Classify each record
    df["kind"] = np.where(df["delay_s"] <= LIVE_THRESHOLD_S, "live", "backfill")

    # Convert to AEST for display
    df["wall_aest"] = df["wall"].dt.tz_localize("UTC").dt.tz_convert(AEST)
    df["sd_aest"]   = df["sd"].dt.tz_localize("UTC").dt.tz_convert(AEST)

    return df


def plot(df: pd.DataFrame, out_path: str):

    # ── Downsample for scatter legibility ─────────────────────────────────────
    live     = df[df["kind"] == "live"    ].iloc[::DOWNSAMPLE]
    backfill = df[df["kind"] == "backfill"].iloc[::DOWNSAMPLE]

    # ── Summary stats printed to console ──────────────────────────────────────
    bf_all = df[df["kind"] == "backfill"]
    if len(bf_all):
        sd_min  = bf_all["sd"].min()
        sd_max  = bf_all["sd"].max()
        w_min   = bf_all["wall"].min()
        w_max   = bf_all["wall"].max()
        sd_span = (sd_max  - sd_min).total_seconds()  / 3600
        w_span  = (w_max   - w_min).total_seconds()   / 60
        ratio   = sd_span * 60 / max(w_span, 0.01)
        print(f"\n── Backfill summary ─────────────────────────────────")
        print(f"  Records classified as backfill : {len(bf_all):,}")
        print(f"  SD timestamp range             : {sd_min} → {sd_max} UTC")
        print(f"  SD coverage                    : {sd_span:.2f} hours")
        print(f"  Bridge receive window          : {w_min} → {w_max} UTC")
        print(f"  Receive window duration        : {w_span:.1f} minutes")
        print(f"  Compression ratio              : {ratio:.1f}× "
              f"(hours of data per minute of upload)")
        print(f"─────────────────────────────────────────────────────\n")

    outage_s_aest   = OUTAGE_START_UTC.replace(
        tzinfo=datetime.timezone.utc).astimezone(AEST)
    reconnect_aest  = RECONNECT_UTC.replace(
        tzinfo=datetime.timezone.utc).astimezone(AEST)

    # ── Figure ────────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(11, 5), facecolor="#FAFAFA")
    ax.set_facecolor("#F9F9F9")

    # Outage shading — vertical band between outage start and reconnect
    ax.axvspan(
        mdates.date2num(outage_s_aest),
        mdates.date2num(reconnect_aest),
        color=C_OUTAGE, alpha=0.12, zorder=0,
        label=f"Network outage  "
              f"({OUTAGE_START_UTC.strftime('%H:%M')}–"
              f"{RECONNECT_UTC.strftime('%H:%M')} UTC, "
              f"11 h 57 min)"
    )

    # Diagonal reference line (perfect 1:1 — receive time = SD time)
    x_ref = [df["sd_aest"].min(), df["sd_aest"].max()]
    ax.plot(
        x_ref, x_ref,
        color="#AAAAAA", linewidth=0.8, linestyle="--",
        label="1:1 reference", zorder=1
    )

    # Live records
    ax.scatter(
        live["sd_aest"], live["wall_aest"],
        s=1.5, color=C_LIVE, alpha=0.5, linewidths=0,
        label="Live telemetry (≈ SD timestamp)", zorder=2
    )

    # Backfill records
    ax.scatter(
        backfill["sd_aest"], backfill["wall_aest"],
        s=3, color=C_BACKFILL, alpha=0.7, linewidths=0,
        label=f"SD backfill burst  "
              f"(reconnect {RECONNECT_UTC.strftime('%H:%M')} UTC, "
              f"{len(bf_all):,} records)", zorder=3
    )

    # ── Annotations ───────────────────────────────────────────────────────────
    ax.annotate(
        "Outage start\n23:00 AEST",
        xy=(mdates.date2num(outage_s_aest), mdates.date2num(outage_s_aest)),
        xytext=(mdates.date2num(outage_s_aest) - 0.08,
                mdates.date2num(reconnect_aest) + 0.05),
        fontsize=8, color=C_OUTAGE,
        arrowprops=dict(arrowstyle="->", color=C_OUTAGE, lw=0.8),
        ha="right"
    )
    ax.annotate(
        "Backfill burst\n10:57 AEST",
        xy=(mdates.date2num(outage_s_aest),
            mdates.date2num(reconnect_aest)),
        xytext=(mdates.date2num(outage_s_aest) - 0.08,
                mdates.date2num(reconnect_aest) + 0.15),
        fontsize=8, color=C_BACKFILL,
        arrowprops=dict(arrowstyle="->", color=C_BACKFILL, lw=0.8),
        ha="right"
    )

    # ── Axes formatting ───────────────────────────────────────────────────────
    date_fmt = mdates.DateFormatter("%d %b\n%H:%M", tz=AEST)
    ax.xaxis.set_major_formatter(date_fmt)
    ax.yaxis.set_major_formatter(date_fmt)
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=3, tz=AEST))
    ax.yaxis.set_major_locator(mdates.HourLocator(interval=3, tz=AEST))

    ax.set_xlabel("SD acquisition timestamp (AEST)", fontsize=11)
    ax.set_ylabel("Bridge receive time (AEST)",       fontsize=11)
    ax.set_title(
        "BME280 SD Acquisition Timestamp vs Cloud Receive Time\n"
        "UQ Network Outage and SD Card Backfill: 28–29 May 2026",
        fontsize=11
    )

    ax.grid(True, color=C_GRID, linewidth=0.5, zorder=0)
    ax.spines[["top", "right"]].set_visible(False)

    leg = ax.legend(
        fontsize=8.5, framealpha=0.9,
        loc="upper left", markerscale=4
    )

    plt.tight_layout()
    plt.savefig(out_path, dpi=DPI, bbox_inches="tight")
    plt.close()
    print(f"Figure saved → {out_path}")


# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", default="bme_times.csv",
                        help="Path to bme_times.csv (default: ./bme_times.csv)")
    parser.add_argument("--out", default="backfill_plot.png",
                        help="Output figure path (default: backfill_plot.png)")
    args = parser.parse_args()

    print(f"Loading {args.csv} …")
    df = load(args.csv)
    print(f"  {len(df):,} records loaded  "
          f"({(df['kind']=='live').sum():,} live, "
          f"{(df['kind']=='backfill').sum():,} backfill)")

    plot(df, args.out)
