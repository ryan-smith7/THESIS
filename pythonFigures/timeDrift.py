#!/usr/bin/env python3
"""
Time sync drift measurement — sensor node UART vs PC UTC.

Parses Zephyr time_sync log lines from the sensor node serial port,
compares node-reported UTC against PC system UTC (NTP-disciplined),
and logs drift in milliseconds to CSV.

Usage:
    /opt/anaconda3/bin/python3 timesync_measure.py /dev/tty.usbserial-10 115200

Output:
    timesync_drift.csv  — one row per sync event
    Live drift printed to terminal
"""

from serial import Serial
import datetime
import re
import csv
import sys
import time

# ── Config ────────────────────────────────────────────────────────────────────
PORT     = sys.argv[1] if len(sys.argv) > 1 else "/dev/tty.usbserial-1140"
BAUD     = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
CSV_FILE = "timesync_drift.csv"

# Matches: time_sync: synced UTC=1779071162.122  local_ms=113030  drift=23 PPM
SYNC_RE = re.compile(
    r"time_sync: synced UTC=(\d+)\.(\d+)\s+local_ms=(\d+)\s+drift=(-?\d+)"
)

# ── CSV setup ─────────────────────────────────────────────────────────────────
csvfile = open(CSV_FILE, "w", newline="")
writer  = csv.writer(csvfile)
writer.writerow([
    "pc_utc_sec", "pc_utc_ms",
    "node_utc_sec", "node_utc_ms",
    "node_local_ms",
    "drift_ppm",
    "error_ms",
    "abs_error_ms",
])
csvfile.flush()

print(f"Opening {PORT} at {BAUD} baud...")
print(f"Logging drift to {CSV_FILE}")
print(f"Press Ctrl+C to stop.\n")
print(f"{'PC UTC':<26} {'Node UTC':<20} {'Error (ms)':>12} {'PPM':>8}")
print("-" * 70)

# ── Main loop ─────────────────────────────────────────────────────────────────
try:
    ser = Serial(PORT, BAUD, timeout=2)
    time.sleep(0.5)

    while True:
        try:
            raw = ser.readline()
        except Exception as e:
            print(f"Serial error: {e}")
            break

        if not raw:
            continue

        line = raw.decode("utf-8", errors="ignore").strip()

        m = SYNC_RE.search(line)
        if not m:
            continue

        node_sec  = int(m.group(1))
        node_ms   = int(m.group(2))
        local_ms  = int(m.group(3))
        drift_ppm = int(m.group(4))

        # PC UTC — timezone-aware, timestamp() gives correct UTC epoch
        pc_now = datetime.datetime.now(datetime.timezone.utc)
        pc_sec = int(pc_now.timestamp())
        pc_ms  = pc_now.microsecond // 1000

        # Reconstruct full UTC in milliseconds for both
        node_utc_ms_total = node_sec * 1000 + node_ms
        pc_utc_ms_total   = pc_sec   * 1000 + pc_ms

        error_ms = node_utc_ms_total - pc_utc_ms_total
        abs_err  = abs(error_ms)

        # ── Terminal output ───────────────────────────────────────────────────
        pc_str   = pc_now.strftime("%Y-%m-%d %H:%M:%S") + f".{pc_ms:03d}"
        node_str = f"{node_sec}.{node_ms:03d}"
        print(f"{pc_str:<26} {node_str:<20} {error_ms:>+12.0f} ms  {drift_ppm:>6} PPM")

        # ── CSV write ─────────────────────────────────────────────────────────
        writer.writerow([
            pc_sec, pc_ms,
            node_sec, node_ms,
            local_ms,
            drift_ppm,
            error_ms,
            abs_err,
        ])
        csvfile.flush()

except KeyboardInterrupt:
    print("\n\nStopped.")
    print(f"Results saved to {CSV_FILE}")
finally:
    csvfile.close()
    try:
        ser.close()
    except Exception:
        pass