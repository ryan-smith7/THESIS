from flask import Flask, request, jsonify
from flask_cors import CORS
import paho.mqtt.client as mqtt
import pyodbc
import json
import logging

logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
logger = logging.getLogger(__name__)

app = Flask(__name__)
CORS(app)

# ── SQL connection ────────────────────────────────────────────────────────────
# Edit these to match your Azure SQL credentials
SQL_SERVER   = 'iot-telemetry-server-ryan-smith.database.windows.net'
SQL_DATABASE = 'iot-telemetry-db'
SQL_USER     = 'sqladmin'
SQL_PASSWORD = 'cEgjuz-jetsod-5xybpa'  # ← fill in

SQL_CONN_STR = (
    f"Driver={{ODBC Driver 18 for SQL Server}};"
    f"Server={SQL_SERVER};"
    f"Database={SQL_DATABASE};"
    f"Uid={SQL_USER};"
    f"Pwd={SQL_PASSWORD};"
    f"Encrypt=yes;TrustServerCertificate=no;"
)

_conn = None

def get_conn():
    global _conn
    if _conn is None:
        logger.info("SQL: opening new connection")
        _conn = pyodbc.connect(SQL_CONN_STR)
    else:
        try:
            _conn.cursor().execute("SELECT 1")
        except Exception:
            logger.warning("SQL: connection lost, reconnecting")
            _conn = pyodbc.connect(SQL_CONN_STR)
    return _conn

# ── MQTT upload (live telemetry path — unchanged) ─────────────────────────────
@app.route('/upload', methods=['POST'])
def upload():
    data = request.get_json()
    messages = data if isinstance(data, list) else [data]
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.connect("localhost", 1883)
    for msg in messages:
        client.publish("devices/upload", json.dumps(msg), qos=1)
    client.disconnect()
    return jsonify({"ok": len(messages)})

# ── Direct SQL upload ─────────────────────────────────────────────────────────
@app.route('/upload_csv', methods=['POST'])
def upload_csv():
    data     = request.get_json()
    modality = data.get('modality')
    rows     = data.get('rows', [])

    if not rows:
        return jsonify({'ok': 0})

    logger.info(f"upload_csv: modality={modality} batch={len(rows)} rows")

    try:
        conn   = get_conn()
        cursor = conn.cursor()

        if   modality == 'bme280':   inserted = _insert_bme280(cursor, rows)
        elif modality == 'ens160':   inserted = _insert_ens160(cursor, rows)
        elif modality == 'as7343':   inserted = _insert_spectrum(cursor, rows)
        elif modality == 'moisture': inserted = _insert_soil(cursor, rows)
        elif modality == 'ds18b20':  inserted = _insert_soil_temp(cursor, rows)
        elif modality == 'sound':    inserted = _insert_sound(cursor, rows)
        elif modality == 'battery':  inserted = _insert_battery(cursor, rows)
        else:
            logger.warning(f"upload_csv: unknown modality '{modality}'")
            return jsonify({'error': 'unknown modality: ' + str(modality)}), 400

        conn.commit()
        logger.info(f"upload_csv: {modality} committed {inserted} rows OK")
        return jsonify({'ok': inserted})

    except Exception as e:
        logger.error(f"upload_csv: {modality} FAILED — {e}")
        # Reset connection so next request gets a fresh one
        global _conn
        _conn = None
        return jsonify({'error': str(e)}), 500


# ── Modality inserters (all use MERGE for deduplication) ──────────────────────

def _insert_bme280(cursor, rows):
    for r in rows:
        cursor.execute("""
            MERGE telemetry_bme280 AS target
            USING (VALUES (?, ?, ?, ?, ?, ?, ?))
                AS source (device_id, timestamp, temperature_c, humidity_percent, pressure_hpa, gateway_id, raw_payload)
            ON  target.device_id = source.device_id
            AND target.timestamp = source.timestamp
            WHEN NOT MATCHED THEN
                INSERT (device_id, timestamp, temperature_c, humidity_percent, pressure_hpa, gateway_id, raw_payload)
                VALUES (source.device_id, source.timestamp, source.temperature_c, source.humidity_percent, source.pressure_hpa, source.gateway_id, source.raw_payload);
        """, r['device_id'], r['datetime'], r['temp_c'], r['rh_pct'], r['press_hPa'], 'CSV', None)
    logger.info(f"bme280: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


def _insert_ens160(cursor, rows):
    for r in rows:
        cursor.execute("""
            MERGE telemetry_ens160 AS target
            USING (VALUES (?, ?, ?, ?, ?, ?, ?))
                AS source (device_id, timestamp, eco2_ppm, tvoc_ppb, aqi, gateway_id, raw_payload)
            ON  target.device_id = source.device_id
            AND target.timestamp = source.timestamp
            WHEN NOT MATCHED THEN
                INSERT (device_id, timestamp, eco2_ppm, tvoc_ppb, aqi, gateway_id, raw_payload)
                VALUES (source.device_id, source.timestamp, source.eco2_ppm, source.tvoc_ppb, source.aqi, source.gateway_id, source.raw_payload);
        """, r['device_id'], r['datetime'], r['eco2_ppm'], r['tvoc_ppb'], r['aqi'], 'CSV', None)
    logger.info(f"ens160: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


def _insert_spectrum(cursor, rows):
    for r in rows:
        cursor.execute("""
            MERGE telemetry_spectrum AS target
            USING (VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))
                AS source (device_id, timestamp,
                           AS7343_405nm, AS7343_425nm, AS7343_450nm, AS7343_475nm,
                           AS7343_515nm, AS7343_550nm, AS7343_555nm, AS7343_600nm,
                           AS7343_640nm, AS7343_690nm, AS7343_745nm, AS7343_855nm,
                           AS7343_visible, gateway_id, raw_payload)
            ON  target.device_id = source.device_id
            AND target.timestamp = source.timestamp
            WHEN NOT MATCHED THEN
                INSERT (device_id, timestamp,
                        AS7343_405nm, AS7343_425nm, AS7343_450nm, AS7343_475nm,
                        AS7343_515nm, AS7343_550nm, AS7343_555nm, AS7343_600nm,
                        AS7343_640nm, AS7343_690nm, AS7343_745nm, AS7343_855nm,
                        AS7343_visible, gateway_id, raw_payload)
                VALUES (source.device_id, source.timestamp,
                        source.AS7343_405nm, source.AS7343_425nm, source.AS7343_450nm, source.AS7343_475nm,
                        source.AS7343_515nm, source.AS7343_550nm, source.AS7343_555nm, source.AS7343_600nm,
                        source.AS7343_640nm, source.AS7343_690nm, source.AS7343_745nm, source.AS7343_855nm,
                        source.AS7343_visible, source.gateway_id, source.raw_payload);
        """, r['device_id'], r['datetime'],
             r['F1_405nm_mWm2'], r['F2_425nm_mWm2'], r['FZ_450nm_mWm2'], r['F3_475nm_mWm2'],
             r['F4_515nm_mWm2'], r['F5_550nm_mWm2'], r['FY_555nm_mWm2'], r['FXL_600nm_mWm2'],
             r['F6_640nm_mWm2'], r['F7_690nm_mWm2'], r['F8_745nm_mWm2'], r['NIR_855nm_mWm2'],
             r['VIS_broadband_mWm2'], 'CSV', None)
    logger.info(f"spectrum: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


def _insert_soil(cursor, rows):
    for r in rows:
        cursor.execute("""
            MERGE telemetry_soil AS target
            USING (VALUES (?, ?, ?, ?, ?))
                AS source (device_id, timestamp, soil_vwc_percent, gateway_id, raw_payload)
            ON  target.device_id = source.device_id
            AND target.timestamp = source.timestamp
            WHEN NOT MATCHED THEN
                INSERT (device_id, timestamp, soil_vwc_percent, gateway_id, raw_payload)
                VALUES (source.device_id, source.timestamp, source.soil_vwc_percent, source.gateway_id, source.raw_payload);
        """, r['device_id'], r['datetime'], r['vwc_percent'], 'CSV', None)
    logger.info(f"soil: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


def _insert_soil_temp(cursor, rows):
    for r in rows:
        cursor.execute("""
            MERGE telemetry_soil_temperature AS target
            USING (VALUES (?, ?, ?, ?, ?))
                AS source (device_id, timestamp, temperature_c, gateway_id, raw_payload)
            ON  target.device_id = source.device_id
            AND target.timestamp = source.timestamp
            WHEN NOT MATCHED THEN
                INSERT (device_id, timestamp, temperature_c, gateway_id, raw_payload)
                VALUES (source.device_id, source.timestamp, source.temperature_c, source.gateway_id, source.raw_payload);
        """, r['device_id'], r['datetime'], r['temp_c'], 'CSV', None)
    logger.info(f"soil_temp: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


def _insert_sound(cursor, rows):
    for r in rows:
        bins = r.get('bins', [])
        cursor.execute("""
            MERGE telemetry_sound AS target
            USING (VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))
                AS source (device_id, received_time, uptime_ms, rms_dbfs,
                           bin_low_hz, bin_res_hz, bins_json,
                           gateway_id, message_id, schema_version, message_type, raw_payload)
            ON  target.device_id     = source.device_id
            AND target.received_time = source.received_time
            WHEN NOT MATCHED THEN
                INSERT (device_id, received_time, uptime_ms, rms_dbfs,
                        bin_low_hz, bin_res_hz, bins_json,
                        gateway_id, message_id, schema_version, message_type, raw_payload)
                VALUES (source.device_id, source.received_time, source.uptime_ms, source.rms_dbfs,
                        source.bin_low_hz, source.bin_res_hz, source.bins_json,
                        source.gateway_id, source.message_id, source.schema_version,
                        source.message_type, source.raw_payload);
        """, r['device_id'], r['datetime'], r['uptime_ms'], r['rms_dbfs'],
             43, 43, json.dumps(bins),
             'CSV', '', '1', 'sound', None)
    logger.info(f"sound: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


def _insert_battery(cursor, rows):
    for r in rows:
        cursor.execute("""
            MERGE telemetry_battery AS target
            USING (VALUES (?, ?, ?, ?, ?, ?, ?))
                AS source (device_id, timestamp, batt_mv, batt_pct, rate_pct_hr, gateway_id, raw_payload)
            ON  target.device_id = source.device_id
            AND target.timestamp = source.timestamp
            WHEN NOT MATCHED THEN
                INSERT (device_id, timestamp, batt_mv, batt_pct, rate_pct_hr, gateway_id, raw_payload)
                VALUES (source.device_id, source.timestamp, source.batt_mv, source.batt_pct,
                        source.rate_pct_hr, source.gateway_id, source.raw_payload);
        """, r['device_id'], r['datetime'], r['batt_mv'], r['batt_pct'], r['rate_pct_hr'], 'CSV', None)
    logger.info(f"battery: merged {len(rows)} rows | first={rows[0]['datetime']} last={rows[-1]['datetime']}")
    return len(rows)


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8080)