"""
aranet_compare_final.py
=======================
Compares Aranet reference sensor against the IoT system BME280 and ENS160
for Temperature, Relative Humidity, Barometric Pressure, and eCO2.

USAGE
-----
1.  Set the CONFIG block below.
2.  Place all three CSV files in the same folder as this script.
3.  Run:  python aranet_compare_final.py

CSV FILES REQUIRED
------------------
ARANET_CSV      :  "Sensor data.csv"       (exported from Aranet app)
BME_CSV         :  bme.csv                 (exported from Azure SQL)
ENS_CSV         :  ens.csv                 (exported from Azure SQL)
SUBSTRATE_CSV   :  substrateTemp.csv       (exported from Azure SQL)

SQL QUERIES
-----------
-- BME280
SELECT timestamp AS time,
       temperature_c    AS bme_temperature_c,
       humidity_percent AS bme_humidity_pct,
       pressure_hpa     AS bme_pressure_hpa
FROM telemetry_bme280
WHERE device_id  = 'dev-1'
  AND timestamp >= '2026-05-25 12:00:00'
  AND timestamp  < '2026-05-25 16:00:00'
  AND temperature_c IS NOT NULL
ORDER BY timestamp ASC

-- ENS160
SELECT timestamp AS time,
       eco2_ppm  AS ens_eco2_ppm
FROM telemetry_ens160
WHERE device_id  = 'dev-1'
  AND timestamp >= '2026-05-25 12:00:00'
  AND timestamp  < '2026-05-25 16:00:00'
  AND eco2_ppm IS NOT NULL
ORDER BY timestamp ASC

-- DS18B20 substrate temperature
SELECT timestamp AS time,
       temperature_c AS ds18b20_temp_c
FROM telemetry_soil_temperature
WHERE device_id  = 'dev-2'
  AND timestamp >= '2026-05-25 12:00:00'
  AND timestamp  < '2026-05-25 16:00:00'
  AND temperature_c IS NOT NULL
ORDER BY timestamp ASC

INSTALL
-------
    pip install pandas matplotlib scipy numpy
"""

import os, sys, re
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.gridspec import GridSpec
from scipy import stats

# ═══════════════════════════════ CONFIG ══════════════════════════════════════

ARANET_CSV    = "Sensor data.csv"
ARANET_SENSOR = "09C05"   # None to list all sensor IDs first

# BME_CSV       = "bmeFinal.csv"
# ENS_CSV       = "ensFinal.csv"
# SUBSTRATE_CSV = "TempFinal.csv"

# WINDOW_START  = "2026-05-23 01:00:00"   # UTC
# WINDOW_END    = "2026-05-23 05:00:00"   # UTC

BME_CSV       = "bme.csv"
ENS_CSV       = "ens.csv"
SUBSTRATE_CSV = "substrateTemp.csv"

WINDOW_START  = "2026-05-25 12:00:00"   # UTC
WINDOW_END    = "2026-05-25 12:05:00"   # UTC


ARANET_TZ_OFFSET_HOURS = 10  # Aranet CSV timestamps are in AEST (UTC+10) — subtract 10 h to align with IoT UTC

RESAMPLE_FREQ = "30s"

OUT_FIGURE    = "aranet_vs_iot.png"
OUT_STATS     = "aranet_vs_iot_stats.csv"

# ═════════════════════════════════════════════════════════════════════════════

BG_DARK   = "#f8f9fa"
BG_PANEL  = "#ffffff"
GRID_COL  = "#dee2e6"
SPINE_COL = "#adb5bd"

MODALITIES = [
    dict(key="temperature", label="Temperature",           unit="°C",
         ar_key="temperature", iot_col="bme_temperature_c",
         iot_col2="ds18b20_temp_c", iot_col2_label="In-substrate (DS18B20)",
         c_ar="#c0392b", c_iot="#e67e22", c_iot2="#6c5ce7"),
    dict(key="humidity",    label="Relative Humidity",     unit="%",
         ar_key="humidity",    iot_col="bme_humidity_pct",
         c_ar="#2980b9", c_iot="#27ae60"),
    dict(key="pressure",    label="Pressure",              unit="hPa",
         ar_key="pressure",    iot_col="bme_pressure_hpa",
         c_ar="#8e44ad", c_iot="#16a085"),
    dict(key="co2",         label="CO\u2082 / eCO\u2082",  unit="ppm",
         ar_key="co2",         iot_col="ens_eco2_ppm",
         c_ar="#c1d300", c_iot="#c0392b"),
]


# ─────────────────────────── ARANET HELPERS ──────────────────────────────────

def load_aranet(path):
    df = pd.read_csv(path, encoding="utf-8-sig")
    time_col = df.columns[0]
    df[time_col] = pd.to_datetime(df[time_col], utc=True)
    df = df.rename(columns={time_col: "time"})
    return df.sort_values("time").reset_index(drop=True)


def list_aranet_sensors(df):
    ids = set()
    for col in df.columns:
        m = re.match(r"^(\S+)\s+Aranet4\s+", col)
        if m:
            ids.add(m.group(1))
    return sorted(ids)


def get_aranet_cols(df, sensor_id):
    mapping = {}
    for col in df.columns:
        if not col.startswith(sensor_id):
            continue
        cl = col.lower()
        if   "temperature" in cl: mapping["temperature"] = col
        elif "humidity"    in cl: mapping["humidity"]    = col
        elif "pressure"    in cl: mapping["pressure"]    = col
        elif ("co2" in cl or "carbon" in cl
              or "co\u2082" in cl or "\u2082" in col.lower()):
            mapping["co2"] = col
    return mapping


# ─────────────────────────── IOT HELPERS ─────────────────────────────────────

def load_iot_csvs(bme_path, ens_path, substrate_path):
    bme = pd.read_csv(bme_path, encoding="utf-8-sig")
    ens = pd.read_csv(ens_path, encoding="utf-8-sig")

    bme["time"] = pd.to_datetime(bme["time"], utc=True)
    ens["time"] = pd.to_datetime(ens["time"], utc=True)
    bme = bme.sort_values("time").reset_index(drop=True)
    ens = ens.sort_values("time").reset_index(drop=True)

    # Pressure: stored as kPa in DB, Aranet reports hPa — multiply by 10
    bme["bme_pressure_hpa"] = bme["bme_pressure_hpa"] * 10

    merged = pd.merge_asof(bme, ens, on="time",
                           tolerance=pd.Timedelta("2s"), direction="nearest")
    print(f"  BME rows: {len(bme):,}   ENS rows: {len(ens):,}   "
          f"Merged: {len(merged):,}   ENS NULLs: {merged['ens_eco2_ppm'].isna().sum()}")
    print(f"  Pressure after x10: "
          f"{merged['bme_pressure_hpa'].min():.1f} – {merged['bme_pressure_hpa'].max():.1f} hPa")

    if substrate_path and os.path.exists(substrate_path):
        sub = pd.read_csv(substrate_path, encoding="utf-8-sig")
        sub["time"] = pd.to_datetime(sub["time"], utc=True)
        sub = sub.sort_values("time").reset_index(drop=True)
        merged = pd.merge_asof(merged, sub, on="time",
                               tolerance=pd.Timedelta("10s"), direction="nearest")
        nulls = merged["ds18b20_temp_c"].isna().sum()
        print(f"  DS18B20 rows: {len(sub):,}   NULLs after merge: {nulls}")
        print(f"  DS18B20 range: {merged['ds18b20_temp_c'].min():.2f} – "
              f"{merged['ds18b20_temp_c'].max():.2f} °C")
    else:
        merged["ds18b20_temp_c"] = float("nan")
        print("  Substrate CSV not found — ds18b20_temp_c set to NaN")

    return merged


# ─────────────────────────── STATISTICS ──────────────────────────────────────

def resample_align(ref, sys_, freq):
    ref_r = ref.resample(freq).mean()
    sys_r = sys_.resample(freq).mean()
    both  = pd.concat([ref_r, sys_r], axis=1).dropna()
    both.columns = ["ref", "sys"]
    return both["ref"], both["sys"]


def compute_stats(ref, sys_):
    diff = sys_ - ref
    if ref.std() < 1e-9 or sys_.std() < 1e-9:
        r, p = float("nan"), float("nan")
    else:
        r, p = stats.pearsonr(ref, sys_)
    return dict(n=int(len(ref)), bias=float(diff.mean()),
                mae=float(diff.abs().mean()),
                rmse=float(np.sqrt((diff**2).mean())),
                r=float(r), p=float(p))


def fmt_r(r):
    return f"{r:.4f}" if not np.isnan(r) else "N/A (constant)"

def fmt_p(p):
    return f"{p:.6f}" if not np.isnan(p) else "N/A"


# ─────────────────────────── PLOT ────────────────────────────────────────────

def style_ax(ax):
    ax.set_facecolor(BG_PANEL)
    for sp in ax.spines.values():
        sp.set_edgecolor(SPINE_COL)
    ax.tick_params(colors="black", labelsize=8)
    ax.xaxis.label.set_color("black")
    ax.yaxis.label.set_color("black")
    ax.title.set_color("black")
    ax.grid(True, color=GRID_COL, linewidth=0.5)


def make_figure(aranet_win, aranet_cols, iot_df, sensor_id):
    active = [m for m in MODALITIES
              if m["ar_key"] in aranet_cols and m["iot_col"] in iot_df.columns]
    n_rows = len(active)

    fig = plt.figure(figsize=(20, 5 * n_rows), facecolor=BG_DARK)
    fig.suptitle(
        f"Aranet Reference  vs  IoT System  —  BME280 / ENS160\n"
        f"{WINDOW_START}  to  {WINDOW_END}  UTC",
        color="black", fontsize=13, fontweight="bold", y=0.99,
    )
    gs = GridSpec(n_rows, 2, figure=fig,
                  hspace=0.55, wspace=0.28,
                  left=0.07, right=0.97, top=0.94, bottom=0.05)

    ar_idx   = aranet_win.set_index("time")
    iot_idx  = iot_df.set_index("time")
    stats_rows = []

    for row, m in enumerate(active):
        ar_ser  = ar_idx[aranet_cols[m["ar_key"]]].dropna()
        iot_ser = iot_idx[m["iot_col"]].dropna()

        ax_ts = fig.add_subplot(gs[row, 0])
        ax_sc = fig.add_subplot(gs[row, 1])
        style_ax(ax_ts)
        style_ax(ax_sc)

        # ── time series ───────────────────────────────────────────────────
        ax_ts.plot(ar_ser.index, ar_ser.values,
                   color=m["c_ar"], lw=2, marker="o", ms=4, zorder=3,
                   label="Aranet reference")
        ax_ts.plot(iot_ser.index, iot_ser.values,
                   color=m["c_iot"], lw=1.2, alpha=0.85, zorder=2,
                   label="IoT system (BME280)")

        # ── DS18B20 substrate overlay on temperature panel ────────────────
        sub_stats    = None
        ar_sub_stats = None
        if m.get("iot_col2") and m["iot_col2"] in iot_idx.columns:
            sub_ser = iot_idx[m["iot_col2"]].dropna()
            if not sub_ser.empty:
                ax_ts.plot(sub_ser.index, sub_ser.values,
                           color=m["c_iot2"], lw=1.4, alpha=0.9, zorder=2,
                           label=m["iot_col2_label"])
                print(f"  Plotted DS18B20: {len(sub_ser)} points, "
                      f"range {sub_ser.min():.2f}–{sub_ser.max():.2f} °C")

                # BME280 vs DS18B20 (5-second bins)
                bme_r5 = iot_ser.resample("5s").mean()
                sub_r5 = sub_ser.resample("5s").mean()
                both_bs = pd.concat([bme_r5, sub_r5], axis=1).dropna()
                both_bs.columns = ["bme", "sub"]
                if len(both_bs) >= 2:
                    sub_stats = compute_stats(both_bs["bme"], both_bs["sub"])
                    print(f"  BME280 vs DS18B20 — bias: {sub_stats['bias']:+.3f} °C  "
                          f"RMSE: {sub_stats['rmse']:.3f} °C  n: {sub_stats['n']}")

                # DS18B20 vs Aranet (RESAMPLE_FREQ bins)
                try:
                    ar_r2, sub_r2 = resample_align(ar_ser, sub_ser, RESAMPLE_FREQ)
                    if len(ar_r2) >= 2:
                        ar_sub_stats = compute_stats(ar_r2, sub_r2)
                        print(f"  DS18B20 vs Aranet — bias: {ar_sub_stats['bias']:+.3f} °C  "
                              f"RMSE: {ar_sub_stats['rmse']:.3f} °C  "
                              f"r: {fmt_r(ar_sub_stats['r'])}  n: {ar_sub_stats['n']}")
                except Exception:
                    pass
            else:
                print("  DS18B20 series empty after dropna — check merge")

        ax_ts.set_title(f"{m['label']}  ({m['unit']})", fontsize=10, pad=4)
        ax_ts.set_ylabel(m["unit"], fontsize=8)
        ax_ts.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        ax_ts.xaxis.set_major_locator(mdates.MinuteLocator(byminute=range(0, 60, 5)))
        plt.setp(ax_ts.xaxis.get_majorticklabels(),
                 rotation=30, ha="right", fontsize=7)
        ax_ts.legend(fontsize=7.5, facecolor=BG_PANEL,
                     labelcolor="black", framealpha=0.9, loc="best",
                     edgecolor=SPINE_COL)

        # ── scatter + stats ───────────────────────────────────────────────
        try:
            ref_r, sys_r = resample_align(ar_ser, iot_ser, RESAMPLE_FREQ)
            st = compute_stats(ref_r, sys_r)

            pad = (ref_r.max() - ref_r.min()) * 0.05 or 0.5
            lo  = min(ref_r.min(), sys_r.min()) - pad
            hi  = max(ref_r.max(), sys_r.max()) + pad
            ax_sc.plot([lo, hi], [lo, hi], color="#333333",
                       lw=1, ls="--", alpha=0.5, label="1:1 line")
            ax_sc.scatter(ref_r, sys_r, color=m["c_iot"],
                          s=40, alpha=0.85, zorder=3, edgecolors="none")
            ax_sc.set_xlim(lo, hi)
            ax_sc.set_ylim(lo, hi)
            ax_sc.set_xlabel(f"Aranet reference  ({m['unit']})", fontsize=8)
            ax_sc.set_ylabel(f"IoT system  ({m['unit']})", fontsize=8)
            ax_sc.legend(fontsize=7, facecolor=BG_PANEL,
                         labelcolor="black", framealpha=0.9,
                         edgecolor=SPINE_COL)

            title = (f"RMSE = {st['rmse']:.3f}   MAE = {st['mae']:.3f}   "
                     f"Bias = {st['bias']:+.3f}   r = {fmt_r(st['r'])}   n = {st['n']}")
            if m["key"] == "co2":
                title += "\n(eCO\u2082 MOX proxy vs Aranet NDIR \u2014 trend agreement only)"
            ax_sc.set_title(title, fontsize=8, pad=4)

            # ── Aranet vs BME280 row ──────────────────────────────────────
            stats_rows.append({
                "Comparison":        f"Aranet vs BME280 ({m['label']})",
                "Unit":              m["unit"],
                "Resample interval": RESAMPLE_FREQ,
                "N bins":            st["n"],
                "Bias":              f"{st['bias']:+.4f}",
                "MAE":               f"{st['mae']:.4f}",
                "RMSE":              f"{st['rmse']:.4f}",
                "Pearson r":         fmt_r(st["r"]),
                "p-value":           fmt_p(st["p"]),
            })

            # ── BME280 vs DS18B20 row ─────────────────────────────────────
            if sub_stats is not None:
                stats_rows.append({
                    "Comparison":        "BME280 vs DS18B20 in-substrate (Temperature)",
                    "Unit":              "°C",
                    "Resample interval": "5s",
                    "N bins":            sub_stats["n"],
                    "Bias":              f"{sub_stats['bias']:+.4f}",
                    "MAE":               f"{sub_stats['mae']:.4f}",
                    "RMSE":              f"{sub_stats['rmse']:.4f}",
                    "Pearson r":         fmt_r(sub_stats["r"]),
                    "p-value":           fmt_p(sub_stats["p"]),
                })

            # ── DS18B20 vs Aranet row ─────────────────────────────────────
            if ar_sub_stats is not None:
                stats_rows.append({
                    "Comparison":        "Aranet vs DS18B20 in-substrate (Temperature)",
                    "Unit":              "°C",
                    "Resample interval": RESAMPLE_FREQ,
                    "N bins":            ar_sub_stats["n"],
                    "Bias":              f"{ar_sub_stats['bias']:+.4f}",
                    "MAE":               f"{ar_sub_stats['mae']:.4f}",
                    "RMSE":              f"{ar_sub_stats['rmse']:.4f}",
                    "Pearson r":         fmt_r(ar_sub_stats["r"]),
                    "p-value":           fmt_p(ar_sub_stats["p"]),
                })

        except Exception as exc:
            ax_sc.text(0.5, 0.5, f"Cannot compute stats\n{exc}",
                       ha="center", va="center", color="black",
                       transform=ax_sc.transAxes, fontsize=9)

    plt.savefig(OUT_FIGURE, dpi=160, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close("all")
    print(f"  Saved figure  →  {OUT_FIGURE}")
    return stats_rows


# ─────────────────────────── MAIN ────────────────────────────────────────────

def main():
    if not os.path.exists(ARANET_CSV):
        sys.exit(f"Cannot find '{ARANET_CSV}'")

    print("\nLoading Aranet CSV …")
    aranet = load_aranet(ARANET_CSV)
    if ARANET_TZ_OFFSET_HOURS != 0:
        aranet["time"] = aranet["time"] - pd.Timedelta(hours=ARANET_TZ_OFFSET_HOURS)
        print(f"  Aranet timestamps shifted from AEST to UTC (−{ARANET_TZ_OFFSET_HOURS} h)")

    sensors = list_aranet_sensors(aranet)
    print(f"  Rows : {len(aranet):,}")
    print(f"  Range: {aranet['time'].min()}  to  {aranet['time'].max()}")
    print(f"  Sensor IDs: {sensors}")

    if ARANET_SENSOR is None:
        print("\n  Set ARANET_SENSOR in CONFIG and re-run.")
        return

    aranet_cols = get_aranet_cols(aranet, ARANET_SENSOR)
    print(f"  Using sensor  : {ARANET_SENSOR}")
    print(f"  Columns mapped: {list(aranet_cols.keys())}")

    if "co2" not in aranet_cols:
        print("  NOTE: CO2 column not found — checking columns:")
        for col in aranet.columns:
            if ARANET_SENSOR in col:
                print(f"    {col}")

    w0 = pd.Timestamp(WINDOW_START, tz="UTC")
    w1 = pd.Timestamp(WINDOW_END,   tz="UTC")
    aranet_win = aranet[(aranet["time"] >= w0) & (aranet["time"] < w1)].copy()
    print(f"  Aranet rows in window: {len(aranet_win)}")
    if len(aranet_win) < 2:
        print("  Too few rows — widen window or check timestamps.")
        return

    for path, name in [(BME_CSV, "BME_CSV"), (ENS_CSV, "ENS_CSV")]:
        if not os.path.exists(path):
            sys.exit(f"Cannot find '{path}'")

    print("\nLoading IoT CSVs …")
    iot_df = load_iot_csvs(BME_CSV, ENS_CSV, SUBSTRATE_CSV)

    iot_df = iot_df[(iot_df["time"] >= w0) & (iot_df["time"] < w1)].copy()
    print(f"  IoT rows in window: {len(iot_df):,}")
    print(f"  IoT columns: {list(iot_df.columns)}")

    if iot_df.empty:
        print("  No IoT rows in window.")
        return

    print("\nGenerating figure …")
    stats_rows = make_figure(aranet_win, aranet_cols, iot_df, ARANET_SENSOR)

    if stats_rows:
        st_df = pd.DataFrame(stats_rows)
        st_df.to_csv(OUT_STATS, index=False)
        print(f"  Saved stats   →  {OUT_STATS}")
        print(f"\n{'─'*72}")
        print(st_df.to_string(index=False))
        print(f"{'─'*72}")

    print("\nDone.")


if __name__ == "__main__":
    main()