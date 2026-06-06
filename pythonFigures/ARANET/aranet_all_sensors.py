"""
aranet_all_sensors.py
=====================
Plots all four Aranet sensors on the same axes for each modality
(Temperature, Humidity, Pressure, CO2) over a chosen time window.

USAGE
-----
    python aranet_all_sensors.py

CONFIG
------
Edit ARANET_CSV and the window below.

INSTALL
-------
    pip install pandas matplotlib
"""

import os, sys, re
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.gridspec import GridSpec

# ═══════════════════════════════ CONFIG ══════════════════════════════════════

ARANET_CSV   = "Sensor data.csv"

WINDOW_START = "2026-05-23 11:11:00"   # UTC
WINDOW_END   = "2026-05-23 14:15:30"   # UTC

OUT_FIGURE   = "aranet_all_sensors.png"

# Sensor display labels — edit if you know which unit is where physically
SENSOR_LABELS = {
    "09C00": "09C00",
    "09C05": "09C05 (grow tent)",
    "09C09": "09C09",
    "09C0F": "09C0F",
}

# Colours per sensor — consistent across all four panels
SENSOR_COLORS = {
    "09C00": "#e74c3c",
    "09C05": "#2ecc71",
    "09C09": "#3498db",
    "09C0F": "#f39c12",
}

# ═════════════════════════════════════════════════════════════════════════════

BG_DARK  = "#1a1a2e"
BG_PANEL = "#16213e"
GRID_COL = "#2a2a4a"
SPINE_COL= "#404060"

MODALITIES = [
    dict(key="temperature", label="Temperature",   unit="°C"),
    dict(key="humidity",    label="Rel. Humidity", unit="%"),
    dict(key="pressure",    label="Pressure",      unit="hPa"),
    dict(key="co2",         label="CO₂",           unit="ppm"),
]


def load_aranet(path: str) -> pd.DataFrame:
    df = pd.read_csv(path, encoding="utf-8-sig")
    time_col = df.columns[0]
    df[time_col] = pd.to_datetime(df[time_col], utc=True)
    df = df.rename(columns={time_col: "time"})
    return df.sort_values("time").reset_index(drop=True)


def list_sensors(df: pd.DataFrame) -> list:
    ids = set()
    for col in df.columns:
        m = re.match(r"^(\S+)\s+Aranet4\s+", col)
        if m:
            ids.add(m.group(1))
    return sorted(ids)


def get_cols(df: pd.DataFrame, sensor_id: str) -> dict:
    mapping = {}
    for col in df.columns:
        if not col.startswith(sensor_id):
            continue
        cl = col.lower()
        if   "temperature" in cl:
            mapping["temperature"] = col
        elif "humidity" in cl:
            mapping["humidity"] = col
        elif "pressure" in cl:
            mapping["pressure"] = col
        elif "co2" in cl or "co\u2082" in cl or "\u2082" in col:
            mapping["co2"] = col
    return mapping


def style_ax(ax):
    ax.set_facecolor(BG_PANEL)
    for sp in ax.spines.values():
        sp.set_edgecolor(SPINE_COL)
    ax.tick_params(colors="white", labelsize=8)
    ax.xaxis.label.set_color("white")
    ax.yaxis.label.set_color("white")
    ax.title.set_color("white")
    ax.grid(True, color=GRID_COL, linewidth=0.5)


def main():
    if not os.path.exists(ARANET_CSV):
        sys.exit(f"Cannot find '{ARANET_CSV}' — place it next to this script.")

    print(f"Loading {ARANET_CSV} …")
    df = load_aranet(ARANET_CSV)
    sensors = list_sensors(df)
    print(f"  Sensors found: {sensors}")

    w0 = pd.Timestamp(WINDOW_START, tz="UTC")
    w1 = pd.Timestamp(WINDOW_END,   tz="UTC")
    win = df[(df["time"] >= w0) & (df["time"] < w1)].copy()
    print(f"  Rows in window [{WINDOW_START} – {WINDOW_END}]: {len(win)}")

    if len(win) == 0:
        print("  No rows in window — check WINDOW_START / WINDOW_END.")
        nearby = df[df["time"].between(
            w0 - pd.Timedelta("10min"),
            w1 + pd.Timedelta("10min")
        )]["time"]
        print("  Nearby timestamps:")
        print(nearby.head(10).to_string())
        return

    # Build per-sensor column maps
    sensor_cols = {s: get_cols(df, s) for s in sensors}

    fig = plt.figure(figsize=(16, 14), facecolor=BG_DARK)
    fig.suptitle(
        f"All Aranet Sensors  —  {WINDOW_START} to {WINDOW_END} UTC",
        color="white", fontsize=13, fontweight="bold", y=0.98,
    )

    gs = GridSpec(2, 2, figure=fig,
                  hspace=0.42, wspace=0.28,
                  left=0.07, right=0.97,
                  top=0.93, bottom=0.06)

    win_idx = win.set_index("time")

    panel_positions = [(0,0), (0,1), (1,0), (1,1)]

    for (r, c), m in zip(panel_positions, MODALITIES):
        ax = fig.add_subplot(gs[r, c])
        style_ax(ax)

        plotted = False
        for sensor_id in sensors:
            col_map = sensor_cols.get(sensor_id, {})
            col = col_map.get(m["key"])
            if col is None or col not in win_idx.columns:
                continue

            ser = win_idx[col].dropna()
            if ser.empty:
                continue

            color = SENSOR_COLORS.get(sensor_id, "#ffffff")
            label = SENSOR_LABELS.get(sensor_id, sensor_id)

            ax.plot(ser.index, ser.values,
                    color=color, linewidth=1.8,
                    marker="o", markersize=3.5,
                    label=label, zorder=3)
            plotted = True

        ax.set_title(f"{m['label']}  ({m['unit']})", fontsize=11, pad=5)
        ax.set_ylabel(m["unit"], fontsize=9)
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
        ax.xaxis.set_major_locator(mdates.AutoDateLocator())
        plt.setp(ax.xaxis.get_majorticklabels(),
                 rotation=25, ha="right", fontsize=7.5)

        if plotted:
            ax.legend(fontsize=8, facecolor=BG_DARK,
                      labelcolor="white", framealpha=0.8,
                      loc="best", ncol=1)
        else:
            ax.text(0.5, 0.5, "No data in window",
                    ha="center", va="center", color="#888888",
                    transform=ax.transAxes, fontsize=10)

    plt.savefig(OUT_FIGURE, dpi=160, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close("all")
    print(f"\n  Saved  →  {OUT_FIGURE}")


if __name__ == "__main__":
    main()
