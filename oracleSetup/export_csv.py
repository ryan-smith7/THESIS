"""
Azure SQL CSV Exporter (pyodbc version)
=======================================
Exports all telemetry tables to CSV using pyodbc.
No extra installs needed — pyodbc already configured on VM.

Usage:
    python3 export_csvs.py

Outputs:
    ./deployment_csvs/bme280.csv
    ./deployment_csvs/ens160.csv
    ./deployment_csvs/spectrum.csv
    ./deployment_csvs/soil.csv
    ./deployment_csvs/soiltemp.csv
    ./deployment_csvs/sound.csv
    ./deployment_csvs/sound_bins.csv
"""

import os
import pyodbc
import pandas as pd

# ── MACROS — edit these ───────────────────────────────────────────────────────

SERVER   = 'iot-telemetry-server-ryan-smith.database.windows.net'
DATABASE = 'iot-telemetry-db'
USER     = 'sqladmin'
PASSWORD = ''   # ← fill in

DATE_START = '2026-05-21 00:00:00'
DATE_END   = '2026-05-27 00:00:00'

OUTPUT_DIR = './deployment_csvs'

# ── Queries ───────────────────────────────────────────────────────────────────

EXPORTS = {
    'bme280': """
        SELECT timestamp, temperature_c, humidity_percent, pressure_hpa
        FROM telemetry_bme280
        WHERE timestamp >= ? AND timestamp < ?
        ORDER BY timestamp ASC
    """,

    'ens160': """
        SELECT timestamp, eco2_ppm, tvoc_ppb, aqi
        FROM telemetry_ens160
        WHERE timestamp >= ? AND timestamp < ?
        ORDER BY timestamp ASC
    """,

    'spectrum': """
        SELECT timestamp, AS7343_405nm, AS7343_425nm, AS7343_450nm, AS7343_475nm,
               AS7343_515nm, AS7343_550nm, AS7343_555nm, AS7343_600nm,
               AS7343_640nm, AS7343_690nm, AS7343_745nm, AS7343_855nm, AS7343_visible
        FROM telemetry_spectrum
        WHERE timestamp >= ? AND timestamp < ?
        ORDER BY timestamp ASC
    """,

    'soil': """
        SELECT timestamp, soil_vwc_percent
        FROM telemetry_soil
        WHERE timestamp >= ? AND timestamp < ?
        ORDER BY timestamp ASC
    """,

    'soiltemp': """
        SELECT timestamp, temperature_c AS substrate_temp_c
        FROM telemetry_soil_temperature
        WHERE timestamp >= ? AND timestamp < ?
        ORDER BY timestamp ASC
    """,

    'sound': """
        SELECT received_time, rms_dbfs
        FROM telemetry_sound
        WHERE received_time >= ? AND received_time < ?
        ORDER BY received_time ASC
    """,

    'sound_bins': """
        SELECT received_time, rms_dbfs, bins_json
        FROM telemetry_sound
        WHERE received_time >= ? AND received_time < ?
        ORDER BY received_time ASC
    """,
}

# ── Connection ────────────────────────────────────────────────────────────────

def get_conn():
    cs = (
        f"Driver={{ODBC Driver 18 for SQL Server}};"
        f"Server={SERVER};Database={DATABASE};"
        f"Uid={USER};Pwd={PASSWORD};"
        f"Encrypt=yes;TrustServerCertificate=no;"
    )
    return pyodbc.connect(cs)

# ── Export runner ─────────────────────────────────────────────────────────────

def run_export(conn, name, query):
    out_path = os.path.join(OUTPUT_DIR, f'{name}.csv')
    print(f"\n[{name}] Querying...", flush=True)
    try:
        df = pd.read_sql(query, conn, params=(DATE_START, DATE_END))
        print(f"[{name}] {len(df):,} rows received — writing CSV...", flush=True)
        df.to_csv(out_path, index=False)
        size_mb = os.path.getsize(out_path) / 1024 / 1024
        print(f"[{name}] Done — {size_mb:.2f} MB written to {out_path}", flush=True)
    except Exception as e:
        print(f"[{name}] ERROR — {e}", flush=True)

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print(f"Deployment window: {DATE_START}  →  {DATE_END}")
    print(f"Output directory:  {OUTPUT_DIR}")

    print("\nConnecting to Azure SQL...", flush=True)
    conn = get_conn()
    print("Connected.", flush=True)

    for name, query in EXPORTS.items():
        run_export(conn, name, query)

    conn.close()

    print("\n=== Export complete ===")
    print(f"Files in {OUTPUT_DIR}:")
    for f in sorted(os.listdir(OUTPUT_DIR)):
        path = os.path.join(OUTPUT_DIR, f)
        print(f"  {f:30s}  {os.path.getsize(path)/1024/1024:.2f} MB")

if __name__ == '__main__':
    main()