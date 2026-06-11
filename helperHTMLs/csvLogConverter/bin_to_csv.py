#!/usr/bin/env python3
"""
Sensor Log Converter — Python edition
Converts .bin files produced by the mycorrhizal IoT nodes to .csv

Supported modalities (auto-detected from filename):
  bme280.bin / BMEnnnn.BIN  — temperature, humidity, pressure
  ens160.bin / ENSnnnn.BIN  — eCO2, TVOC, AQI
  as7343.bin / AS7nnnn.BIN  — 13-channel spectral irradiance
  moisture.bin / MSTnnnn.BIN — volumetric water content
  sound.bin  / SNDnnnn.BIN  — RMS + 348 FFT bins
  ds18b20.bin / D18nnnn.BIN — soil temperature

Usage:
  python3 bin_to_csv.py file1.bin file2.bin ...
  python3 bin_to_csv.py *.bin
"""

import struct
import sys
import os
import csv
import zlib
from datetime import datetime, timezone

UTC_MIN        = 1_700_000_000
AS7_NUM_CH     = 13
SOUND_NUM_BINS = 348


# ── CRC32 ─────────────────────────────────────────────────────────────────
def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def verify_crc(buf: bytes, offset: int, struct_len: int) -> bool:
    stored = struct.unpack_from('<I', buf, offset + struct_len)[0]
    computed = crc32(buf[offset:offset + struct_len])
    return stored == computed


# ── Timestamp formatting ──────────────────────────────────────────────────
def fmt_dt(s: int, ms: int) -> str:
    if s > UTC_MIN:
        dt = datetime.fromtimestamp(s + ms / 1000, tz=timezone.utc)
        return dt.strftime('%Y-%m-%d %H:%M:%S.') + f'{ms:03d}'
    if s > 0:
        return f'unsynced({s}.{ms:03d})'
    return 'unsynced'


# ── uint64 helper ─────────────────────────────────────────────────────────
def get_uint64(buf: bytes, offset: int) -> int:
    lo, hi = struct.unpack_from('<II', buf, offset)
    return hi * 0x1_0000_0000 + lo


# ── UTC backfill ──────────────────────────────────────────────────────────
def backfill_utc(rows: list) -> list:
    anchor = next((r for r in rows if r['s'] > UTC_MIN), None)
    if not anchor:
        return rows
    anchor_utc_ms = anchor['s'] * 1000 + anchor['ms']
    result = []
    for r in rows:
        if r['s'] > UTC_MIN or r['uptime_ms'] == 0:
            result.append(r)
            continue
        rec_utc_ms = anchor_utc_ms - (anchor['uptime_ms'] - r['uptime_ms'])
        if rec_utc_ms < UTC_MIN * 1000:
            result.append(r)
            continue
        r = dict(r)
        r['s']  = rec_utc_ms // 1000
        r['ms'] = round(rec_utc_ms % 1000)
        result.append(r)
    return result


# ── Parsers ───────────────────────────────────────────────────────────────
def parse_bme280(buf: bytes):
    rec_sz, struct_sz = 44, 40
    rows, corrupt = [], 0
    for i in range(len(buf) // rec_sz):
        o = i * rec_sz
        if not verify_crc(buf, o, struct_sz):
            corrupt += 1; continue
        temp, rh, press = struct.unpack_from('<ddd', buf, o)
        s,  = struct.unpack_from('<I', buf, o + 24)
        ms, = struct.unpack_from('<H', buf, o + 28)
        uptime_ms = get_uint64(buf, o + 32)
        rows.append({'s': s, 'ms': ms, 'uptime_ms': uptime_ms,
                     'temp': temp, 'rh': rh, 'press': press})
    if corrupt:
        print(f'  bme280: {corrupt} corrupt record(s) skipped')
    return rows

def csv_bme280(rows):
    header = ['datetime','utc_sec','utc_ms','uptime_ms','temp_c','rh_pct','press_hPa']
    data = [[fmt_dt(r['s'],r['ms']), r['s'], r['ms'], r['uptime_ms'],
             round(r['temp'],2), round(r['rh'],2), round(r['press']*10,3)] for r in rows]
    return header, data


def parse_ens160(buf: bytes):
    rec_sz, struct_sz = 36, 32
    rows, corrupt = [], 0
    for i in range(len(buf) // rec_sz):
        o = i * rec_sz
        if not verify_crc(buf, o, struct_sz):
            corrupt += 1; continue
        eco2, tvoc, aqi = struct.unpack_from('<iii', buf, o)
        s,  = struct.unpack_from('<I', buf, o + 12)
        ms, = struct.unpack_from('<H', buf, o + 16)
        uptime_ms = get_uint64(buf, o + 24)
        rows.append({'s': s, 'ms': ms, 'uptime_ms': uptime_ms,
                     'eco2': eco2, 'tvoc': tvoc, 'aqi': aqi})
    if corrupt:
        print(f'  ens160: {corrupt} corrupt record(s) skipped')
    return rows

def csv_ens160(rows):
    header = ['datetime','utc_sec','utc_ms','uptime_ms','eco2_ppm','tvoc_ppb','aqi']
    data = [[fmt_dt(r['s'],r['ms']), r['s'], r['ms'], r['uptime_ms'],
             r['eco2'], r['tvoc'], r['aqi']] for r in rows]
    return header, data


def parse_as7343(buf: bytes):
    rec_sz, struct_sz = 76, 72
    rows, corrupt = [], 0
    for i in range(len(buf) // rec_sz):
        o = i * rec_sz
        if not verify_crc(buf, o, struct_sz):
            corrupt += 1; continue
        ch = list(struct.unpack_from('<' + 'I'*AS7_NUM_CH, buf, o))
        ms, = struct.unpack_from('<H', buf, o + 52)
        s,  = struct.unpack_from('<I', buf, o + 56)
        uptime_ms = get_uint64(buf, o + 64)
        rows.append({'s': s, 'ms': ms, 'uptime_ms': uptime_ms, 'ch': ch})
    if corrupt:
        print(f'  as7343: {corrupt} corrupt record(s) skipped')
    return rows

def csv_as7343(rows):
    wl = ['F1_405nm_mWm2','F2_425nm_mWm2','FZ_450nm_mWm2','F3_475nm_mWm2',
          'F4_515nm_mWm2','F5_550nm_mWm2','FY_555nm_mWm2','FXL_600nm_mWm2',
          'F6_640nm_mWm2','F7_690nm_mWm2','F8_745nm_mWm2','NIR_855nm_mWm2','VIS_broadband_mWm2']
    header = ['datetime','utc_sec','utc_ms','uptime_ms'] + wl
    data = [[fmt_dt(r['s'],r['ms']), r['s'], r['ms'], r['uptime_ms'],
             *[round(v/1000, 3) for v in r['ch']]] for r in rows]
    return header, data


def parse_moisture(buf: bytes):
    rec_sz, struct_sz = 20, 16
    rows, corrupt = [], 0
    for i in range(len(buf) // rec_sz):
        o = i * rec_sz
        if not verify_crc(buf, o, struct_sz):
            corrupt += 1; continue
        vwc_raw, ms = struct.unpack_from('<HH', buf, o)
        s, = struct.unpack_from('<I', buf, o + 4)
        uptime_ms = get_uint64(buf, o + 8)
        rows.append({'s': s, 'ms': ms, 'uptime_ms': uptime_ms, 'vwc': vwc_raw / 100})
    if corrupt:
        print(f'  moisture: {corrupt} corrupt record(s) skipped')
    return rows

def csv_moisture(rows):
    header = ['datetime','utc_sec','utc_ms','uptime_ms','vwc_percent']
    data = [[fmt_dt(r['s'],r['ms']), r['s'], r['ms'], r['uptime_ms'],
             round(r['vwc'],2)] for r in rows]
    return header, data


def parse_sound(buf: bytes):
    has_uptime = (len(buf) % 716 == 0)
    rec_sz   = 716 if has_uptime else 708
    struct_sz = 712 if has_uptime else 704
    rows, corrupt = [], 0
    for i in range(len(buf) // rec_sz):
        o = i * rec_sz
        if not verify_crc(buf, o, struct_sz):
            corrupt += 1; continue
        s,  = struct.unpack_from('<I', buf, o)
        ms, = struct.unpack_from('<H', buf, o + 4)
        rms_raw, = struct.unpack_from('<h', buf, o + 6)
        rms = rms_raw / 100 + 120
        uptime_ms = get_uint64(buf, o + 8) if has_uptime else 0
        bin_off = o + 16 if has_uptime else o + 8
        bins = list(struct.unpack_from('<' + 'H'*SOUND_NUM_BINS, buf, bin_off))
        rows.append({'s': s, 'ms': ms, 'uptime_ms': uptime_ms, 'rms': rms, 'bins': bins})
    if corrupt:
        print(f'  sound: {corrupt} corrupt record(s) skipped')
    if not has_uptime:
        print('  sound: old layout — uptime will be 0')
    return rows

def csv_sound(rows):
    bin_headers = [f'bin{i+1}_{round((i+1)*43.1)}hz_dbspl' for i in range(SOUND_NUM_BINS)]
    header = ['datetime','utc_sec','utc_ms','uptime_ms','rms_dbfs'] + bin_headers
    data = [[fmt_dt(r['s'],r['ms']), r['s'], r['ms'], r['uptime_ms'],
             round(r['rms'],2), *[round(v/100,2) for v in r['bins']]] for r in rows]
    return header, data


def parse_ds18b20(buf: bytes):
    rec_sz, struct_sz = 28, 24
    rows, corrupt = [], 0
    for i in range(len(buf) // rec_sz):
        o = i * rec_sz
        if not verify_crc(buf, o, struct_sz):
            corrupt += 1; continue
        val1, val2 = struct.unpack_from('<ii', buf, o)
        s,  = struct.unpack_from('<I', buf, o + 8)
        ms, = struct.unpack_from('<H', buf, o + 12)
        uptime_ms = get_uint64(buf, o + 16)
        rows.append({'s': s, 'ms': ms, 'uptime_ms': uptime_ms,
                     'temp_c': val1 + val2 / 1_000_000})
    if corrupt:
        print(f'  ds18b20: {corrupt} corrupt record(s) skipped')
    return rows

def csv_ds18b20(rows):
    header = ['datetime','utc_sec','utc_ms','uptime_ms','temp_c']
    data = [[fmt_dt(r['s'],r['ms']), r['s'], r['ms'], r['uptime_ms'],
             round(r['temp_c'],4)] for r in rows]
    return header, data


# ── Modality registry ─────────────────────────────────────────────────────
import re

MODALITIES = {
    'bme':     {'parse': parse_bme280,  'csv': csv_bme280,
                'utc_name': 'bme280.bin',  'boot_pat': re.compile(r'^bme\d{4}\.bin$', re.I)},
    'ens':     {'parse': parse_ens160,  'csv': csv_ens160,
                'utc_name': 'ens160.bin',  'boot_pat': re.compile(r'^ens\d{4}\.bin$', re.I)},
    'as7':     {'parse': parse_as7343,  'csv': csv_as7343,
                'utc_name': 'as7343.bin',  'boot_pat': re.compile(r'^as7\d{4}\.bin$', re.I)},
    'mst':     {'parse': parse_moisture,'csv': csv_moisture,
                'utc_name': 'moisture.bin','boot_pat': re.compile(r'^mst\d{4}\.bin$', re.I)},
    'snd':     {'parse': parse_sound,   'csv': csv_sound,
                'utc_name': 'sound.bin',   'boot_pat': re.compile(r'^snd\d{4}\.bin$', re.I)},
    'ds18b20': {'parse': parse_ds18b20, 'csv': csv_ds18b20,
                'utc_name': 'ds18b20.bin', 'boot_pat': re.compile(r'^d18\d{4}\.bin$', re.I)},
}

def classify(filename: str):
    name = os.path.basename(filename).lower()
    for key, m in MODALITIES.items():
        if name == m['utc_name']:
            return key
        if m['boot_pat'].match(name):
            return key
    return None


# ── Main ──────────────────────────────────────────────────────────────────
def convert(path: str):
    name = os.path.basename(path)
    mod_key = classify(name)
    if mod_key is None:
        print(f'SKIP {name} — unrecognised filename')
        return

    m = MODALITIES[mod_key]
    with open(path, 'rb') as f:
        buf = f.read()

    print(f'Reading {name} ...')
    rows = m['parse'](buf)
    rows = backfill_utc(rows)
    print(f'  {len(rows)} records parsed')

    header, data = m['csv'](rows)

    out_path = path.replace('.bin', '.csv').replace('.BIN', '.csv')
    with open(out_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(data)

    print(f'  -> {os.path.basename(out_path)} ({len(data)} rows)')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: python3 bin_to_csv.py file1.bin file2.bin ...')
        sys.exit(1)
    for path in sys.argv[1:]:
        convert(path)
