#!/usr/bin/env python3
"""
Inter-node synchronisation measurement — Node 1 vs Node 2 simultaneous.

Both sensor nodes are plugged into the PC simultaneously. Each node's
serial port is read in a separate thread. When both nodes produce a
sync event within a configurable coincidence window, the pair is emitted
as a single row showing:

  - Each node's UTC value and its individual error vs PC UTC
  - The inter-node UTC difference (node1_utc_ms - node2_utc_ms)

This inter-node difference is the quantity that directly matters for
multi-modal sensor fusion — e.g. correlating a sound spike on Node 1
with a VOC spike on Node 2. It is independent of any systematic offset
shared by both nodes (e.g. HTTP server latency), making it a cleaner
measure of FTSP's synchronisation quality than either node's absolute
error alone.

Unpaired events (only one node synced within the window) are still
logged for completeness.

Usage:
    python3 timeDrift_internode.py /dev/tty.usbserial-N1 /dev/tty.usbserial-N2 115200

    Port order: first arg = Node 1, second arg = Node 2.
    Baud rate is optional (default 115200), applied to both ports.

Output:
    internode_sync.csv  — one row per event (paired or unpaired)
    Live output printed to terminal
    Session statistics on Ctrl+C
"""

from serial import Serial
import datetime
import re
import csv
import sys
import time
import math
import threading
import queue

# ── Config ────────────────────────────────────────────────────────────────────
if len(sys.argv) < 3:
    print("Usage: timeDrift_internode.py <PORT_NODE1> <PORT_NODE2> [BAUD]")
    sys.exit(1)

PORT_N1  = sys.argv[1]
PORT_N2  = sys.argv[2]
BAUD     = int(sys.argv[3]) if len(sys.argv) > 3 else 115200
CSV_FILE = "internode_sync.csv"

# Coincidence window: a sync event from each node must arrive within this
# many milliseconds of each other (PC wall clock) to be considered a pair.
# FTSP sync period is typically seconds; 2000 ms is generous but unambiguous.
COINCIDENCE_WINDOW_MS = 2000

# Matches: time_sync: synced UTC=1779071162.122  local_ms=113030  drift=23 PPM
SYNC_RE = re.compile(
    r"time_sync: synced UTC=(\d+)\.(\d{1,3})\s+local_ms=(\d+)\s+drift=(-?\d+)"
)

# ── Shared event queue ────────────────────────────────────────────────────────
# Each reader thread posts dicts; the main thread matches pairs.
event_q: queue.Queue = queue.Queue()
stop_event = threading.Event()

# ── CSV setup ─────────────────────────────────────────────────────────────────
csvfile = open(CSV_FILE, "w", newline="")
writer  = csv.writer(csvfile)
writer.writerow([
    "pair_type",            # "paired", "unpaired_n1", "unpaired_n2"
    "pc_iso_n1",
    "node1_utc_sec", "node1_utc_ms",
    "node1_error_ms",
    "node1_drift_ppm",
    "pc_iso_n2",
    "node2_utc_sec", "node2_utc_ms",
    "node2_error_ms",
    "node2_drift_ppm",
    "internode_diff_ms",    # node1_utc_ms_total - node2_utc_ms_total; "" if unpaired
])
csvfile.flush()

# ── Reader thread ─────────────────────────────────────────────────────────────
def reader(port: str, node_id: int, baud: int):
    """Reads sync log lines from one serial port and posts events to event_q."""
    try:
        ser = Serial(port, baud, timeout=2)
        time.sleep(0.5)
    except Exception as e:
        print(f"[Node {node_id}] Failed to open {port}: {e}")
        return

    print(f"[Node {node_id}] Opened {port}")

    try:
        while not stop_event.is_set():
            try:
                raw = ser.readline()
                pc_now = datetime.datetime.now(datetime.timezone.utc)
            except Exception as e:
                print(f"[Node {node_id}] Serial error: {e}")
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

            pc_sec = int(pc_now.timestamp())
            pc_ms  = pc_now.microsecond // 1000

            node_utc_ms_total = node_sec * 1000 + node_ms
            pc_utc_ms_total   = pc_sec   * 1000 + pc_ms
            error_ms = node_utc_ms_total - pc_utc_ms_total

            event_q.put({
                "node_id":    node_id,
                "pc_now":     pc_now,
                "pc_wall_ms": pc_utc_ms_total,
                "node_sec":   node_sec,
                "node_ms":    node_ms,
                "error_ms":   error_ms,
                "drift_ppm":  drift_ppm,
                "local_ms":   local_ms,
            })
    finally:
        ser.close()

# ── Header ────────────────────────────────────────────────────────────────────
print(f"Inter-node sync measurement")
print(f"  Node 1 : {PORT_N1}")
print(f"  Node 2 : {PORT_N2}")
print(f"  Baud   : {BAUD}")
print(f"  Window : ±{COINCIDENCE_WINDOW_MS} ms coincidence")
print(f"  Output : {CSV_FILE}")
print(f"Press Ctrl+C to stop.\n")
print(f"{'Type':<14} {'N1 UTC':<20} {'N1 err':>10}  {'N2 UTC':<20} {'N2 err':>10}  {'ΔT (ms)':>10}")
print("-" * 90)

# ── Start reader threads ──────────────────────────────────────────────────────
t1 = threading.Thread(target=reader, args=(PORT_N1, 1, BAUD), daemon=True)
t2 = threading.Thread(target=reader, args=(PORT_N2, 2, BAUD), daemon=True)
t1.start()
t2.start()

# ── Pairing logic (main thread) ───────────────────────────────────────────────
# Pending events that haven't yet been matched; keyed by node_id.
# At most one pending event per node at a time (FTSP sync rate is slow).
pending: dict[int, dict] = {}

# ── Accumulators ──────────────────────────────────────────────────────────────
internode_diffs = []
n1_errors       = []
n2_errors       = []
n_paired        = 0
n_unpaired      = 0

def flush_pending(node_id: int, reason: str):
    """Emit a pending event as unpaired."""
    global n_unpaired
    ev = pending.pop(node_id)
    n_unpaired += 1
    label = f"unpaired_n{node_id}"
    if node_id == 1:
        row = [label,
               ev["pc_now"].isoformat(), ev["node_sec"], ev["node_ms"], ev["error_ms"], ev["drift_ppm"],
               "", "", "", "", "", ""]
    else:
        row = [label,
               "", "", "", "", "",
               ev["pc_now"].isoformat(), ev["node_sec"], ev["node_ms"], ev["error_ms"], ev["drift_ppm"], ""]
    writer.writerow(row)
    csvfile.flush()
    n1_utc_col = f"{ev['node_sec']}.{ev['node_ms']:03d}" if node_id == 1 else "—"
    n1_err_col = f"{ev['error_ms']:+.0f} ms"           if node_id == 1 else "—"
    n2_utc_col = f"{ev['node_sec']}.{ev['node_ms']:03d}" if node_id == 2 else "—"
    n2_err_col = f"{ev['error_ms']:+.0f} ms"           if node_id == 2 else "—"
    print(f"{label:<14} {n1_utc_col:<20} {n1_err_col:>10}  "
          f"{n2_utc_col:<20} {n2_err_col:>10}  {'—':>10}  ({reason})")

try:
    while True:
        try:
            ev = event_q.get(timeout=1.0)
        except queue.Empty:
            # Check for stale pending events (no partner arrived within window)
            now_ms = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
            for nid in list(pending.keys()):
                age_ms = now_ms - pending[nid]["pc_wall_ms"]
                if age_ms > COINCIDENCE_WINDOW_MS:
                    flush_pending(nid, "timeout")
            continue

        node_id = ev["node_id"]
        partner = 3 - node_id  # node 1 ↔ node 2

        # Expire any stale pending event for THIS node (shouldn't happen at
        # normal FTSP rates, but guards against rapid re-sync corner cases)
        if node_id in pending:
            flush_pending(node_id, "superseded")

        # Check if partner has a recent pending event
        if partner in pending:
            age_ms = abs(ev["pc_wall_ms"] - pending[partner]["pc_wall_ms"])
            if age_ms <= COINCIDENCE_WINDOW_MS:
                # ── Emit paired row ───────────────────────────────────────────
                p = pending.pop(partner)
                n1_ev = ev if node_id == 1 else p
                n2_ev = ev if node_id == 2 else p

                n1_utc_total = n1_ev["node_sec"] * 1000 + n1_ev["node_ms"]
                n2_utc_total = n2_ev["node_sec"] * 1000 + n2_ev["node_ms"]
                diff_ms = n1_utc_total - n2_utc_total

                internode_diffs.append(diff_ms)
                n1_errors.append(n1_ev["error_ms"])
                n2_errors.append(n2_ev["error_ms"])
                n_paired += 1

                writer.writerow([
                    "paired",
                    n1_ev["pc_now"].isoformat(),
                    n1_ev["node_sec"], n1_ev["node_ms"],
                    n1_ev["error_ms"], n1_ev["drift_ppm"],
                    n2_ev["pc_now"].isoformat(),
                    n2_ev["node_sec"], n2_ev["node_ms"],
                    n2_ev["error_ms"], n2_ev["drift_ppm"],
                    diff_ms,
                ])
                csvfile.flush()

                n1_str = f"{n1_ev['node_sec']}.{n1_ev['node_ms']:03d}"
                n2_str = f"{n2_ev['node_sec']}.{n2_ev['node_ms']:03d}"
                print(f"{'paired':<14} {n1_str:<20} {n1_ev['error_ms']:>+10.0f} ms  "
                      f"{n2_str:<20} {n2_ev['error_ms']:>+10.0f} ms  {diff_ms:>+10.0f} ms")
                continue
            else:
                # Partner event too old — flush it as unpaired first
                flush_pending(partner, "window miss")

        # No usable partner yet; park this event
        pending[node_id] = ev

except KeyboardInterrupt:
    stop_event.set()
finally:
    # Flush any remaining pending events
    for nid in list(pending.keys()):
        flush_pending(nid, "session end")

    # ── Session statistics ────────────────────────────────────────────────────
    def stats_str(vals, label):
        if not vals:
            return f"  {label}: no data"
        n    = len(vals)
        mean = sum(vals) / n
        std  = math.sqrt(sum((v - mean) ** 2 for v in vals) / n)
        return (f"  {label}  (N={n})\n"
                f"    Mean : {mean:+.2f} ms   Std : {std:.2f} ms   "
                f"Range : [{min(vals):+.2f}, {max(vals):+.2f}] ms")

    print(f"\n{'─'*60}")
    print(f"  Session statistics")
    print(f"{'─'*60}")
    print(f"  Paired events    : {n_paired}")
    print(f"  Unpaired events  : {n_unpaired}")
    print()
    print(stats_str(internode_diffs, "Inter-node ΔT (N1 − N2)"))
    print(stats_str(n1_errors,       "Node 1 absolute error vs PC"))
    print(stats_str(n2_errors,       "Node 2 absolute error vs PC"))
    print(f"{'─'*60}")
    print(f"  Results saved to : {CSV_FILE}")

    csvfile.close()
    t1.join(timeout=2)
    t2.join(timeout=2)