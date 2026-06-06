#!/usr/bin/env python3
"""
timeDrift_ble_latency.py — BLE GATT time-sync latency measurement.

Reads both gateway and sensor node UART simultaneously. When the gateway
logs a "sent UTC=X to node N" event and the node subsequently logs
"synced UTC=X", the PC wall-clock difference between the two readline()
returns gives the BLE GATT delivery latency.

Timeline measured:
  t_gw  — PC time when gateway UART line is read  (post bt_gatt_write call)
  t_node — PC time when node UART line is read     (post GATT write callback)
  latency = t_node - t_gw

The UTC values in both log lines should match (same timestamp was sent).
A UTC mismatch indicates the node received a different broadcast cycle —
those pairs are flagged and excluded from latency stats.

Gateway log line matched:
  <inf> time_sync_writer: time_sync_writer: sent UTC=1780207009.10 to node 0

Node log line matched:
  <inf> time_sync: time_sync: synced UTC=1780207009.100  local_ms=...  drift=...

Usage:
    python3 timeDrift_ble_latency.py /dev/tty.usbserial-GW /dev/tty.usbserial-NODE [BAUD]

Output:
    ble_latency.csv   — one row per matched pair
    Live output to terminal
    Session statistics on Ctrl+C
"""

from serial import Serial
import datetime, re, csv, sys, time, math, threading, queue

# ── Config ─────────────────────────────────────────────────────────────────────
if len(sys.argv) < 3:
    print("Usage: timeDrift_ble_latency.py <PORT_GW> <PORT_NODE> [BAUD]")
    sys.exit(1)

PORT_GW   = sys.argv[1]
PORT_NODE = sys.argv[2]
BAUD      = int(sys.argv[3]) if len(sys.argv) > 3 else 115200
CSV_FILE  = "ble_latency.csv"

# How long to wait for the node to log its sync after the gateway sends (ms).
# FTSP sync interval is 30 s; 5000 ms covers GATT + Zephyr log queue delay.
MATCH_WINDOW_MS = 5000

# Gateway: sent UTC=1780207009.10 to node 0
GW_RE = re.compile(
    r"time_sync_writer: sent UTC=(\d+)\.(\d{1,3}) to node (\d+)"
)

# Node: synced UTC=1780207009.100  local_ms=...  drift=...
NODE_RE = re.compile(
    r"time_sync: synced UTC=(\d+)\.(\d{1,3})\s+local_ms=(\d+)\s+drift=(-?\d+)"
)

# ── Shared event queue ─────────────────────────────────────────────────────────
event_q   = queue.Queue()
stop_flag = threading.Event()

# ── CSV ────────────────────────────────────────────────────────────────────────
csvfile = open(CSV_FILE, "w", newline="")
writer  = csv.writer(csvfile)
writer.writerow([
    "pc_iso_gw", "pc_iso_node",
    "gw_utc_sec", "gw_utc_ms",
    "node_utc_sec", "node_utc_ms",
    "utc_match",          # True if gateway and node UTC agree
    "latency_ms",         # t_node_pc - t_gw_pc  (wall-clock delivery delay)
    "node_local_ms",
    "drift_ppm",
])
csvfile.flush()

# ── Reader thread ──────────────────────────────────────────────────────────────
def reader(port, role, baud):
    try:
        ser = Serial(port, baud, timeout=2)
        time.sleep(0.5)
        print(f"[{role}] Opened {port}")
    except Exception as e:
        print(f"[{role}] Failed to open {port}: {e}")
        return

    try:
        while not stop_flag.is_set():
            try:
                raw    = ser.readline()
                pc_now = datetime.datetime.now(datetime.timezone.utc)
            except Exception as e:
                print(f"[{role}] Serial error: {e}")
                break

            if not raw:
                continue

            line = raw.decode("utf-8", errors="ignore").strip()

            if role == "GW":
                m = GW_RE.search(line)
                if m:
                    event_q.put({
                        "role":     "GW",
                        "pc_now":   pc_now,
                        "pc_ms":    int(pc_now.timestamp() * 1000),
                        "utc_sec":  int(m.group(1)),
                        "utc_ms":   int(m.group(2).ljust(3, "0")),
                        "node_idx": m.group(3),
                    })
            else:
                m = NODE_RE.search(line)
                if m:
                    event_q.put({
                        "role":      "NODE",
                        "pc_now":    pc_now,
                        "pc_ms":     int(pc_now.timestamp() * 1000),
                        "utc_sec":   int(m.group(1)),
                        "utc_ms":    int(m.group(2).ljust(3, "0")),
                        "local_ms":  int(m.group(3)),
                        "drift_ppm": int(m.group(4)),
                    })
    finally:
        ser.close()

# ── Start readers ──────────────────────────────────────────────────────────────
t_gw   = threading.Thread(target=reader, args=(PORT_GW,   "GW",   BAUD), daemon=True)
t_node = threading.Thread(target=reader, args=(PORT_NODE, "NODE", BAUD), daemon=True)
t_gw.start()
t_node.start()

print(f"\nBLE GATT latency measurement")
print(f"  Gateway : {PORT_GW}")
print(f"  Node    : {PORT_NODE}")
print(f"  Baud    : {BAUD}")
print(f"  Window  : {MATCH_WINDOW_MS} ms match window")
print(f"  Output  : {CSV_FILE}")
print(f"Press Ctrl+C to stop.\n")
print(f"{'GW UTC':<22} {'Node UTC':<22} {'UTC match':>10}  {'Latency':>10}  {'PPM':>6}")
print("─" * 78)

# ── Pairing logic ──────────────────────────────────────────────────────────────
pending_gw   = []   # GW events waiting for a node match
pending_node = []   # Node events that arrived before GW (shouldn't happen often)
latencies    = []
mismatches   = 0

def try_match():
    """Try to pair the oldest pending GW event with a node event."""
    global mismatches
    if not pending_gw or not pending_node:
        return

    gw_ev   = pending_gw[0]
    node_ev = pending_node[0]

    age_ms = node_ev["pc_ms"] - gw_ev["pc_ms"]

    # Node arrived before gateway log — shouldn't happen, discard node event
    if age_ms < -200:
        pending_node.pop(0)
        return

    # Outside match window — GW event is stale, emit as unmatched
    if age_ms > MATCH_WINDOW_MS:
        pending_gw.pop(0)
        print(f"  [unmatched GW event — no node response within {MATCH_WINDOW_MS} ms]")
        return

    # Matched pair
    gw_ev   = pending_gw.pop(0)
    node_ev = pending_node.pop(0)

    latency_ms  = node_ev["pc_ms"] - gw_ev["pc_ms"]
    utc_match   = (gw_ev["utc_sec"] == node_ev["utc_sec"] and
                   abs(gw_ev["utc_ms"] - node_ev["utc_ms"]) <= 2)

    if utc_match:
        latencies.append(latency_ms)
    else:
        mismatches += 1

    gw_str   = f"{gw_ev['utc_sec']}.{gw_ev['utc_ms']:03d}"
    node_str = f"{node_ev['utc_sec']}.{node_ev['utc_ms']:03d}"
    match_str = "✓" if utc_match else "✗ MISMATCH"
    print(f"{gw_str:<22} {node_str:<22} {match_str:>10}  {latency_ms:>8} ms  "
          f"{node_ev['drift_ppm']:>6}")

    writer.writerow([
        gw_ev["pc_now"].isoformat(),
        node_ev["pc_now"].isoformat(),
        gw_ev["utc_sec"],   gw_ev["utc_ms"],
        node_ev["utc_sec"], node_ev["utc_ms"],
        utc_match,
        latency_ms,
        node_ev["local_ms"],
        node_ev["drift_ppm"],
    ])
    csvfile.flush()

try:
    while True:
        try:
            ev = event_q.get(timeout=1.0)
        except queue.Empty:
            # Expire stale GW events
            now_ms = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
            pending_gw[:]   = [e for e in pending_gw   if now_ms - e["pc_ms"] <= MATCH_WINDOW_MS]
            pending_node[:] = [e for e in pending_node if now_ms - e["pc_ms"] <= MATCH_WINDOW_MS]
            continue

        if ev["role"] == "GW":
            pending_gw.append(ev)
        else:
            pending_node.append(ev)

        try_match()

except KeyboardInterrupt:
    stop_flag.set()
finally:
    if latencies:
        n    = len(latencies)
        mean = sum(latencies) / n
        std  = math.sqrt(sum((x - mean) ** 2 for x in latencies) / n)

        print(f"\n{'─'*55}")
        print(f"  BLE GATT latency statistics  (N = {n} matched pairs)")
        print(f"{'─'*55}")
        print(f"  Mean latency  : {mean:.1f} ms")
        print(f"  Std deviation : {std:.1f} ms")
        print(f"  Min / Max     : {min(latencies)} / {max(latencies)} ms")
        print(f"  UTC mismatches: {mismatches}")
        print(f"{'─'*55}")
        print(f"  Results saved to: {CSV_FILE}")
    else:
        print("\nNo matched pairs captured.")

    csvfile.close()
    t_gw.join(timeout=2)
    t_node.join(timeout=2)
