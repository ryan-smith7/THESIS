#!/usr/bin/env python3
"""
timeDrift_node_v2.py — Sensor node drift vs Oracle SNTP (not PC NTP).

Same fix as timeDrift_gateway_v2.py — replaces PC NTP reference with an
Oracle SNTP query so the reference clock matches what the gateway syncs to.

One SNTP query is made per sync event immediately after readline() returns.
Both error_vs_pc and error_vs_oracle are logged.

Usage:
    python3 timeDrift_node_v2.py /dev/tty.usbserial-NODE 115200
"""

from serial import Serial
from repo.prac2.pythonFigures.sntp_oracle import sntp_query
import datetime, re, csv, sys, time, math

PORT     = sys.argv[1] if len(sys.argv) > 1 else "/dev/tty.usbserial-1140"
BAUD     = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
CSV_FILE = "node_drift_v2.csv"

SYNC_RE = re.compile(
    r"time_sync: synced UTC=(\d+)\.(\d{1,3})\s+local_ms=(\d+)\s+drift=(-?\d+)"
)

csvfile = open(CSV_FILE, "w", newline="")
writer  = csv.writer(csvfile)
writer.writerow([
    "pc_iso",
    "pc_utc_sec", "pc_utc_ms",
    "oracle_utc_sec", "oracle_utc_ms",
    "oracle_rtt_ms", "oracle_theta_ms",
    "node_utc_sec", "node_utc_ms",
    "node_local_ms", "drift_ppm",
    "error_vs_pc_ms",
    "error_vs_oracle_ms",
    "abs_error_vs_pc_ms",
    "abs_error_vs_oracle_ms",
])
csvfile.flush()

print(f"Opening {PORT} at {BAUD} baud...")
print(f"Logging to {CSV_FILE}  |  Press Ctrl+C to stop.\n")
print(f"{'Node UTC':<20} {'PPM':>6}  {'vs PC':>10}  {'vs Oracle':>12}  {'Oracle RTT':>12}")
print("─" * 70)

errs_pc     = []
errs_oracle = []

try:
    ser = Serial(PORT, BAUD, timeout=2)
    time.sleep(0.5)

    while True:
        try:
            raw    = ser.readline()
            pc_now = datetime.datetime.now(datetime.timezone.utc)
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
        node_ms   = int(m.group(2).ljust(3, "0"))
        local_ms  = int(m.group(3))
        drift_ppm = int(m.group(4))

        # ── SNTP query to Oracle ──────────────────────────────────────────────
        try:
            oracle_utc, oracle_rtt_ms, oracle_theta_ms = sntp_query()
        except Exception as e:
            print(f"  [Oracle query failed: {e} — skipping row]")
            continue

        oracle_sec = int(oracle_utc)
        oracle_ms_part = int((oracle_utc - oracle_sec) * 1000)

        pc_sec = int(pc_now.timestamp())
        pc_ms  = pc_now.microsecond // 1000

        node_utc_ms_total   = node_sec   * 1000 + node_ms
        pc_utc_ms_total     = pc_sec     * 1000 + pc_ms
        oracle_utc_ms_total = oracle_sec * 1000 + oracle_ms_part

        err_vs_pc     = node_utc_ms_total - pc_utc_ms_total
        err_vs_oracle = node_utc_ms_total - oracle_utc_ms_total

        errs_pc.append(err_vs_pc)
        errs_oracle.append(err_vs_oracle)

        node_str = f"{node_sec}.{node_ms:03d}"
        print(f"{node_str:<20} {drift_ppm:>6}  "
              f"{err_vs_pc:>+10.0f} ms  {err_vs_oracle:>+10.0f} ms  "
              f"{oracle_rtt_ms:>10} ms")

        writer.writerow([
            pc_now.isoformat(),
            pc_sec, pc_ms,
            oracle_sec, oracle_ms_part,
            oracle_rtt_ms, oracle_theta_ms,
            node_sec, node_ms,
            local_ms, drift_ppm,
            err_vs_pc,
            err_vs_oracle,
            abs(err_vs_pc),
            abs(err_vs_oracle),
        ])
        csvfile.flush()

except KeyboardInterrupt:
    pass
finally:
    def _stats(label, errs):
        if not errs:
            return
        n    = len(errs)
        mean = sum(errs) / n
        std  = math.sqrt(sum((e - mean)**2 for e in errs) / n)
        print(f"  {label}")
        print(f"    Mean={mean:+.1f} ms  σ={std:.1f} ms  "
              f"Min={min(errs):+.1f}  Max={max(errs):+.1f} ms")

    print(f"\n{'─'*55}")
    print(f"  Session statistics  (N = {len(errs_pc)})")
    print(f"{'─'*55}")
    _stats("Node vs PC NTP    ", errs_pc)
    _stats("Node vs Oracle SNTP", errs_oracle)
    print(f"{'─'*55}")
    if errs_pc and errs_oracle:
        oracle_pc_diff = sum(errs_pc)/len(errs_pc) - sum(errs_oracle)/len(errs_oracle)
        print(f"  Oracle vs PC offset : {oracle_pc_diff:+.1f} ms")
        print()
        if abs(oracle_pc_diff) < 100:
            print("  → Oracle and PC agree — ~960 ms is true sync error from gateway")
        else:
            print(f"  → Oracle is {oracle_pc_diff:+.0f} ms vs PC — matches gateway offset, system consistent")
    print(f"{'─'*55}")
    print(f"  Results saved to: {CSV_FILE}")

    csvfile.close()
    try:
        ser.close()
    except Exception:
        pass