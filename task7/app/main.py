# #!/usr/bin/env python3
# import re, json, threading
# from collections import deque
# from datetime import datetime, timezone, timedelta
# import serial
# from flask import Flask, jsonify, request

# # -------------------------------
# # Config / State
# # -------------------------------
# SERIAL_PORT = "/dev/tty.usbserial-0001"
# BAUD_RATE = 115200
# RING_BUFFER_MAXLEN = 1000

# ring_buffer = deque(maxlen=RING_BUFFER_MAXLEN)
# buffer_lock = threading.Lock()

# # Anchor to convert device uptime (ms) -> epoch (ms)
# last_anchor = {"uptime_ms": None, "epoch_ms": None}

# # -------------------------------
# # Helpers
# # -------------------------------
# def extract_json_content(text: str):
#     m = re.search(r"\{.*\}", text)
#     return m.group(0) if m else None

# def now_ms() -> int:
#     return int(datetime.now(timezone.utc).timestamp() * 1000)

# def parse_from_to():
#     """Parse Grafana query params ?from=${__from}&to=${__to} (ms epoch or ISO)."""
#     def to_dt(v):
#         if not v: return None
#         v = v.strip()
#         # ms / s epoch
#         if re.fullmatch(r"\d{10,13}", v):
#             ms = int(v) if len(v) == 13 else int(v) * 1000
#             return datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
#         # ISO 8601
#         try:
#             return datetime.fromisoformat(v.replace("Z", "+00:00"))
#         except Exception:
#             return None

#     start = to_dt(request.args.get("from"))
#     end   = to_dt(request.args.get("to"))
#     if not end:
#         end = datetime.now(timezone.utc)
#     if not start:
#         start = end - timedelta(hours=1)
#     return start, end

# def uptime_to_epoch_ms(uptime_ms: int):
#     """Map device uptime to real epoch using the latest anchor."""
#     a_up, a_ep = last_anchor["uptime_ms"], last_anchor["epoch_ms"]
#     if a_up is None or a_ep is None:
#         return None
#     delta = max(a_up - uptime_ms, 0)
#     return a_ep - delta

# # -------------------------------
# # UART Serial Reader
# # -------------------------------
# def read_serial():
#     try:
#         ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
#         print(f"[INFO] Connected {SERIAL_PORT} @ {BAUD_RATE}")
#     except Exception as e:
#         print(f"[ERROR] open serial: {e}")
#         return

#     while True:
#         try:
#             line = ser.readline().decode("utf-8", errors="ignore").strip()
#             if not line:
#                 continue
#             print(line)
#             js = extract_json_content(line)
#             if not js:
#                 continue

#             try:
#                 msg = json.loads(js)
#             except json.JSONDecodeError:
#                 print(f"[WARN] bad JSON: {js[:120]}...")
#                 continue

#             # Stamp arrival time
#             msg["_arrival_epoch_ms"] = now_ms()

#             # Update uptime->epoch anchor if rtc_time present
#             up = msg.get("rtc_time")
#             try:
#                 up = int(up)
#                 # Only move the anchor forward
#                 if last_anchor["uptime_ms"] is None or up > last_anchor["uptime_ms"]:
#                     last_anchor["uptime_ms"] = up
#                     last_anchor["epoch_ms"]  = msg["_arrival_epoch_ms"]
#             except Exception:
#                 pass

#             with buffer_lock:
#                 ring_buffer.append(msg)

#         except Exception as e:
#             print(f"[ERROR] serial read: {e}")

# # -------------------------------
# # Flask
# # -------------------------------
# app = Flask(__name__)

# @app.get("/")
# def health_check():
#     return jsonify({"status": "OK"}), 200

# @app.get("/query")
# def query():
#     start_dt, end_dt = parse_from_to()
#     start_ms = int(start_dt.timestamp() * 1000)
#     end_ms   = int(end_dt.timestamp() * 1000)

#     # series[name] = [[val, ts_ms], ...]
#     series = {}

#     with buffer_lock:
#         for msg in list(ring_buffer):
#             dev_id  = msg.get("dev_id")
#             values  = msg.get("values", [])
#             rtc_raw = msg.get("rtc_time")

#             if dev_id is None or not values:
#                 continue

#             # Timestamp (prefer uptime->epoch, else arrival, else now)
#             ts_ms = None
#             try:
#                 ts_ms = uptime_to_epoch_ms(int(rtc_raw))
#             except Exception:
#                 pass
#             if ts_ms is None:
#                 ts_ms = msg.get("_arrival_epoch_ms", now_ms())

#             # range filter
#             if not (start_ms <= ts_ms <= end_ms):
#                 continue

#             # --------- Device mappings ----------
#             if dev_id == 1 and len(values) >= 3:
#                 names = ["BME280_Temperature", "BME280_Pressure", "BME280_Humidity"]
#                 for i, name in enumerate(names):
#                     try:
#                         val = float(values[i])
#                         series.setdefault(name, []).append([val, ts_ms])
#                     except Exception:
#                         pass

#             elif dev_id == 2 and len(values) >= 3:
#                 names = ["ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI"]
#                 for i, name in enumerate(names):
#                     try:
#                         val = float(values[i])
#                         series.setdefault(name, []).append([val, ts_ms])
#                     except Exception:
#                         pass

#             elif dev_id == 3:
#                 # values like "λ=450nm:123"
#                 for v in values:
#                     m = re.match(r"λ=(\d+)nm:(\d+)", v)
#                     if m:
#                         wl  = m.group(1)
#                         val = float(m.group(2))
#                         series.setdefault(f"AS7343_{wl}nm", []).append([val, ts_ms])

#     # Sort points (optional)
#     for pts in series.values():
#         pts.sort(key=lambda p: p[1])

#     resp = [{"target": name, "datapoints": pts} for name, pts in series.items()]
#     return jsonify(resp), 200

# # -------------------------------
# # Serial writer / simple REPL
# # -------------------------------
# def write_serial(cmd: str):
#     try:
#         with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
#             ser.write((cmd + "\n").encode("utf-8"))
#             ser.flush()
#     except Exception as e:
#         print(f"[ERROR] write serial: {e}")

# def screen_loop():
#     print("Type 'buffer' to show last 10, 'exit' to quit.")
#     while True:
#         cmd = input("> ").strip()
#         if cmd == "exit":
#             print("Bye.")
#             break
#         elif cmd == "buffer":
#             with buffer_lock:
#                 for m in list(ring_buffer)[-10:]:
#                     print(json.dumps(m, indent=2))
#         elif cmd:
#             write_serial(cmd)
            
#             # --- place with your other imports/helpers ---
# import re, json
# from datetime import datetime, timezone

# def iso_utc(ms: int) -> str:
#     return datetime.fromtimestamp(ms/1000, tz=timezone.utc).isoformat().replace("+00:00", "Z")

# def parse_from_to_iso():
#     def to_dt(v):
#         if not v: return None
#         v = v.strip()
#         if re.fullmatch(r"\d{10,13}", v):
#             ms = int(v) if len(v)==13 else int(v)*1000
#             return datetime.fromtimestamp(ms/1000, tz=timezone.utc)
#         try:
#             return datetime.fromisoformat(v.replace("Z","+00:00"))
#         except:
#             return None

#     end   = to_dt(request.args.get("to"))   or datetime.now(timezone.utc)
#     start = to_dt(request.args.get("from")) or (end - timedelta(hours=1))
#     return start, end

# def parse_targets_param(param: str):
#     """Support {A,B,C} or single A."""
#     if not param: return []
#     s = param.strip()
#     if s.startswith("{") and s.endswith("}"):
#         s = s[1:-1]
#     return [p.strip() for p in s.split(",") if p.strip()]

# @app.get("/metrics")
# def metrics():
#     """
#     Infinity-style endpoint:
#       /metrics?target={AS7343_450nm,AS7343_555nm}&from=${__from:date:iso}&to=${__to:date:iso}
#     Returns:
#       [ {"metric": <name>, "value": <float>, "time": <ISO8601 Z>}, ... ]
#     """
#     start_dt, end_dt = parse_from_to_iso()
#     start_ms = int(start_dt.timestamp()*1000)
#     end_ms   = int(end_dt.timestamp()*1000)

#     requested = parse_targets_param(request.args.get("target"))
#     want_all = (len(requested) == 0)

#     rows = []

#     with buffer_lock:
#         for msg in list(ring_buffer):
#             dev_id  = msg.get("dev_id")
#             values  = msg.get("values", [])
#             if dev_id is None or not values:
#                 continue

#             # --- timestamp (prefer uptime->epoch; else arrival/now) ---
#             ts_ms = None
#             try:
#                 ts_ms = uptime_to_epoch_ms(int(msg.get("rtc_time")))
#             except Exception:
#                 pass
#             if ts_ms is None:
#                 ts_ms = msg.get("_arrival_epoch_ms", now_ms())

#             if not (start_ms <= ts_ms <= end_ms):
#                 continue

#             # --- map device -> metric names and emit rows ---
#             if dev_id == 1 and len(values) >= 3:
#                 pairs = [("BME280_Temperature", values[0]),
#                          ("BME280_Pressure",    values[1]),
#                          ("BME280_Humidity",    values[2])]
#                 for name, val in pairs:
#                     if want_all or name in requested:
#                         try:
#                             rows.append({"metric": name, "value": float(val), "time": iso_utc(ts_ms)})
#                         except: pass

#             elif dev_id == 2 and len(values) >= 3:
#                 pairs = [("ENS160_eCO2", values[0]),
#                          ("ENS160_TVOC", values[1]),
#                          ("ENS160_AQI",  values[2])]
#                 for name, val in pairs:
#                     if want_all or name in requested:
#                         try:
#                             rows.append({"metric": name, "value": float(val), "time": iso_utc(ts_ms)})
#                         except: pass
#             elif dev_id == 3:
#                 # entries like "λ=450nm:123"
#                 for v in values:
#                     m = re.match(r"λ=(\d+)nm:(\d+)", v)
#                     if m:
#                         wl = int(m.group(1))
#                         name = "AS7343_VISIBLE" if wl == 999 else f"AS7343_{wl}nm"
#                         if want_all or name in requested:
#                             rows.append({
#                                 "metric": name,
#                                 "value": float(m.group(2)),
#                                 "time": iso_utc(ts_ms)
#                             })
                            
#     # (Optional) sort by time for neatness
#     rows.sort(key=lambda r: r["time"])
#     return jsonify(rows), 200

# @app.get("/metrics/names")
# def metric_names_static():
#     wl = [405,425,450,475,515,550,555,600,640,690,745,855]
#     names = [
#         "BME280_Temperature","BME280_Pressure","BME280_Humidity",
#         "ENS160_eCO2","ENS160_TVOC","ENS160_AQI", "AS7343_VISIBLE"
#         *[f"AS7343_{w}nm" for w in wl]
#     ]
#     return jsonify(names), 200


# # -------------------------------
# # Main
# # -------------------------------
# if __name__ == "__main__":
#     t = threading.Thread(target=read_serial, daemon=True)
#     t.start()

#     shell = threading.Thread(target=screen_loop, daemon=True)
#     shell.start()

#     print("[INFO] Running at http://0.0.0.0:5050")
#     app.run(host="0.0.0.0", port=5050)



#!/usr/bin/env python3
import re
import json
import threading
from collections import deque
from datetime import datetime, timezone, timedelta

import serial
from flask import Flask, jsonify, request

# -------------------------------
# Config / State
# -------------------------------
SERIAL_PORT = "/dev/tty.usbserial-0001"
BAUD_RATE = 115200
RING_BUFFER_MAXLEN = 1000

ring_buffer = deque(maxlen=RING_BUFFER_MAXLEN)
buffer_lock = threading.Lock()

# Anchor to convert device uptime (ms) -> epoch (ms)
last_anchor = {"uptime_ms": None, "epoch_ms": None}

# -------------------------------
# Helpers
# -------------------------------
def extract_json_content(text: str):
    """Return the first {...} JSON object in a line or None."""
    m = re.search(r"\{.*\}", text)
    return m.group(0) if m else None

def now_ms() -> int:
    return int(datetime.now(timezone.utc).timestamp() * 1000)

def parse_from_to():
    """Parse Grafana query params ?from=${__from}&to=${__to} (ms epoch or ISO)."""
    def to_dt(v):
        if not v:
            return None
        v = v.strip()
        # ms/s epoch
        if re.fullmatch(r"\d{10,13}", v):
            ms = int(v) if len(v) == 13 else int(v) * 1000
            return datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
        # ISO 8601
        try:
            return datetime.fromisoformat(v.replace("Z", "+00:00"))
        except Exception:
            return None

    end   = to_dt(request.args.get("to"))   or datetime.now(timezone.utc)
    start = to_dt(request.args.get("from")) or (end - timedelta(hours=1))
    return start, end

def parse_from_to_iso():
    """Same as parse_from_to but used by /metrics (returns datetimes)."""
    def to_dt(v):
        if not v:
            return None
        v = v.strip()
        if re.fullmatch(r"\d{10,13}", v):
            ms = int(v) if len(v) == 13 else int(v) * 1000
            return datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
        try:
            return datetime.fromisoformat(v.replace("Z", "+00:00"))
        except Exception:
            return None

    end   = to_dt(request.args.get("to"))   or datetime.now(timezone.utc)
    start = to_dt(request.args.get("from")) or (end - timedelta(hours=1))
    return start, end

def iso_utc(ms: int) -> str:
    """Epoch ms -> ISO8601 Z string."""
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).isoformat().replace("+00:00", "Z")

def parse_targets_param(param: str):
    """Support {A,B,C} or single A."""
    if not param:
        return []
    s = param.strip()
    if s.startswith("{") and s.endswith("}"):
        s = s[1:-1]
    return [p.strip() for p in s.split(",") if p.strip()]

def uptime_to_epoch_ms(uptime_ms: int):
    """Map device uptime to real epoch using the latest anchor."""
    a_up, a_ep = last_anchor["uptime_ms"], last_anchor["epoch_ms"]
    if a_up is None or a_ep is None:
        return None
    # If device uptime is earlier than our anchor, go backwards by delta
    delta = max(a_up - uptime_ms, 0)
    return a_ep - delta

def coerce_float(x):
    try:
        return float(x)
    except Exception:
        return None

def flatten_env(payload: dict):
    """Yield (metric_name, value) for environment, mapped to legacy names."""
    env = payload.get("environment") or {}
    mapping = {
        "temperature_c":   "BME280_Temperature",
        "humidity_percent":"BME280_Humidity",
        "pressure_hpa":    "BME280_Pressure",
        "eco2_ppm":        "ENS160_eCO2",
        "tvoc_ppb":        "ENS160_TVOC",
        "aqi":             "ENS160_AQI",
        # battery has no legacy equivalent; keep Env_Batt_mV
        "batt_mV":         "Env_Batt_mV",
    }
    for k, v in env.items():
        name = mapping.get(k)
        if not name:
            continue  # ignore unknown env keys
        val = coerce_float(v)
        if val is not None:
            yield name, val


def flatten_spectrum(payload: dict):
    """Yield (metric_name, value) for AS7343 channels in payload.spectrum."""
    spec = payload.get("spectrum") or {}
    for k, v in spec.items():
        if k.startswith("AS7343_"):
            val = coerce_float(v)
            if val is not None:
                yield k, val

def is_telemetry_envelope(msg: dict) -> bool:
    hdr = msg.get("header")
    return isinstance(hdr, dict) and hdr.get("messageType") == "telemetry" and "payload" in msg

def ts_ms_for_envelope(msg: dict):
    """
    Timestamp for telemetry envelope:
    ONLY use payload.uptime_ms -> anchor. If missing, fall back to arrival.
    """
    payload = msg.get("payload", {})
    up = payload.get("uptime_ms")
    try:
        ms = uptime_to_epoch_ms(int(up))
        if ms is not None:
            return ms
    except Exception:
        pass
    return msg.get("_arrival_epoch_ms", now_ms())

# -------------------------------
# UART Serial Reader
# -------------------------------
def read_serial():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"[INFO] Connected {SERIAL_PORT} @ {BAUD_RATE}")
    except Exception as e:
        print(f"[ERROR] open serial: {e}")
        return

    while True:
        try:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            print(line)

            js = extract_json_content(line)
            if not js:
                continue

            try:
                msg = json.loads(js)
            except json.JSONDecodeError:
                print(f"[WARN] bad JSON: {js[:120]}...")
                continue

            # Stamp arrival time
            msg["_arrival_epoch_ms"] = now_ms()

            # Update uptime->epoch anchor (prefer envelope uptime_ms)
            up = None
            if is_telemetry_envelope(msg):
                up = msg["payload"].get("uptime_ms")
            else:
                # keep legacy fallback if old flat schema still shows up
                up = msg.get("rtc_time")

            try:
                up = int(up)
                if last_anchor["uptime_ms"] is None or up > last_anchor["uptime_ms"]:
                    last_anchor["uptime_ms"] = up
                    last_anchor["epoch_ms"]  = msg["_arrival_epoch_ms"]
            except Exception:
                pass

            with buffer_lock:
                ring_buffer.append(msg)

        except Exception as e:
            print(f"[ERROR] serial read: {e}")

# -------------------------------
# Flask
# -------------------------------
app = Flask(__name__)

@app.get("/")
def health_check():
    return jsonify({"status": "OK"}), 200

@app.get("/query")
def query():
    """Grafana SimpleJSON-style: returns [{target, datapoints:[[val, ts], ...]}, ...]."""
    start_dt, end_dt = parse_from_to()
    start_ms = int(start_dt.timestamp() * 1000)
    end_ms = int(end_dt.timestamp() * 1000)

    series = {}  # name -> [[val, ts], ...]

    with buffer_lock:
        for msg in list(ring_buffer):

            # -------- New telemetry envelope (uses payload.uptime_ms only) --------
            if is_telemetry_envelope(msg):
                payload = msg["payload"]
                ts_ms = ts_ms_for_envelope(msg)
                if not (start_ms <= ts_ms <= end_ms):
                    continue

                # Spectrum (AS7343_*)
                for name, val in flatten_spectrum(payload):
                    series.setdefault(name, []).append([val, ts_ms])

                # Optional: environment metrics
                for name, val in flatten_env(payload):
                    series.setdefault(name, []).append([val, ts_ms])

                continue  # handled

            # -------- Legacy flat schema (dev_id/values) --------
            dev_id = msg.get("dev_id")
            values = msg.get("values", [])
            rtc_raw = msg.get("rtc_time")
            if dev_id is None or not values:
                continue

            ts_ms = None
            try:
                ts_ms = uptime_to_epoch_ms(int(rtc_raw))
            except Exception:
                pass
            if ts_ms is None:
                ts_ms = msg.get("_arrival_epoch_ms", now_ms())

            if not (start_ms <= ts_ms <= end_ms):
                continue

            if dev_id == 1 and len(values) >= 3:
                names = ["BME280_Temperature", "BME280_Pressure", "BME280_Humidity"]
                for i, name in enumerate(names):
                    val = coerce_float(values[i])
                    if val is not None:
                        series.setdefault(name, []).append([val, ts_ms])

            elif dev_id == 2 and len(values) >= 3:
                names = ["ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI"]
                for i, name in enumerate(names):
                    val = coerce_float(values[i])
                    if val is not None:
                        series.setdefault(name, []).append([val, ts_ms])

            elif dev_id == 3:
                # values like "λ=450nm:123"
                for v in values:
                    m = re.match(r"λ=(\d+)nm:(\d+)", v)
                    if m:
                        wl = m.group(1)
                        val = coerce_float(m.group(2))
                        if val is not None:
                            series.setdefault(f"AS7343_{wl}nm", []).append([val, ts_ms])

    # Sort points
    for pts in series.values():
        pts.sort(key=lambda p: p[1])

    resp = [{"target": name, "datapoints": pts} for name, pts in series.items()]
    return jsonify(resp), 200

@app.get("/metrics")
def metrics():
    """
    Infinity/JSON API style:
      /metrics?target={AS7343_450nm,AS7343_VISIBLE}&from=${__from:date:iso}&to=${__to:date:iso}
    Returns:
      [ {"metric": <name>, "value": <float>, "time": <ISO8601 Z>}, ... ]
    """
    start_dt, end_dt = parse_from_to_iso()
    start_ms = int(start_dt.timestamp() * 1000)
    end_ms = int(end_dt.timestamp() * 1000)

    requested = parse_targets_param(request.args.get("target"))
    want_all = (len(requested) == 0)

    rows = []

    def maybe_add(name, val, ts_ms):
        if val is None:
            return
        if want_all or name in requested:
            rows.append({"metric": name, "value": float(val), "time": iso_utc(ts_ms)})

    with buffer_lock:
        for msg in list(ring_buffer):

            if is_telemetry_envelope(msg):
                payload = msg["payload"]
                ts_ms = ts_ms_for_envelope(msg)  # uptime_ms -> anchor
                if not (start_ms <= ts_ms <= end_ms):
                    continue
                for name, val in flatten_spectrum(payload):
                    maybe_add(name, val, ts_ms)
                for name, val in flatten_env(payload):
                    maybe_add(name, val, ts_ms)
                continue

            # Legacy fallback
            dev_id = msg.get("dev_id")
            values = msg.get("values", [])
            if dev_id is None or not values:
                continue

            ts_ms = None
            try:
                ts_ms = uptime_to_epoch_ms(int(msg.get("rtc_time")))
            except Exception:
                pass
            if ts_ms is None:
                ts_ms = msg.get("_arrival_epoch_ms", now_ms())

            if not (start_ms <= ts_ms <= end_ms):
                continue

            if dev_id == 1 and len(values) >= 3:
                for name, val in [("BME280_Temperature", values[0]),
                                  ("BME280_Pressure",    values[1]),
                                  ("BME280_Humidity",    values[2])]:
                    maybe_add(name, coerce_float(val), ts_ms)

            elif dev_id == 2 and len(values) >= 3:
                for name, val in [("ENS160_eCO2", values[0]),
                                  ("ENS160_TVOC", values[1]),
                                  ("ENS160_AQI",  values[2])]:
                    maybe_add(name, coerce_float(val), ts_ms)

            elif dev_id == 3:
                for v in values:
                    m = re.match(r"λ=(\d+)nm:(\d+)", v)
                    if m:
                        wl = int(m.group(1))
                        name = f"AS7343_{wl}nm"
                        maybe_add(name, coerce_float(m.group(2)), ts_ms)

    rows.sort(key=lambda r: r["time"])
    return jsonify(rows), 200

@app.get("/metrics/names")
def metric_names_static():
    wl = [405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855]
    spec = [f"AS7343_{w}nm" for w in wl] + ["AS7343_VISIBLE"]

    # Legacy names only (collapsed), plus battery
    base = [
        "BME280_Temperature", "BME280_Pressure", "BME280_Humidity",
        "ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI",
        "Env_Batt_mV",
    ]
    return jsonify(base + spec), 200


# -------------------------------
# Serial writer / simple REPL
# -------------------------------
def write_serial(cmd: str):
    try:
        with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
            ser.write((cmd + "\n").encode("utf-8"))
            ser.flush()
    except Exception as e:
        print(f"[ERROR] write serial: {e}")

def screen_loop():
    print("Type 'buffer' to show last 10, 'exit' to quit.")
    while True:
        try:
            cmd = input("> ").strip()
        except EOFError:
            break

        if cmd == "exit":
            print("Bye.")
            break
        elif cmd == "buffer":
            with buffer_lock:
                for m in list(ring_buffer)[-10:]:
                    print(json.dumps(m, indent=2))
        elif cmd:
            write_serial(cmd)

# -------------------------------
# Main
# -------------------------------
if __name__ == "__main__":
    t = threading.Thread(target=read_serial, daemon=True)
    t.start()

    shell = threading.Thread(target=screen_loop, daemon=True)
    shell.start()

    print("[INFO] Running at http://0.0.0.0:5050")
    app.run(host="0.0.0.0", port=5050)
