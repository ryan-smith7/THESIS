#!/usr/bin/env python3
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import sys

CSV = sys.argv[1] if len(sys.argv) > 1 else "ble_latency.csv"

df = pd.read_csv(CSV)
df["t"] = pd.to_datetime(df["pc_iso_gw"], utc=True)
df["elapsed_min"] = (df["t"] - df["t"].iloc[0]).dt.total_seconds() / 60

matched = df[df["utc_match"] == True].copy()

Q1    = matched["latency_ms"].quantile(0.25)
Q3    = matched["latency_ms"].quantile(0.75)
fence = Q3 + 1.5 * (Q3 - Q1)
clean    = matched[matched["latency_ms"] <= fence]
outliers = matched[matched["latency_ms"] >  fence]

med_l  = clean["latency_ms"].median()
mean_l = clean["latency_ms"].mean()
std_l  = clean["latency_ms"].std()
mn_l   = clean["latency_ms"].min()
mx_l   = clean["latency_ms"].max()

mean_ppm = matched["drift_ppm"].mean()
std_ppm  = matched["drift_ppm"].std()

C_LAT = "#2563EB"
C_PPM = "#DC2626"
C_OUT = "#F59E0B"

plt.rcParams.update({
    "font.family":       "sans-serif",
    "font.size":         11,
    "axes.titlesize":    12,
    "axes.labelsize":    11,
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "axes.grid":         True,
    "grid.alpha":        0.30,
    "grid.linestyle":    "--",
    "legend.framealpha": 0.9,
    "figure.dpi":        180,
})

# ── Figure 1: BLE latency over time ───────────────────────────────────────────
fig1, ax1 = plt.subplots(figsize=(10, 4))

ax1.scatter(clean["elapsed_min"], clean["latency_ms"],
            color=C_LAT, s=20, alpha=0.70, zorder=3,
            label=(f"BLE GATT latency  |  Median {med_l:.0f} ms  |  "
                   f"σ= {std_l:.1f} ms  |  Min/Max {mn_l:.0f}/{mx_l:.0f} ms  |  N={len(clean)}"))

if len(outliers):
    ax1.scatter(outliers["elapsed_min"], outliers["latency_ms"],
                color=C_OUT, s=35, marker="x", linewidths=1.4, zorder=4)

ax1.axhline(med_l, color=C_LAT, lw=1.2, ls=":", zorder=2)
ax1.axhspan(mean_l - std_l, mean_l + std_l, alpha=0.10, color=C_LAT)

pad = max(5, (mx_l - mn_l) * 0.08)
ax1.set_ylim(mn_l - pad, mx_l + pad)
ax1.set_xlabel("Elapsed time (minutes)")
ax1.set_ylabel("Latency (ms)")
ax1.set_title("BLE GATT Time-Sync Delivery Latency — Gateway → Sensor Node")
ax1.legend(fontsize=9, loc="upper right")
fig1.tight_layout()
fig1.savefig("fig_ble_latency.png", bbox_inches="tight")
print("Saved fig_ble_latency.png")

# ── Figure 2: drift PPM from BLE latency session ───────────────────────────────
fig2, ax2 = plt.subplots(figsize=(10, 4))

ax2.plot(matched["elapsed_min"], matched["drift_ppm"],
         color=C_PPM, lw=1.2, marker="o", ms=3, alpha=0.75,
         label=(f"Drift PPM  |  Mean {mean_ppm:.0f}  |  σ= {std_ppm:.0f}  |  "
                f"Min/Max {matched['drift_ppm'].min():.0f}/{matched['drift_ppm'].max():.0f}  |  "
                f"N={len(matched)}"))

ax2.axhline(0,        color="black", lw=0.8, ls="--", alpha=0.35)
ax2.axhline(mean_ppm, color=C_PPM,   lw=1.0, ls="--", alpha=0.6)
ax2.axhspan(-500, 500, alpha=0.05, color="grey")
ax2.axhspan(mean_ppm - std_ppm, mean_ppm + std_ppm, alpha=0.10, color=C_PPM)

ax2.set_xlabel("Elapsed time (minutes)")
ax2.set_ylabel("Drift (PPM)")
ax2.set_title("FTSP Regression Corrected Sensor Node Drift Over Time")
ax2.legend(fontsize=9, loc="upper right")
fig2.tight_layout()
fig2.savefig("fig_node_drift_ppm.png", bbox_inches="tight")
print("Saved fig_node_drift_ppm.png")
