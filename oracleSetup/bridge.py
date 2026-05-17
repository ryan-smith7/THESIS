#!/usr/bin/env python3
"""
MQTT Bridge: Mosquitto (plaintext) → upload_relay.py (direct SQL)

ESP32 connects to Mosquitto on port 1883 (no TLS).
This script subscribes to all device topics on Mosquitto,
and forwards directly to the local
upload_relay.py /upload_csv endpoint (acts as Azure Function) → Azure SQL.

Architecture:
  ESP32 → Mosquitto:1883 → bridge.py → upload_relay:8080 → Azure SQL
"""

import paho.mqtt.client as mqtt
import json
import logging
import requests

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
log = logging.getLogger(__name__)

# ── Local Mosquitto broker ────────────────────────────────
LOCAL_HOST = "localhost"
LOCAL_PORT = 1883

# ── Relay endpoint ────────────────────────────────────────
RELAY_URL = "http://localhost:8080/upload_csv"

# ── Message type detection ────────────────────────────────
def detect_modality(msg: dict):
    if "bme280"           in msg: return "bme280"
    if "ens160"           in msg: return "ens160"
    if "spectrum"         in msg: return "as7343"
    if "soil_temperature" in msg: return "ds18b20"
    if "soil"             in msg: return "moisture"
    if "sound"            in msg: return "sound"
    if "battery"          in msg: return "battery"
    return None

# ── Convert live JSON msg → toSql-compatible flat dict ────
def msg_to_sql_row(msg: dict, modality: str):
    from datetime import datetime

    utc_sec = msg.get("utc_sec", 0)
    utc_ms  = msg.get("utc_ms",  0)
    dev     = msg.get("deviceId", "dev-1")

    if utc_sec > 1_700_000_000:
        dt = datetime.utcfromtimestamp(utc_sec + utc_ms / 1000.0)
        ts = dt.strftime('%Y-%m-%d %H:%M:%S.') + f"{utc_ms:03d}"
    else:
        ts = "unsynced"

    if modality == "bme280":
        b = msg.get("bme280", {})
        return { "device_id": dev, "datetime": ts,
                 "temp_c":    b.get("temperature_c",    0),
                 "rh_pct":    b.get("humidity_percent", 0),
                 "press_hPa": b.get("pressure_hpa",     0) }

    if modality == "ens160":
        e = msg.get("ens160", {})
        return { "device_id": dev, "datetime": ts,
                 "eco2_ppm": e.get("eco2_ppm", 0),
                 "tvoc_ppb": e.get("tvoc_ppb", 0),
                 "aqi":      e.get("aqi",      0) }

    if modality == "as7343":
        s = msg.get("spectrum", {})
        wl   = ["405nm","425nm","450nm","475nm","515nm","550nm","555nm",
                "600nm","640nm","690nm","745nm","855nm","VIS"]
        keys = ["F1_405nm_mWm2","F2_425nm_mWm2","FZ_450nm_mWm2","F3_475nm_mWm2",
                "F4_515nm_mWm2","F5_550nm_mWm2","FY_555nm_mWm2","FXL_600nm_mWm2",
                "F6_640nm_mWm2","F7_690nm_mWm2","F8_745nm_mWm2","NIR_855nm_mWm2",
                "VIS_broadband_mWm2"]
        row = { "device_id": dev, "datetime": ts }
        for k, w in zip(keys, wl):
            row[k] = s.get(w, 0) / 1000.0  # µW/m² → mW/m²
        return row

    if modality == "moisture":
        return { "device_id": dev, "datetime": ts,
                 "vwc_percent": msg.get("soil", {}).get("vwc_percent", 0) }

    if modality == "ds18b20":
        return { "device_id": dev, "datetime": ts,
                 "temp_c": msg.get("soil_temperature", {}).get("temperature_c", 0) }

    if modality == "battery":
        b = msg.get("battery", {})
        return { "device_id":   dev, "datetime": ts,
                 "batt_mv":     b.get("mV",         0),
                 "batt_pct":    b.get("pct",         0),
                 "rate_pct_hr": b.get("rate_pct_hr", 0.0) }

    if modality == "sound":
        snd = msg.get("sound", {})
        bins = snd.get("bins", [])
        return { "device_id": dev, "datetime": ts,
                 "uptime_ms": snd.get("uptime_ms", msg.get("uptime_ms", 0)),
                 "rms_dbfs":  snd.get("rms_dbfs", 0),
                 "bins":      bins }

    return None


# ── Local broker client ───────────────────────────────────
def on_local_connect(client, userdata, flags, rc):
    if rc == 0:
        log.info("Local Mosquitto connected OK")
        client.subscribe("devices/#", qos=1)
        log.info("Subscribed to devices/#")
    else:
        log.error(f"Local connect failed rc={rc}")

def on_local_disconnect(client, userdata, rc):
    log.warning(f"Local broker disconnected rc={rc} — will reconnect")

def on_message(client, userdata, msg):
    payload_str = msg.payload.decode('utf-8', errors='ignore')
    log.info(f"Received [{len(msg.payload)} bytes]: {payload_str[:200]}")

    try:
        parsed = json.loads(payload_str)
    except json.JSONDecodeError:
        log.error("Failed to parse JSON — skipping")
        return

    messages = parsed if isinstance(parsed, list) else [parsed]

    for m in messages:
        modality = detect_modality(m)
        if modality is None:
            log.warning(f"Unknown type — skipping: {str(m)[:100]}")
            continue
        row = msg_to_sql_row(m, modality)
        if row is None:
            continue
        try:
            resp = requests.post(RELAY_URL, json={ "modality": modality, "rows": [row] }, timeout=10)
            if resp.ok:
                log.info(f"{modality} → SQL OK")
            else:
                log.error(f"Relay error {resp.status_code}: {resp.text[:120]}")
        except Exception as e:
            log.error(f"Failed to post {modality}: {e}")


local = mqtt.Client(client_id="oracle-bridge-local", protocol=mqtt.MQTTv311,
                    clean_session=True)
local.on_connect    = on_local_connect
local.on_disconnect = on_local_disconnect
local.on_message    = on_message
local.reconnect_delay_set(min_delay=1, max_delay=10)

log.info("Starting MQTT bridge...")
log.info(f"  Local:  {LOCAL_HOST}:{LOCAL_PORT}")
log.info(f"  Relay:  {RELAY_URL}")

local.connect(LOCAL_HOST, LOCAL_PORT, keepalive=60)
local.loop_forever()