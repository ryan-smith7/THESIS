#!/usr/bin/env python3
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import sys
from pathlib import Path

GATEWAY_CSV = sys.argv[1] if len(sys.argv) > 1 else "gateway_drift.csv"
NODE_CSV    = sys.argv[2] if len(sys.argv) > 2 else "node_drift_v2.csv"

# ── Load gateway ───────────────────────────────────────────────────────────────
gw = pd.read_csv(GATEWAY_CSV)
gw["t"] = pd.to_datetime(gw["pc_iso"], utc=True)
gw["elapsed_min"] = (gw["t"] - gw["t"].iloc[0]).dt.total_seconds() / 60

# v1 has rtt_ms, v2 has gw_rtt_ms
rtt_col = "rtt_ms" if "rtt_ms" in gw.columns else "gw_rtt_ms"
mean_rtt = gw[rtt_col].mean()
std_rtt  = gw[rtt_col].std()

C_RTT = "#2563EB"
C_PPM = "#DC2626"

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

# ── Figure 1: Gateway RTT ──────────────────────────────────────────────────────
fig1, ax1 = plt.subplots(figsize=(10, 4))
ax1.plot(gw["elapsed_min"], gw[rtt_col],
         color=C_RTT, lw=1.4, marker="o", ms=4, alpha=0.80,
         label=f"RTT  |  Mean {mean_rtt:.0f} ms  |  σ {std_rtt:.0f} ms  |  "
               f"Min/Max {gw[rtt_col].min():.0f}/{gw[rtt_col].max():.0f} ms  |  N={len(gw)}")
ax1.axhline(mean_rtt, color=C_RTT, lw=1.0, ls="--", alpha=0.7)
ax1.axhspan(mean_rtt - std_rtt, mean_rtt + std_rtt, alpha=0.10, color=C_RTT)
ax1.set_xlabel("Elapsed time (minutes)")
ax1.set_ylabel("Round-trip time (ms)")
ax1.set_title("Wifi Board SNTP Round-Trip Time to Oracle VM")
ax1.legend(fontsize=9, loc="upper left")
fig1.tight_layout()
fig1.savefig("fig_gateway_rtt.png", bbox_inches="tight")
print("Saved fig_gateway_rtt.png")

# ── Figure 2: Node drift PPM (only if file exists) ────────────────────────────
if not Path(NODE_CSV).exists():
    print(f"Skipping drift PPM plot — {NODE_CSV} not found")
    sys.exit(0)

nd = pd.read_csv(NODE_CSV)
if nd.empty:
    print("Node CSV is empty — skipping drift PPM plot")
    sys.exit(0)

nd["t"] = pd.to_datetime(nd["pc_iso"], utc=True)
nd["elapsed_min"] = (nd["t"] - nd["t"].iloc[0]).dt.total_seconds() / 60
mean_ppm = nd["drift_ppm"].mean()
std_ppm  = nd["drift_ppm"].std()

fig2, ax2 = plt.subplots(figsize=(10, 4))
ax2.plot(nd["elapsed_min"], nd["drift_ppm"],
         color=C_PPM, lw=1.2, marker="o", ms=3, alpha=0.75,
         label=f"Drift PPM  |  Mean {mean_ppm:.0f}  |  σ {std_ppm:.0f}  |  "
               f"Min/Max {nd['drift_ppm'].min():.0f}/{nd['drift_ppm'].max():.0f}  |  N={len(nd)}")
ax2.axhline(0,        color="black", lw=0.8, ls="--", alpha=0.35)
ax2.axhline(mean_ppm, color=C_PPM,   lw=1.0, ls="--", alpha=0.6)
ax2.axhspan(-500, 500, alpha=0.05, color="grey")
ax2.axhspan(mean_ppm - std_ppm, mean_ppm + std_ppm, alpha=0.10, color=C_PPM)
ax2.set_xlabel("Elapsed time (minutes)")
ax2.set_ylabel("Crystal drift (PPM)")
ax2.set_title("FTSP Regression Corrected Sensor Node Drift Over Time")
ax2.legend(fontsize=9, loc="upper right")
fig2.tight_layout()
fig2.savefig("fig_node_drift_ppm.png", bbox_inches="tight")
print("Saved fig_node_drift_ppm.png")