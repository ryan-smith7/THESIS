import pandas as pd
import datetime

df = pd.read_csv("bme_times.csv", header=None, names=["wall", "sd"])
df["wall"] = pd.to_datetime(df["wall"], format="mixed")
df["sd"]   = pd.to_datetime(df["sd"],   format="mixed")

# Outage window defined by SD acquisition timestamp
# (not wall receive time — wall filter was double-counting live records
#  received after reconnect alongside the backfill records)
OUTAGE_START_UTC = datetime.datetime(2026, 5, 28, 13, 0, 19)  # last live UTC
RECONNECT_UTC    = datetime.datetime(2026, 5, 29, 0, 57, 34)  # first backfill UTC

backfill = df[
    (df["sd"] >= OUTAGE_START_UTC) &
    (df["sd"] <= RECONNECT_UTC)
].copy()
backfill = backfill.sort_values("sd").reset_index(drop=True)

# Gap between consecutive SD timestamps
backfill["sd_gap_s"] = backfill["sd"].diff().dt.total_seconds()

# Expected count: outage duration in seconds at 1 Hz
outage_duration_s = int((RECONNECT_UTC - OUTAGE_START_UTC).total_seconds())
total    = len(backfill)
gaps     = backfill[backfill["sd_gap_s"] > 5]

print(f"Outage duration        : {outage_duration_s // 3600} h "
      f"{(outage_duration_s % 3600) // 60} min "
      f"{outage_duration_s % 60} s  "
      f"({outage_duration_s:,} s)")
print(f"Backfill records       : {total:,}")
print(f"Expected at 1 Hz       : ~{outage_duration_s:,}")
print(f"Coverage               : {total / outage_duration_s * 100:.1f}%")
print(f"Missing records        : ~{outage_duration_s - total:,}")
print(f"Gaps > 5 s             : {len(gaps)}")
print(f"Largest gap            : {backfill['sd_gap_s'].max():.1f} s")
print(f"\nGap locations:")
print(gaps[["sd", "sd_gap_s"]].to_string())


# """
# plot_gaps.py
# ============
# Analyses gaps between consecutive SD timestamps in the BME280 backfill
# to show data continuity and identify any dropped records.

# Input:  bme_times.csv   (wall_receive_time, sd_timestamp — from bridge.log)
# Output: gap_analysis.png

# Usage:
#     python plot_gaps.py
#     python plot_gaps.py --csv bme_times.csv --out gap_analysis.png
# """

# import argparse
# import datetime
# import numpy as np
# import pandas as pd
# import matplotlib
# matplotlib.use("Agg")
# import matplotlib.pyplot as plt
# import matplotlib.gridspec as gridspec
# import matplotlib.dates as mdates

# # ── CONFIG ────────────────────────────────────────────────────────────────────
# RECONNECT_UTC = datetime.datetime(2026, 5, 29, 0, 57, 34)
# AEST          = datetime.timezone(datetime.timedelta(hours=10))

# # Gap larger than this is flagged as a missing-data event
# GAP_THRESHOLD_S = 5

# # Expected inter-sample period at 1 Hz
# EXPECTED_S = 1.0

# C_NORMAL  = "#2E5C8A"   # blue
# C_GAP     = "#E05A2B"   # orange
# C_REF     = "#AAAAAA"   # grey dashed
# C_GRID    = "#E0E0E0"

# DPI = 160
# # ─────────────────────────────────────────────────────────────────────────────


# def load_backfill(csv_path: str) -> pd.DataFrame:
#     df = pd.read_csv(csv_path, header=None, names=["wall", "sd"])
#     df["wall"] = pd.to_datetime(df["wall"], format="mixed")
#     df["sd"]   = pd.to_datetime(df["sd"],   format="mixed")

#     # Isolate backfill records: received after reconnect
#     bf = df[df["wall"] >= RECONNECT_UTC].copy()
#     bf = bf.sort_values("sd").reset_index(drop=True)
#     bf["sd_gap_s"] = bf["sd"].diff().dt.total_seconds()

#     # Convert to AEST for display
#     bf["sd_aest"] = bf["sd"].dt.tz_localize("UTC").dt.tz_convert(AEST)

#     return bf


# def summarise(bf: pd.DataFrame):
#     gaps = bf[bf["sd_gap_s"] > GAP_THRESHOLD_S]
#     total    = len(bf)
#     sd_range = (bf["sd"].max() - bf["sd"].min()).total_seconds()
#     expected = int(sd_range / EXPECTED_S)
#     coverage = total / expected * 100 if expected else 0

#     print(f"\n── Backfill gap analysis ─────────────────────────────────")
#     print(f"  Total backfill records : {total:,}")
#     print(f"  Expected at 1 Hz       : ~{expected:,}")
#     print(f"  Coverage               : {coverage:.1f}%")
#     print(f"  Missing records        : ~{expected - total:,}")
#     print(f"  Gaps > {GAP_THRESHOLD_S} s             : {len(gaps)}")
#     if len(gaps):
#         for _, row in gaps.iterrows():
#             sd_a = row["sd_aest"].strftime("%H:%M:%S AEST")
#             print(f"    {sd_a}  —  {row['sd_gap_s']:.0f} s gap  "
#                   f"(~{row['sd_gap_s']:.0f} missing records)")
#     print(f"──────────────────────────────────────────────────────────\n")
#     return gaps, expected, coverage


# def plot(bf: pd.DataFrame, gaps: pd.DataFrame,
#          expected: int, coverage: float, out_path: str):

#     fig = plt.figure(figsize=(12, 7), facecolor="#FAFAFA")
#     gs  = gridspec.GridSpec(
#         2, 1, height_ratios=[2, 1], hspace=0.35,
#         top=0.91, bottom=0.09, left=0.10, right=0.97
#     )

#     # ── Top panel: cumulative record count vs SD timestamp ────────────────────
#     ax1 = fig.add_subplot(gs[0])

#     # Ideal line — what perfect 1 Hz coverage looks like
#     sd_min  = bf["sd_aest"].iloc[0]
#     sd_max  = bf["sd_aest"].iloc[-1]
#     ideal_x = [sd_min, sd_max]
#     ideal_n = (sd_max - sd_min).total_seconds() / EXPECTED_S
#     ideal_y = [0, ideal_n]
#     ax1.plot(ideal_x, ideal_y,
#              color=C_REF, linewidth=1.2, linestyle="--",
#              label=f"Ideal 1 Hz coverage  (~{expected:,} records expected)",
#              zorder=1)

#     # Actual cumulative count
#     ax1.plot(bf["sd_aest"], np.arange(len(bf)),
#              color=C_NORMAL, linewidth=1.4,
#              label=f"Actual SD records received  ({len(bf):,} records, "
#                    f"{coverage:.1f}% coverage)",
#              zorder=2)

#     # Mark gap locations on the top panel
#     for _, row in gaps.iterrows():
#         ax1.axvline(row["sd_aest"], color=C_GAP,
#                     linewidth=1.2, linestyle=":", alpha=0.8, zorder=3)

#     ax1.set_ylabel("Cumulative record count", fontsize=10)
#     ax1.set_title(
#         "BME280 SD Card Backfill — Data Continuity Analysis\n"
#         "UQ Network Outage 28–29 May 2026",
#         fontsize=11, pad=8
#     )
#     ax1.legend(fontsize=8.5, framealpha=0.9, loc="upper left")
#     ax1.grid(True, color=C_GRID, linewidth=0.5, zorder=0)
#     ax1.spines[["top", "right"]].set_visible(False)
#     date_fmt = mdates.DateFormatter("%d %b\n%H:%M", tz=AEST)
#     ax1.xaxis.set_major_formatter(date_fmt)
#     ax1.xaxis.set_major_locator(mdates.HourLocator(interval=1, tz=AEST))
#     ax1.set_xlim(sd_min, sd_max)

#     # ── Bottom panel: inter-sample gap time series ─────────────────────────────
#     ax2 = fig.add_subplot(gs[1], sharex=ax1)

#     # Normal gaps (≤ threshold)
#     normal_mask = bf["sd_gap_s"] <= GAP_THRESHOLD_S
#     ax2.scatter(
#         bf.loc[normal_mask, "sd_aest"],
#         bf.loc[normal_mask, "sd_gap_s"],
#         s=0.8, color=C_NORMAL, alpha=0.4, linewidths=0,
#         label=f"Normal gap  (≤ {GAP_THRESHOLD_S} s)",
#         zorder=2
#     )

#     # Anomalous gaps
#     gap_mask = bf["sd_gap_s"] > GAP_THRESHOLD_S
#     ax2.scatter(
#         bf.loc[gap_mask, "sd_aest"],
#         bf.loc[gap_mask, "sd_gap_s"],
#         s=40, color=C_GAP, zorder=4,
#         label=f"Anomalous gap  (> {GAP_THRESHOLD_S} s)",
#         marker="^"
#     )

#     # Annotate each anomalous gap
#     for _, row in gaps.iterrows():
#         label = f"{row['sd_gap_s']:.0f} s\n({row['sd_gap_s']:.0f} records\nmissing)"
#         ax2.annotate(
#             label,
#             xy=(row["sd_aest"], row["sd_gap_s"]),
#             xytext=(10, 8), textcoords="offset points",
#             fontsize=7.5, color=C_GAP,
#             arrowprops=dict(arrowstyle="->", color=C_GAP, lw=0.7)
#         )

#     # Expected 1 Hz reference line
#     ax2.axhline(EXPECTED_S, color=C_REF, linewidth=0.9,
#                 linestyle="--", label="Expected 1 s interval", zorder=1)

#     ax2.set_ylabel("Inter-sample gap (s)", fontsize=10)
#     ax2.set_xlabel("SD acquisition timestamp (AEST)", fontsize=10)
#     ax2.legend(fontsize=8.5, framealpha=0.9, loc="upper right")
#     ax2.grid(True, color=C_GRID, linewidth=0.5, zorder=0)
#     ax2.spines[["top", "right"]].set_visible(False)
#     ax2.xaxis.set_major_formatter(date_fmt)
#     ax2.set_xlim(sd_min, sd_max)

#     # Log scale on y if the largest gap dwarfs the normal gaps
#     max_gap = bf["sd_gap_s"].max()
#     if max_gap > 100:
#         ax2.set_yscale("log")
#         ax2.set_ylabel("Inter-sample gap (s, log scale)", fontsize=10)

#     plt.savefig(out_path, dpi=DPI, bbox_inches="tight")
#     plt.close()
#     print(f"Figure saved → {out_path}")


# # ─────────────────────────────────────────────────────────────────────────────

# if __name__ == "__main__":
#     parser = argparse.ArgumentParser()
#     parser.add_argument("--csv", default="bme_times.csv")
#     parser.add_argument("--out", default="gap_analysis.png")
#     args = parser.parse_args()

#     print(f"Loading {args.csv} …")
#     bf = load_backfill(args.csv)
#     print(f"  {len(bf):,} backfill records isolated")

#     gaps, expected, coverage = summarise(bf)
#     plot(bf, gaps, expected, coverage, args.out)