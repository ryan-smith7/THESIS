import logging
import json
import os
import pyodbc
from datetime import datetime, timezone

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)


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


def main(events) -> None:
    logger.warning("TRIGGER FIRED: received batch")

    server   = os.environ.get("SQL_SERVER",   "iot-telemetry-server-ryan-smith.database.windows.net")
    database = os.environ.get("SQL_DATABASE", "iot-telemetry-db")
    password = os.environ.get("SQL_PASSWORD")

    if not password:
        logger.error("Missing SQL_PASSWORD environment variable")
        return

    logger.warning("Connecting to server=%s database=%s user=sqladmin", server, database)

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

    try:
        conn   = pyodbc.connect(conn_str)
        cursor = conn.cursor()
    except Exception as e:
        logger.error("Failed to connect to SQL: %s", e, exc_info=True)
        return

    inserted_sensor = 0
    inserted_sound  = 0

    for event in events:
        try:
            body = event.get_body().decode("utf-8", errors="ignore")
            logger.info("Message body prefix: %s", body[:200])

            msg = json.loads(body)

            header  = msg.get("header",  {}) or {}
            payload = msg.get("payload", {}) or {}

            message_type = header.get("messageType", "telemetry")
            message_id   = header.get("messageId",   "")
            gateway_id   = header.get("gatewayId",   "")

            # ── SOUND SPECTRUM ───────────────────────────────────────────
            if message_type == "sound_spectrum":

                device_id  = payload.get("deviceId",   "unknown")
                uptime_ms  = _to_int(payload.get("uptime_ms"),  0)
                rms_dbfs   = _to_float(payload.get("rms_dbfs"), 0.0)
                bin_low_hz = _to_int(payload.get("bin_low_hz"), 0)
                bin_res_hz = _to_int(payload.get("bin_res_hz"), 0)
                bins       = payload.get("bins", []) or []

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
                    device_id,
                    datetime.utcnow(),
                    uptime_ms,
                    rms_dbfs,
                    bin_low_hz,
                    bin_res_hz,
                    json.dumps(bins),          # ~2500 chars — needs NVARCHAR(MAX)
                    gateway_id,
                    message_id,
                    header.get("schemaVersion", ""),
                    message_type,
                    body[:4000]                # raw capped; full data is in bins_json
                )
                inserted_sound += 1
                logger.info("Sound inserted: dev=%s rms=%.2f bins=%d",
                            device_id, rms_dbfs, len(bins))
                continue

            # ── SENSOR TELEMETRY ─────────────────────────────────────────
            if message_type not in ("telemetry", ""):
                logger.warning("Unknown messageType '%s' — skipping", message_type)
                continue

            device_id  = payload.get("deviceId", "unknown")
            timestamp  = _parse_iso(payload.get("timestamp", ""))
            uptime_ms  = _to_int(payload.get("uptime_ms"), 0)
            proto_ver  = _to_int(payload.get("proto_ver"), 0)

            env = payload.get("environment", {}) or {}
            temperature_c    = _to_float(env.get("temperature_c"),    0.0)
            humidity_percent = _to_float(env.get("humidity_percent"), 0.0)
            pressure_hpa     = _to_float(env.get("pressure_hpa"),     0.0)
            eco2_ppm         = _to_int(env.get("eco2_ppm"), 0)
            tvoc_ppb         = _to_int(env.get("tvoc_ppb"), 0)
            aqi              = _to_int(env.get("aqi"),      0)
            batt_mv          = _to_int(env.get("batt_mV"),  0)

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

            # Sound summary (new fields in updated 57-byte sensor packet)
            snd = payload.get("sound", {}) or {}
            snd_rms_dbfs   = _to_float(snd.get("rms_dbfs"),    0.0)
            snd_peak_freq  = _to_int(snd.get("peak_freq_hz"),  0)
            snd_peak_mag   = _to_float(snd.get("peak_mag"),    0.0)
            
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
            inserted_sensor += 1
            logger.info("Sensor inserted: dev=%s ts=%s msg=%s",
                        device_id, timestamp, message_id)

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

    logger.warning("Batch complete. Sensor=%d  Sound=%d",
                   inserted_sensor, inserted_sound)