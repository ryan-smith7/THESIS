"""
dual_uart_sync_test.py
======================
Simultaneously reads UTC timestamps from the gateway and a sensor node
over two UART connections and computes the node-to-gateway clock offset.

SETUP
-----
1.  Connect gateway UART to your laptop (USB-serial adapter or direct USB).
2.  Connect sensor node UART to your laptop (second USB-serial adapter).
3.  Both devices must be running firmware that prints a UTC timestamp line
    when it receives a trigger character over UART, in the format:

        UTC:<decimal_milliseconds>\n

    e.g.  UTC:1748950412345\n

    If your firmware prints UTC in a different format, adjust PARSE_UTC below.

4.  Set GATEWAY_PORT, NODE_PORT, and BAUD below to match your setup.
5.  Run:  python dual_uart_sync_test.py
6.  Press Ctrl+C to stop.  Results are saved to uart_sync_results.csv.

TYPICAL PORTS
-------------
Windows:   COM3, COM4  etc.
macOS:     /dev/tty.usbserial-XXXX  or  /dev/tty.usbmodem-XXXX
Linux:     /dev/ttyUSB0, /dev/ttyUSB1  or  /dev/ttyACM0, /dev/ttyACM1

    List available ports:
        python -c "import serial.tools.list_ports; [print(p) for p in serial.tools.list_ports.comports()]"

REQUIREMENTS
------------
    pip install pyserial pandas matplotlib
"""

import serial
import serial.tools.list_ports
import threading
import time
import csv
import sys
import os
import statistics

import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ══════════════════════════════ CONFIG ═══════════════════════════════════════

GATEWAY_PORT  = "/dev/ttyUSB0"    # ← change to your gateway serial port
NODE_PORT     = "/dev/ttyUSB1"    # ← change to your sensor node serial port
BAUD          = 115200            # must match firmware CONFIG_UART_CONSOLE_ON_DEV

# Character sent to trigger a UTC timestamp printout from each device.
# Adjust if your firmware uses a different trigger (e.g. b'\n', b'T', b'\r').
TRIGGER_CHAR  = b"t"

# How long to wait for a response from each device after sending trigger (s).
RESPONSE_TIMEOUT = 2.0

# Interval between paired samples (s).
SAMPLE_INTERVAL  = 2.0

# Total number of paired samples to collect (stop after this many).
# Set to 0 to run until Ctrl+C.
MAX_SAMPLES = 300   # ~10 minutes at 2 s interval

# Output files
CSV_OUT  = "uart_sync_results.csv"
PLOT_OUT = "uart_sync_offset.png"

# ══════════════════════════════ PARSE HELPER ═════════════════════════════════

def parse_utc_ms(line: str) -> int | None:
    """
    Extract UTC milliseconds from a firmware output line.

    Expected format:  UTC:<integer_ms>
    e.g.              UTC:1748950412345

    Adjust this function if your firmware prints differently.
    """
    line = line.strip()
    if line.upper().startswith("UTC:"):
        try:
            return int(line.split(":")[1].strip())
        except (IndexError, ValueError):
            return None
    return None

# ══════════════════════════════ READER THREAD ════════════════════════════════

class UARTReader:
    """
    Opens a serial port, sends a trigger character on demand,
    and returns the first parseable UTC line received within the timeout.
    """
    def __init__(self, port: str, baud: int, label: str):
        self.label   = label
        self.port    = port
        self.baud    = baud
        self._ser    = None
        self._result = None
        self._event  = threading.Event()

    def open(self):
        self._ser = serial.Serial(
            self.port, self.baud,
            timeout=RESPONSE_TIMEOUT,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE
        )
        # flush any startup noise
        time.sleep(0.05)
        self._ser.reset_input_buffer()
        print(f"  [{self.label}] opened {self.port} @ {self.baud} baud")

    def close(self):
        if self._ser and self._ser.is_open:
            self._ser.close()

    def _read_utc(self):
        """Read lines until a parseable UTC line is found or timeout."""
        deadline = time.monotonic() + RESPONSE_TIMEOUT
        while time.monotonic() < deadline:
            raw = self._ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode("utf-8", errors="ignore")
            except Exception:
                continue
            val = parse_utc_ms(line)
            if val is not None:
                self._result = val
                self._event.set()
                return
        # timed out
        self._result = None
        self._event.set()

    def request_utc(self) -> int | None:
        """Send trigger and block until UTC is received or timeout."""
        self._result = None
        self._event.clear()
        self._ser.reset_input_buffer()
        self._ser.write(TRIGGER_CHAR)
        self._ser.flush()
        t = threading.Thread(target=self._read_utc, daemon=True)
        t.start()
        self._event.wait(timeout=RESPONSE_TIMEOUT + 0.5)
        return self._result

# ══════════════════════════════ STATS HELPER ═════════════════════════════════

def print_summary(offsets_ms: list[float]):
    if not offsets_ms:
        return
    mean = statistics.mean(offsets_ms)
    med  = statistics.median(offsets_ms)
    sd   = statistics.stdev(offsets_ms) if len(offsets_ms) > 1 else 0.0
    mn   = min(offsets_ms)
    mx   = max(offsets_ms)
    print(f"\n  ── Offset summary (node UTC − gateway UTC) ──────────────")
    print(f"     N      : {len(offsets_ms)}")
    print(f"     Mean   : {mean:+.1f} ms")
    print(f"     Median : {med:+.1f} ms")
    print(f"     σ      : {sd:.1f} ms")
    print(f"     Min    : {mn:+.1f} ms")
    print(f"     Max    : {mx:+.1f} ms")
    print(f"  ─────────────────────────────────────────────────────────\n")

# ══════════════════════════════ PLOT ═════════════════════════════════════════

def save_plot(rows: list[dict]):
    if not rows:
        return
    df = pd.DataFrame(rows)
    df["elapsed_min"] = (df["host_ms"] - df["host_ms"].iloc[0]) / 60000.0

    offsets = df["offset_ms"].tolist()
    mean    = statistics.mean(offsets)
    sd      = statistics.stdev(offsets) if len(offsets) > 1 else 0.0
    med     = statistics.median(offsets)

    fig, ax = plt.subplots(figsize=(10, 4), facecolor="#FAFAFA")
    ax.set_facecolor("#F5F5F5")

    ax.axhline(mean, color="#2E5C8A", linewidth=1.2, linestyle="--",
               label=f"Mean {mean:+.1f} ms")
    ax.axhspan(mean - sd, mean + sd, alpha=0.15, color="#2E5C8A",
               label=f"Mean \u00B1 \u03C3 ({sd:.1f} ms)")
    ax.plot(df["elapsed_min"], df["offset_ms"],
            color="#E05A2B", linewidth=1.2, marker="o", markersize=3,
            label="Node \u2212 Gateway offset")

    ax.set_xlabel("Elapsed time (minutes)", fontsize=11)
    ax.set_ylabel("Node UTC \u2212 Gateway UTC (ms)", fontsize=11)
    ax.set_title(
        f"Node-to-Gateway UTC Offset  |  "
        f"Mean {mean:+.1f} ms  |  \u03C3 = {sd:.1f} ms  |  "
        f"Median {med:+.1f} ms  |  N = {len(offsets)}",
        fontsize=11
    )
    ax.legend(fontsize=9, framealpha=0.8)
    ax.grid(True, color="#DDDDDD", linewidth=0.6)
    ax.spines[["top","right"]].set_visible(False)

    plt.tight_layout()
    plt.savefig(PLOT_OUT, dpi=160, bbox_inches="tight")
    plt.close()
    print(f"  Plot saved  →  {PLOT_OUT}")

# ══════════════════════════════ MAIN ═════════════════════════════════════════

def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if ports:
        print("\nAvailable serial ports:")
        for p in ports:
            print(f"  {p.device:30s}  {p.description}")
    else:
        print("\nNo serial ports found.")
    print()

def main():
    print("\n── Dual-UART Node-to-Gateway UTC Offset Test ─────────────────")
    list_ports()

    # Check ports exist before opening
    for label, port in [("Gateway", GATEWAY_PORT), ("Node", NODE_PORT)]:
        if not os.path.exists(port) and not port.upper().startswith("COM"):
            print(f"  WARNING: {label} port '{port}' not found. "
                  f"Update CONFIG at top of script.")

    gw   = UARTReader(GATEWAY_PORT, BAUD, "GATEWAY")
    node = UARTReader(NODE_PORT,    BAUD, "NODE")

    try:
        gw.open()
        node.open()
    except serial.SerialException as e:
        print(f"\n  ERROR opening port: {e}")
        print("  Check port names and that no other process (e.g. screen, "
              "minicom, VSCode serial monitor) has the port open.")
        sys.exit(1)

    rows          = []
    offsets_ms    = []
    n_timeout_gw  = 0
    n_timeout_nd  = 0
    sample_count  = 0

    print(f"\n  Collecting samples every {SAMPLE_INTERVAL} s  "
          f"({'unlimited' if MAX_SAMPLES == 0 else MAX_SAMPLES} max). "
          f"Ctrl+C to stop early.\n")

    csvfile = open(CSV_OUT, "w", newline="")
    writer  = csv.DictWriter(csvfile, fieldnames=[
        "sample", "host_ms", "gateway_utc_ms", "node_utc_ms",
        "offset_ms", "elapsed_min"
    ])
    writer.writeheader()
    t0 = time.monotonic()

    try:
        while True:
            if MAX_SAMPLES > 0 and sample_count >= MAX_SAMPLES:
                print(f"\n  Reached MAX_SAMPLES = {MAX_SAMPLES}. Stopping.")
                break

            # Fire both triggers as close together as possible.
            # Two threads send simultaneously; host UTC is recorded at send.
            gw_result   = [None]
            node_result = [None]

            def gw_task():
                gw_result[0] = gw.request_utc()
            def node_task():
                node_result[0] = node.request_utc()

            host_ms = int(time.time() * 1000)

            t_gw   = threading.Thread(target=gw_task,   daemon=True)
            t_node = threading.Thread(target=node_task, daemon=True)
            t_gw.start()
            t_node.start()
            t_gw.join(timeout=RESPONSE_TIMEOUT + 1.0)
            t_node.join(timeout=RESPONSE_TIMEOUT + 1.0)

            gw_utc   = gw_result[0]
            node_utc = node_result[0]

            sample_count += 1
            elapsed_min   = (time.monotonic() - t0) / 60.0

            if gw_utc is None:
                n_timeout_gw += 1
                print(f"  [{sample_count:4d}] {elapsed_min:6.2f} min  "
                      f"GATEWAY timeout (check trigger / parse format)")
            elif node_utc is None:
                n_timeout_nd += 1
                print(f"  [{sample_count:4d}] {elapsed_min:6.2f} min  "
                      f"NODE timeout (check trigger / parse format)")
            else:
                offset = node_utc - gw_utc
                offsets_ms.append(float(offset))
                row = {
                    "sample":        sample_count,
                    "host_ms":       host_ms,
                    "gateway_utc_ms":gw_utc,
                    "node_utc_ms":   node_utc,
                    "offset_ms":     offset,
                    "elapsed_min":   round(elapsed_min, 3),
                }
                rows.append(row)
                writer.writerow(row)
                csvfile.flush()
                print(f"  [{sample_count:4d}] {elapsed_min:6.2f} min  "
                      f"GW={gw_utc}  NODE={node_utc}  "
                      f"offset={offset:+5d} ms")

            time.sleep(SAMPLE_INTERVAL)

    except KeyboardInterrupt:
        print("\n\n  Interrupted by user.")

    finally:
        csvfile.close()
        gw.close()
        node.close()

    print_summary(offsets_ms)

    if n_timeout_gw or n_timeout_nd:
        print(f"  Timeouts  →  gateway: {n_timeout_gw}   node: {n_timeout_nd}")
        print("  If you see many timeouts, check:")
        print("    1. TRIGGER_CHAR matches what your firmware expects.")
        print("    2. Firmware UTC output line starts with 'UTC:'.")
        print("    3. Baud rate matches firmware CONFIG_UART_CONSOLE_ON_DEV.")
        print("    4. The correct port is assigned to each device.\n")

    if rows:
        save_plot(rows)
        print(f"  CSV saved   →  {CSV_OUT}")
        print(f"  {len(rows)} valid samples, "
              f"{n_timeout_gw + n_timeout_nd} timeouts.\n")
    else:
        print("  No valid samples collected — nothing saved.\n")


if __name__ == "__main__":
    main()
