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
    """
    Reconstruct a UTC datetime from utc_sec + utc_ms stamped on the sensor node.
    Falls back to datetime.utcnow() if utc_sec is zero or below the 2023 threshold
    (meaning the sensor had not yet received a time sync and sent uptime seconds).
    """
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

    if "environment" in msg:
        return "environment"
    if "spectrum" in msg:
        return "spectrum"
    if "soil" in msg:
        return "soil"
    if "battery" in msg:
        return "battery"
    if "sound" in msg:
        return "sound"
    if "current" in msg:
        return "current"

    return "unknown"


# ── Modality handlers ─────────────────────────────────────────────────────────

def _handle_environment(cursor, msg: dict, body: str):
    # batt_mV removed — battery arrives via its own dedicated modality
    device_id        = msg.get("deviceId", "unknown")
    timestamp        = _sensor_timestamp(msg)

    env = msg.get("environment", {}) or {}
    temperature_c    = _to_float(env.get("temperature_c"),    0.0)
    humidity_percent = _to_float(env.get("humidity_percent"), 0.0)
    pressure_hpa     = _to_float(env.get("pressure_hpa"),     0.0)
    eco2_ppm         = _to_int(env.get("eco2_ppm"),           0)
    tvoc_ppb         = _to_int(env.get("tvoc_ppb"),           0)
    aqi              = _to_int(env.get("aqi"),                0)

    cursor.execute(
        """
        INSERT INTO telemetry (
            device_id, timestamp, uptime_ms, proto_ver,
            temperature_c, humidity_percent, pressure_hpa,
            eco2_ppm, tvoc_ppb, aqi,
            as7343_405nm, as7343_425nm, as7343_450nm, as7343_475nm,
            as7343_515nm, as7343_550nm, as7343_555nm, as7343_600nm,
            as7343_640nm, as7343_690nm, as7343_745nm, as7343_855nm,
            as7343_visible,
            snd_rms_dbfs, snd_peak_freq_hz, snd_peak_mag,
            soil_vwc_percent,
            gateway_id, message_id, raw_payload
        ) VALUES (
            ?, ?, ?, ?,
            ?, ?, ?,
            ?, ?, ?,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL,
            NULL, NULL, NULL,
            NULL,
            ?, ?, ?
        )
        """,
        device_id, timestamp, None, None,
        temperature_c, humidity_percent, pressure_hpa,
        eco2_ppm, tvoc_ppb, aqi,
        'GW-01', '', body
    )
    logger.info("env inserted: dev=%s T=%.2f RH=%.2f", device_id, temperature_c, humidity_percent)


def _handle_spectrum(cursor, msg: dict, body: str):
    device_id  = msg.get("deviceId", "unknown")
    timestamp  = _sensor_timestamp(msg)

    spec = msg.get("spectrum", {}) or {}
    as7343_405 = _to_int(spec.get("AS7343_405nm"),  0)
    as7343_425 = _to_int(spec.get("AS7343_425nm"),  0)
    as7343_450 = _to_int(spec.get("AS7343_450nm"),  0)
    as7343_475 = _to_int(spec.get("AS7343_475nm"),  0)
    as7343_515 = _to_int(spec.get("AS7343_515nm"),  0)
    as7343_550 = _to_int(spec.get("AS7343_550nm"),  0)
    as7343_555 = _to_int(spec.get("AS7343_555nm"),  0)
    as7343_600 = _to_int(spec.get("AS7343_600nm"),  0)
    as7343_640 = _to_int(spec.get("AS7343_640nm"),  0)
    as7343_690 = _to_int(spec.get("AS7343_690nm"),  0)
    as7343_745 = _to_int(spec.get("AS7343_745nm"),  0)
    as7343_855 = _to_int(spec.get("AS7343_855nm"),  0)
    as7343_vis = _to_int(spec.get("AS7343_VISIBLE"), 0)

    cursor.execute(
        """
        INSERT INTO telemetry (
            device_id, timestamp, uptime_ms, proto_ver,
            temperature_c, humidity_percent, pressure_hpa,
            eco2_ppm, tvoc_ppb, aqi, batt_mv,
            as7343_405nm, as7343_425nm, as7343_450nm, as7343_475nm,
            as7343_515nm, as7343_550nm, as7343_555nm, as7343_600nm,
            as7343_640nm, as7343_690nm, as7343_745nm, as7343_855nm,
            as7343_visible,
            snd_rms_dbfs, snd_peak_freq_hz, snd_peak_mag,
            soil_vwc_percent,
            gateway_id, message_id, raw_payload
        ) VALUES (
            ?, ?, ?, ?,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?,
            NULL, NULL, NULL,
            NULL,
            ?, ?, ?
        )
        """,
        device_id, timestamp, None, None,
        as7343_405, as7343_425, as7343_450, as7343_475,
        as7343_515, as7343_550, as7343_555, as7343_600,
        as7343_640, as7343_690, as7343_745, as7343_855,
        as7343_vis,
        'GW-01', '', body
    )
    logger.info("spectrum inserted: dev=%s 450nm=%d vis=%d", device_id, as7343_450, as7343_vis)


def _handle_soil(cursor, msg: dict, body: str):
    device_id        = msg.get("deviceId", "unknown")
    timestamp        = _sensor_timestamp(msg)
    soil             = msg.get("soil", {}) or {}
    soil_vwc_percent = _to_float(soil.get("vwc_percent"), 0.0)

    cursor.execute(
        """
        INSERT INTO telemetry (
            device_id, timestamp, uptime_ms, proto_ver,
            temperature_c, humidity_percent, pressure_hpa,
            eco2_ppm, tvoc_ppb, aqi, batt_mv,
            as7343_405nm, as7343_425nm, as7343_450nm, as7343_475nm,
            as7343_515nm, as7343_550nm, as7343_555nm, as7343_600nm,
            as7343_640nm, as7343_690nm, as7343_745nm, as7343_855nm,
            as7343_visible,
            snd_rms_dbfs, snd_peak_freq_hz, snd_peak_mag,
            soil_vwc_percent,
            gateway_id, message_id, raw_payload
        ) VALUES (
            ?, ?, ?, ?,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL,
            NULL, NULL, NULL,
            ?,
            ?, ?, ?
        )
        """,
        device_id, timestamp, None, None,
        soil_vwc_percent,
        'GW-01', '', body
    )
    logger.info("soil inserted: dev=%s vwc=%.2f", device_id, soil_vwc_percent)


def _handle_battery(cursor, msg: dict, body: str):
    # sensor_dev_id comes from inside the battery object — the physical device
    # ID stamped by the sensor node, used to separate Node 1 vs Node 2 in Grafana.
    bat           = msg.get("battery", {}) or {}
    sensor_dev_id = _to_int(bat.get("sensor_dev_id"), 0)
    device_id     = f"dev-{sensor_dev_id}" if sensor_dev_id else msg.get("deviceId", "unknown")
    timestamp     = _sensor_timestamp(msg)
    batt_mv       = _to_int(bat.get("mV"),           0)
    batt_pct      = _to_int(bat.get("pct"),           0)
    batt_rate     = _to_float(bat.get("rate_pct_hr"), 0.0)

    cursor.execute(
        """
        INSERT INTO telemetry (
            device_id, timestamp, uptime_ms, proto_ver,
            temperature_c, humidity_percent, pressure_hpa,
            eco2_ppm, tvoc_ppb, aqi, batt_mv,
            as7343_405nm, as7343_425nm, as7343_450nm, as7343_475nm,
            as7343_515nm, as7343_550nm, as7343_555nm, as7343_600nm,
            as7343_640nm, as7343_690nm, as7343_745nm, as7343_855nm,
            as7343_visible,
            snd_rms_dbfs, snd_peak_freq_hz, snd_peak_mag,
            soil_vwc_percent,
            batt_pct, batt_rate_pct_hr,
            gateway_id, message_id, raw_payload
        ) VALUES (
            ?, ?, ?, ?,
            NULL, NULL, NULL,
            NULL, NULL, NULL, ?,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL,
            NULL, NULL, NULL,
            NULL,
            ?, ?,
            ?, ?, ?
        )
        """,
        device_id, timestamp, None, None,
        batt_mv,
        batt_pct, batt_rate,
        'GW-01', '', body
    )
    logger.info("battery inserted: dev=%s mV=%d pct=%d rate=%.1f ts=%s",
                device_id, batt_mv, batt_pct, batt_rate, timestamp)


def _handle_sound(cursor, msg: dict, body: str):
    if "sound" in msg:
        device_id = msg.get("deviceId", "unknown")
        snd       = msg.get("sound", {}) or {}
        rms_dbfs  = _to_float(snd.get("rms_dbfs"),  0.0)
        peak_freq = _to_int(snd.get("peak_freq_hz"), 0)
        peak_mag  = _to_float(snd.get("peak_mag"),   0.0)
        bins      = snd.get("bins", []) or []
        bin_low   = 43
        bin_res   = 43
        timestamp = _sensor_timestamp(msg)
    else:
        payload   = msg.get("payload", {}) or {}
        device_id = payload.get("deviceId", "unknown")
        rms_dbfs  = _to_float(payload.get("rms_dbfs"), 0.0)
        bin_low   = _to_int(payload.get("bin_low_hz"), 43)
        bin_res   = _to_int(payload.get("bin_res_hz"), 43)
        bins      = payload.get("bins", []) or []
        peak_freq = 0
        peak_mag  = 0.0
        timestamp = datetime.utcnow()

    cursor.execute(
        """
        INSERT INTO telemetry_sound (
            device_id, received_time,
            uptime_ms, rms_dbfs,
            bin_low_hz, bin_res_hz,
            bins_json,
            gateway_id, message_id,
            schema_version, message_type,
            raw_payload
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        device_id, timestamp, None, rms_dbfs,
        bin_low, bin_res, json.dumps(bins),
        'GW-01', '', '1', 'sound', body[:4000]
    )
    logger.info("sound inserted: dev=%s rms=%.2f bins=%d", device_id, rms_dbfs, len(bins))


def _handle_current(cursor, msg: dict, body: str):
    device_id  = msg.get("deviceId", "unknown")
    timestamp  = _sensor_timestamp(msg)
    cur        = msg.get("current", {}) or {}
    current_mA = _to_float(cur.get("current_mA"), 0.0)
    voltage_mV = _to_int(cur.get("voltage_mV"),   0)

    cursor.execute(
        """
        INSERT INTO telemetry_current (
            device_id, timestamp,
            current_mA, voltage_mV,
            gateway_id, raw_payload
        ) VALUES (?, ?, ?, ?, ?, ?)
        """,
        device_id, timestamp,
        current_mA, voltage_mV,
        'GW-01', body
    )
    logger.info("current inserted: dev=%s ts=%s mA=%.3f mV=%d",
                device_id, timestamp, current_mA, voltage_mV)


def _handle_legacy_telemetry(cursor, msg: dict, body: str):
    header  = msg.get("header",  {}) or {}
    payload = msg.get("payload", {}) or {}

    message_id = header.get("messageId", "")
    gateway_id = header.get("gatewayId", "GW-01")

    device_id  = payload.get("deviceId", "unknown")
    timestamp  = _parse_iso(payload.get("timestamp", ""))
    uptime_ms  = _to_int(payload.get("uptime_ms"), 0)
    proto_ver  = _to_int(payload.get("proto_ver"), 0)

    env = payload.get("environment", {}) or {}
    temperature_c    = _to_float(env.get("temperature_c"),    0.0)
    humidity_percent = _to_float(env.get("humidity_percent"), 0.0)
    pressure_hpa     = _to_float(env.get("pressure_hpa"),     0.0)
    eco2_ppm         = _to_int(env.get("eco2_ppm"),           0)
    tvoc_ppb         = _to_int(env.get("tvoc_ppb"),           0)
    aqi              = _to_int(env.get("aqi"),                0)
    batt_mv          = _to_int(env.get("batt_mV"),            0)

    spec = payload.get("spectrum", {}) or {}
    as7343_405 = _to_int(spec.get("AS7343_405nm"),  0)
    as7343_425 = _to_int(spec.get("AS7343_425nm"),  0)
    as7343_450 = _to_int(spec.get("AS7343_450nm"),  0)
    as7343_475 = _to_int(spec.get("AS7343_475nm"),  0)
    as7343_515 = _to_int(spec.get("AS7343_515nm"),  0)
    as7343_550 = _to_int(spec.get("AS7343_550nm"),  0)
    as7343_555 = _to_int(spec.get("AS7343_555nm"),  0)
    as7343_600 = _to_int(spec.get("AS7343_600nm"),  0)
    as7343_640 = _to_int(spec.get("AS7343_640nm"),  0)
    as7343_690 = _to_int(spec.get("AS7343_690nm"),  0)
    as7343_745 = _to_int(spec.get("AS7343_745nm"),  0)
    as7343_855 = _to_int(spec.get("AS7343_855nm"),  0)
    as7343_vis = _to_int(spec.get("AS7343_VISIBLE"), 0)

    snd = payload.get("sound", {}) or {}
    snd_rms_dbfs  = _to_float(snd.get("rms_dbfs"),   0.0)
    snd_peak_freq = _to_int(snd.get("peak_freq_hz"),  0)
    snd_peak_mag  = _to_float(snd.get("peak_mag"),    0.0)

    soil = payload.get("soil", {}) or {}
    soil_vwc_percent = _to_float(soil.get("vwc_percent"), 0.0)

    cursor.execute(
        """
        INSERT INTO telemetry (
            device_id, timestamp, uptime_ms, proto_ver,
            temperature_c, humidity_percent, pressure_hpa,
            eco2_ppm, tvoc_ppb, aqi, batt_mv,
            as7343_405nm, as7343_425nm, as7343_450nm, as7343_475nm,
            as7343_515nm, as7343_550nm, as7343_555nm, as7343_600nm,
            as7343_640nm, as7343_690nm, as7343_745nm, as7343_855nm,
            as7343_visible,
            snd_rms_dbfs, snd_peak_freq_hz, snd_peak_mag,
            soil_vwc_percent,
            gateway_id, message_id, raw_payload
        ) VALUES (
            ?, ?, ?, ?,
            ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?,
            ?, ?, ?,
            ?,
            ?, ?, ?
        )
        """,
        device_id, timestamp, uptime_ms, proto_ver,
        temperature_c, humidity_percent, pressure_hpa,
        eco2_ppm, tvoc_ppb, aqi, batt_mv,
        as7343_405, as7343_425, as7343_450, as7343_475,
        as7343_515, as7343_550, as7343_555, as7343_600,
        as7343_640, as7343_690, as7343_745, as7343_855,
        as7343_vis,
        snd_rms_dbfs, snd_peak_freq, snd_peak_mag,
        soil_vwc_percent,
        gateway_id, message_id, body
    )
    logger.info("legacy telemetry inserted: dev=%s ts=%s", device_id, timestamp)


# ── Main ──────────────────────────────────────────────────────────────────────

def main(events) -> None:
    logger.warning("TRIGGER FIRED: received batch of %d events", len(events))

    try:
        conn   = _connect()
        cursor = conn.cursor()
    except Exception as e:
        logger.error("Failed to connect to SQL: %s", e, exc_info=True)
        return

    counts = {"environment": 0, "spectrum": 0, "soil": 0,
              "battery": 0, "sound": 0, "current": 0, "legacy": 0, "skipped": 0}

    for event in events:
        try:
            body = event.get_body().decode("utf-8", errors="ignore")
            logger.info("Message body prefix: %s", body[:200])

            parsed = json.loads(body)

            # Handle both batched array and single object
            messages = parsed if isinstance(parsed, list) else [parsed]

            for msg in messages:
                msg_type = _detect_type(msg)

                if msg_type == "environment":
                    _handle_environment(cursor, msg, body)
                    counts["environment"] += 1

                elif msg_type == "spectrum":
                    _handle_spectrum(cursor, msg, body)
                    counts["spectrum"] += 1

                elif msg_type == "soil":
                    _handle_soil(cursor, msg, body)
                    counts["soil"] += 1

                elif msg_type == "battery":
                    _handle_battery(cursor, msg, body)
                    counts["battery"] += 1

                elif msg_type in ("sound", "sound_spectrum"):
                    _handle_sound(cursor, msg, body)
                    counts["sound"] += 1

                elif msg_type == "current":
                    _handle_current(cursor, msg, body)
                    counts["current"] += 1

                elif msg_type == "telemetry":
                    _handle_legacy_telemetry(cursor, msg, body)
                    counts["legacy"] += 1

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
        "Batch complete — env=%d spec=%d soil=%d bat=%d snd=%d cur=%d legacy=%d skipped=%d",
        counts["environment"], counts["spectrum"], counts["soil"],
        counts["battery"], counts["sound"], counts["current"], counts["legacy"], counts["skipped"]
    )