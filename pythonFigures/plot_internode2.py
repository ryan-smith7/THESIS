#!/usr/bin/env python3
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import sys

CSV = sys.argv[1] if len(sys.argv) > 1 else "/mnt/user-data/uploads/1780229995271_internode_sync.csv"

df     = pd.read_csv(CSV)
paired = df[df["pair_type"] == "paired"].copy()
paired["t"] = pd.to_datetime(paired["pc_iso_n1"], utc=True)

# Trim to start from UTC 1780227359.738
start_mask = (paired["node1_utc_sec"] == 1780227359) & (paired["node1_utc_ms"] == 738)
start_idx  = paired.index[start_mask][0]
paired     = paired.loc[start_idx:].copy()
paired["elapsed_min"] = (paired["t"] - paired["t"].iloc[0]).dt.total_seconds() / 60

# Exclude outliers (>5 ms) from diff plot and histogram
clean    = paired[paired["internode_diff_ms"].abs() <= 5]
outliers = paired[paired["internode_diff_ms"].abs() >  5]

C_N1   = "#2563EB"
C_N2   = "#DC2626"
C_DIFF = "#16A34A"
C_OUT  = "#F59E0B"
ALPHA  = 0.70

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

fig, (ax_abs, ax_diff) = plt.subplots(2, 1, figsize=(11, 7), 
                                        gridspec_kw={"hspace": 0.45})

# ── Panel 1: N1 and N2 absolute error vs PC ───────────────────────────────────
ax_abs.plot(paired["elapsed_min"], paired["node1_error_ms"],
            color=C_N1, lw=1.2, marker="o", ms=2.5, alpha=ALPHA, label="Node 1")
ax_abs.plot(paired["elapsed_min"], paired["node2_error_ms"],
            color=C_N2, lw=1.2, marker="s", ms=2.5, alpha=ALPHA, label="Node 2")

ax_abs.axhline(paired["node1_error_ms"].median(), color=C_N1, lw=1.0, ls=":",
               alpha=0.7, label=f"N1 median ({paired['node1_error_ms'].median():+.0f} ms)")
ax_abs.axhline(paired["node2_error_ms"].median(), color=C_N2, lw=1.0, ls=":",
               alpha=0.7, label=f"N2 median ({paired['node2_error_ms'].median():+.0f} ms)")

ax_abs.set_xlabel("Elapsed time (minutes)")
ax_abs.set_ylabel("Clock error vs PC NTP (ms)")
ax_abs.set_title("Panel A — Node 1 and Node 2 absolute clock error vs PC NTP")
ax_abs.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{x:+.0f}"))
ax_abs.legend(fontsize=9, ncol=2)

# ── Panel 2: inter-node ΔT over time ─────────────────────────────────────────
ax_diff.scatter(clean["elapsed_min"], clean["internode_diff_ms"],
                color=C_DIFF, s=18, alpha=0.65, zorder=3,
                label=f"ΔT = N1 − N2  (N={len(clean)})")

if len(outliers):
    ax_diff.scatter(outliers["elapsed_min"], outliers["internode_diff_ms"],
                    color=C_OUT, s=60, marker="X", zorder=5,
                    label=f"BLE retransmit outlier (N={len(outliers)}, excluded from stats)")

ax_diff.axhline(0, color="black", lw=0.8, ls="--", alpha=0.4)
ax_diff.axhspan(-2, 2, alpha=0.10, color=C_DIFF)

median_diff = clean["internode_diff_ms"].median()
std_diff    = clean["internode_diff_ms"].std()
ax_diff.axhline(median_diff, color=C_DIFF, lw=1.2, ls=":",
                label=f"Median {median_diff:+.0f} ms  (σ = {std_diff:.2f} ms)")

# Annotate outliers
for _, row in outliers.iterrows():
    ax_diff.annotate(f"{row['internode_diff_ms']:.0f} ms\n(BLE retransmit)",
                     xy=(row["elapsed_min"], 5),
                     xytext=(row["elapsed_min"] - 8, 6.5),
                     fontsize=8, color=C_OUT,
                     arrowprops=dict(arrowstyle="->", color=C_OUT, lw=0.8))

ax_diff.set_xlabel("Elapsed time (minutes)")
ax_diff.set_ylabel("Inter-node ΔT (ms)")
ax_diff.set_title(f"Panel B — Inter-node UTC difference (N1 − N2):  "
                  f"{len(clean)/len(paired)*100:.1f}% of events within ±2 ms")
ax_diff.set_ylim(-5, 10)
ax_diff.legend(fontsize=9, ncol=2)

# Stats annotation box
stats_txt = (f"N = {len(clean)} paired events\n"
             f"Median = {median_diff:+.0f} ms\n"
             f"σ = {std_diff:.2f} ms\n"
             f"Range: [{int(clean['internode_diff_ms'].min()):+d}, "
             f"{int(clean['internode_diff_ms'].max()):+d}] ms")
ax_diff.text(0.98, 0.97, stats_txt, transform=ax_diff.transAxes,
             fontsize=8.5, va="top", ha="right",
             bbox=dict(boxstyle="round,pad=0.4", fc="white", ec="lightgrey", alpha=0.9))

fig.suptitle("Inter-node Time Synchronisation — Node 1 vs Node 2",
             fontsize=13, fontweight="bold")
fig.savefig("/home/claude/fig_internode2.png", bbox_inches="tight")
print("Saved.")
print(f"N paired: {len(paired)}, clean: {len(clean)}, outliers: {len(outliers)}")
print(f"Median ΔT: {median_diff:+.0f} ms, std: {std_diff:.2f} ms")
