import logging
import json
import os
import pyodbc
from datetime import datetime, timezone

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)


# ── Helpers ───────────────────────────────────────────────────────────────────

def _to_int(x, default=0):
    try:
        return int(float(x))
    except Exception:
        return default


def _to_float(x, default=0.0):
    try:
        return float(x)
    except Exception:
        return default


def _parse_iso(ts_raw: str):
    if not ts_raw:
        return datetime.utcnow()
    try:
        s = ts_raw.strip()
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        dt = datetime.fromisoformat(s)
        if dt.tzinfo is not None:
            dt = dt.astimezone(timezone.utc).replace(tzinfo=None)
        return dt
    except Exception:
        return datetime.utcnow()


def _sensor_timestamp(msg: dict):
    utc_sec = _to_int(msg.get("utc_sec"), 0)
    utc_ms  = _to_int(msg.get("utc_ms"),  0)
    if utc_sec > 1_700_000_000:
        return datetime.utcfromtimestamp(utc_sec + utc_ms / 1000.0)
    return datetime.utcnow()


def _connect():
    server   = os.environ.get("SQL_SERVER",   "iot-telemetry-server-ryan-smith.database.windows.net")
    database = os.environ.get("SQL_DATABASE", "iot-telemetry-db")
    password = os.environ.get("SQL_PASSWORD")
    if not password:
        raise RuntimeError("Missing SQL_PASSWORD environment variable")
    conn_str = (
        "DRIVER={ODBC Driver 18 for SQL Server};"
        f"SERVER={server};"
        f"DATABASE={database};"
        "UID=sqladmin;"
        f"PWD={password};"
        "Encrypt=yes;"
        "TrustServerCertificate=no;"
        "Connection Timeout=60;"
    )
    return pyodbc.connect(conn_str)


# ── Type detection ────────────────────────────────────────────────────────────

def _detect_type(msg: dict) -> str:
    logger.warning("TOP LEVEL KEYS: %s", list(msg.keys()))
    header = msg.get("header", {}) or {}
    if header.get("messageType"):
        return header["messageType"]
    if "bme280"      in msg: return "bme280"
    if "ens160"      in msg: return "ens160"
    if "environment" in msg: return "environment"   # legacy combined
    if "spectrum"    in msg: return "spectrum"
    if "soil_temperature" in msg: return "soil_temperature"
    if "soil"        in msg: return "soil"
    if "battery"     in msg: return "battery"
    if "sound"       in msg: return "sound"
    if "current"     in msg: return "current"
    return "unknown"


# ── Modality handlers ─────────────────────────────────────────────────────────
#
# All handlers use MERGE (SQL Server upsert) instead of plain INSERT.
# Deduplication key: device_id + timestamp (derived from utc_sec/utc_ms).
#
# This handles two re-send scenarios:
#   1. SD drain resume — sensor node re-sends records from file start after
#      a power cycle mid-drain (position is RAM-only, lost on reboot)
#   2. Event Hub at-least-once delivery — Azure may deliver the same event
#      to multiple Function instances simultaneously
#
# WHEN NOT MATCHED — insert the row (first time seen)
# WHEN MATCHED     — no-op (already exists, silently skip)
#
# Requires unique indexes on each table:
#   CREATE UNIQUE INDEX UX_bme280_device_time  ON telemetry_bme280  (device_id, timestamp);
#   CREATE UNIQUE INDEX UX_ens160_device_time  ON telemetry_ens160  (device_id, timestamp);
#   CREATE UNIQUE INDEX UX_spectrum_device_time ON telemetry_spectrum (device_id, timestamp);
#   CREATE UNIQUE INDEX UX_soil_device_time    ON telemetry_soil    (device_id, timestamp);
#   CREATE UNIQUE INDEX UX_battery_device_time ON telemetry_battery (device_id, timestamp);
#   CREATE UNIQUE INDEX UX_sound_device_time   ON telemetry_sound   (device_id, received_time);
#   CREATE UNIQUE INDEX UX_current_device_time ON telemetry_current (device_id, timestamp);


def _handle_bme280(cursor, msg: dict, body: str):
    device_id = msg.get("deviceId", "unknown")
    timestamp = _sensor_timestamp(msg)
    bme       = msg.get("bme280", {}) or {}

    temperature_c    = _to_float(bme.get("temperature_c"),    0.0)
    humidity_percent = _to_float(bme.get("humidity_percent"), 0.0)
    pressure_hpa     = _to_float(bme.get("pressure_hpa"),     0.0)

    cursor.execute(
        """
        MERGE telemetry_bme280 AS target
        USING (VALUES (?, ?, ?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                temperature_c, humidity_percent, pressure_hpa,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                temperature_c, humidity_percent, pressure_hpa,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.temperature_c, source.humidity_percent, source.pressure_hpa,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        temperature_c, humidity_percent, pressure_hpa,
        'GW-01', body
    )
    logger.info("bme280 upserted: dev=%s T=%.2f RH=%.2f P=%.3f",
                device_id, temperature_c, humidity_percent, pressure_hpa)


def _handle_ens160(cursor, msg: dict, body: str):
    device_id = msg.get("deviceId", "unknown")
    timestamp = _sensor_timestamp(msg)
    ens       = msg.get("ens160", {}) or {}

    eco2 = _to_int(ens.get("eco2_ppm"), 0)
    tvoc = _to_int(ens.get("tvoc_ppb"), 0)
    aqi  = _to_int(ens.get("aqi"),      0)

    cursor.execute(
        """
        MERGE telemetry_ens160 AS target
        USING (VALUES (?, ?, ?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                eco2_ppm, tvoc_ppb, aqi,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                eco2_ppm, tvoc_ppb, aqi,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.eco2_ppm, source.tvoc_ppb, source.aqi,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        eco2, tvoc, aqi,
        'GW-01', body
    )
    logger.info("ens160 upserted: dev=%s eCO2=%d TVOC=%d AQI=%d",
                device_id, eco2, tvoc, aqi)


def _handle_spectrum(cursor, msg: dict, body: str):
    device_id = msg.get("deviceId", "unknown")
    timestamp = _sensor_timestamp(msg)
    spec      = msg.get("spectrum", {}) or {}
 
    # Receive µW/m² integers, convert to mW/m² floats for SQL storage
    def uW_to_mW(key):
        return _to_int(spec.get(key), 0) / 1000.0
 
    ch = [
        uW_to_mW("405nm"),
        uW_to_mW("425nm"),
        uW_to_mW("450nm"),
        uW_to_mW("475nm"),
        uW_to_mW("515nm"),
        uW_to_mW("550nm"),
        uW_to_mW("555nm"),
        uW_to_mW("600nm"),
        uW_to_mW("640nm"),
        uW_to_mW("690nm"),
        uW_to_mW("745nm"),
        uW_to_mW("855nm"),
        uW_to_mW("VIS"),
    ]
 
    cursor.execute(
        """
        MERGE telemetry_spectrum AS target
        USING (VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                AS7343_405nm, AS7343_425nm, AS7343_450nm, AS7343_475nm,
                AS7343_515nm, AS7343_550nm, AS7343_555nm, AS7343_600nm,
                AS7343_640nm, AS7343_690nm, AS7343_745nm, AS7343_855nm,
                AS7343_visible,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                AS7343_405nm, AS7343_425nm, AS7343_450nm, AS7343_475nm,
                AS7343_515nm, AS7343_550nm, AS7343_555nm, AS7343_600nm,
                AS7343_640nm, AS7343_690nm, AS7343_745nm, AS7343_855nm,
                AS7343_visible,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.AS7343_405nm, source.AS7343_425nm,
                source.AS7343_450nm, source.AS7343_475nm,
                source.AS7343_515nm, source.AS7343_550nm,
                source.AS7343_555nm, source.AS7343_600nm,
                source.AS7343_640nm, source.AS7343_690nm,
                source.AS7343_745nm, source.AS7343_855nm,
                source.AS7343_visible,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        *ch,
        'GW-01', body
    )
    logger.info(
        "spectrum upserted: dev=%s 450nm=%.3f mW/m2 VIS=%.3f mW/m2",
        device_id, ch[2], ch[12]
    )
 
def _handle_soil(cursor, msg: dict, body: str):
    device_id        = msg.get("deviceId", "unknown")
    timestamp        = _sensor_timestamp(msg)
    soil             = msg.get("soil", {}) or {}
    soil_vwc_percent = _to_float(soil.get("vwc_percent"), 0.0)

    cursor.execute(
        """
        MERGE telemetry_soil AS target
        USING (VALUES (?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                soil_vwc_percent,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                soil_vwc_percent,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.soil_vwc_percent,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        soil_vwc_percent,
        'GW-01', body
    )
    logger.info("soil upserted: dev=%s vwc=%.2f", device_id, soil_vwc_percent)

def _handle_soil_temperature(cursor, msg: dict, body: str):
    device_id   = msg.get("deviceId", "unknown")
    timestamp   = _sensor_timestamp(msg)
    soil_temp   = msg.get("soil_temperature", {}) or {}
    temperature = _to_float(soil_temp.get("temperature_c"), 0.0)
 
    cursor.execute(
        """
        MERGE telemetry_soil_temperature AS target
        USING (VALUES (?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                temperature_c,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                temperature_c,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.temperature_c,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        temperature,
        'GW-01', body
    )
    logger.info("soil_temperature upserted: dev=%s T=%.4f", device_id, temperature)

def _handle_battery(cursor, msg: dict, body: str):
    device_id = msg.get("deviceId", "unknown")
    timestamp = _sensor_timestamp(msg)
    bat       = msg.get("battery", {}) or {}

    batt_mv      = _to_int(bat.get("mV"),            0)
    batt_pct     = _to_int(bat.get("pct"),           0)
    rate_pct_hr  = _to_float(bat.get("rate_pct_hr"), 0.0)

    cursor.execute(
        """
        MERGE telemetry_battery AS target
        USING (VALUES (?, ?, ?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                batt_mv, batt_pct, rate_pct_hr,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                batt_mv, batt_pct, rate_pct_hr,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.batt_mv, source.batt_pct, source.rate_pct_hr,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        batt_mv, batt_pct, rate_pct_hr,
        'GW-01', body
    )
    logger.info("battery upserted: dev=%s mV=%d pct=%d",
                device_id, batt_mv, batt_pct)


def _handle_sound(cursor, msg: dict, body: str):
    device_id = msg.get("deviceId", "unknown")
    snd       = msg.get("sound", {}) or {}
    rms_dbfs  = _to_float(snd.get("rms_dbfs"), 0.0)
    bins      = snd.get("bins", []) or []
    timestamp = _sensor_timestamp(msg)

    cursor.execute(
        """
        MERGE telemetry_sound AS target
        USING (VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))
            AS source (
                device_id, received_time,
                uptime_ms, rms_dbfs,
                bin_low_hz, bin_res_hz,
                bins_json,
                gateway_id, message_id,
                schema_version, message_type,
                raw_payload
            )
        ON  target.device_id    = source.device_id
        AND target.received_time = source.received_time
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, received_time,
                uptime_ms, rms_dbfs,
                bin_low_hz, bin_res_hz,
                bins_json,
                gateway_id, message_id,
                schema_version, message_type,
                raw_payload
            )
            VALUES (
                source.device_id, source.received_time,
                source.uptime_ms, source.rms_dbfs,
                source.bin_low_hz, source.bin_res_hz,
                source.bins_json,
                source.gateway_id, source.message_id,
                source.schema_version, source.message_type,
                source.raw_payload
            );
        """,
        device_id, timestamp, None, rms_dbfs,
        43, 43, json.dumps(bins),
        'GW-01', '', '1', 'sound', body[:4000]
    )
    logger.info("sound upserted: dev=%s rms=%.2f bins=%d",
                device_id, rms_dbfs, len(bins))


def _handle_current(cursor, msg: dict, body: str):
    device_id  = msg.get("deviceId", "unknown")
    timestamp  = _sensor_timestamp(msg)
    cur        = msg.get("current", {}) or {}

    current_ma = _to_float(cur.get("current_mA"), 0.0)
    voltage_mv = _to_int(cur.get("voltage_mV"),   0)

    cursor.execute(
        """
        MERGE telemetry_current AS target
        USING (VALUES (?, ?, ?, ?, ?, ?))
            AS source (
                device_id, timestamp,
                current_mA, voltage_mV,
                gateway_id, raw_payload
            )
        ON  target.device_id = source.device_id
        AND target.timestamp = source.timestamp
        WHEN NOT MATCHED THEN
            INSERT (
                device_id, timestamp,
                current_mA, voltage_mV,
                gateway_id, raw_payload
            )
            VALUES (
                source.device_id, source.timestamp,
                source.current_mA, source.voltage_mV,
                source.gateway_id, source.raw_payload
            );
        """,
        device_id, timestamp,
        current_ma, voltage_mv,
        'GW-01', body
    )
    logger.info("current upserted: dev=%s mA=%.3f mV=%d",
                device_id, current_ma, voltage_mv)


# ── Main ──────────────────────────────────────────────────────────────────────

def main(events) -> None:
    logger.warning("TRIGGER FIRED: received batch of %d events", len(events))

    try:
        conn   = _connect()
        cursor = conn.cursor()
    except Exception as e:
        logger.error("Failed to connect to SQL: %s", e, exc_info=True)
        return

    counts = {
        "bme280": 0, "ens160": 0,
        "environment": 0, "spectrum": 0, "soil_temperature": 0,
        "soil": 0,                           
        "battery": 0, "sound": 0, "current": 0,
        "legacy": 0, "skipped": 0
    }

    for event in events:
        try:
            body   = event.get_body().decode("utf-8", errors="ignore")
            logger.info("Message body prefix: %s", body[:200])
            parsed = json.loads(body)

            messages = parsed if isinstance(parsed, list) else [parsed]

            for msg in messages:
                msg_type = _detect_type(msg)

                if   msg_type == "bme280":      _handle_bme280(cursor, msg, body);   counts["bme280"]   += 1
                elif msg_type == "ens160":       _handle_ens160(cursor, msg, body);   counts["ens160"]   += 1
                elif msg_type == "spectrum":     _handle_spectrum(cursor, msg, body); counts["spectrum"] += 1
                elif msg_type == "soil_temperature":   _handle_soil_temperature(cursor, msg, body); counts["soil_temperature"] += 1
                elif msg_type == "soil":         _handle_soil(cursor, msg, body);     counts["soil"]     += 1
                elif msg_type == "battery":      _handle_battery(cursor, msg, body);  counts["battery"]  += 1
                elif msg_type in ("sound", "sound_spectrum"):
                                                 _handle_sound(cursor, msg, body);    counts["sound"]    += 1
                elif msg_type == "current":      _handle_current(cursor, msg, body);  counts["current"]  += 1
                else:
                    logger.warning("Unknown message type '%s' — skipping. Body: %s",
                                   msg_type, body[:200])
                    counts["skipped"] += 1

        except Exception as e:
            logger.error("Failed to process event: %s", e, exc_info=True)

    try:
        conn.commit()
    except Exception as e:
        logger.error("Commit failed: %s", e, exc_info=True)
    finally:
        try:
            conn.close()
        except Exception:
            pass

    logger.warning(
        "Batch complete — bme=%d ens=%d env=%d spec=%d soil=%d soilT=%d bat=%d snd=%d cur=%d legacy=%d skipped=%d",
        counts["bme280"], counts["ens160"], counts["environment"],
        counts["spectrum"], counts["soil"], counts["soil_temperature"],
        counts["battery"], counts["sound"], counts["current"],
        counts["legacy"], counts["skipped"]
    )