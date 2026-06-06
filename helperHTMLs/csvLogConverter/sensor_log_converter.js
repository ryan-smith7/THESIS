// ════════════════════════════════════════════════════════════════
// RELAY HOST — Oracle VM IP, relay on :8080
// ════════════════════════════════════════════════════════════════
const RELAY_HOST = '161.33.232.177';
// ════════════════════════════════════════════════════════════════

const UTC_MIN        = 1700000000;
const AS7_NUM_CH     = 13;
const SOUND_NUM_BINS = 348;
const BATCH_SIZE     = 1000;

// ── CRC32 ─────────────────────────────────────────────────────────────────
const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c;
  }
  return t;
})();

function crc32(buf, offset, len) {
  let crc = 0xFFFFFFFF;
  const u8 = new Uint8Array(buf, offset, len);
  for (let i = 0; i < len; i++) crc = CRC32_TABLE[(crc ^ u8[i]) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function verifyCrc(buf, recOffset, structLen) {
  const stored = new DataView(buf).getUint32(recOffset + structLen, true);
  const computed = crc32(buf, recOffset, structLen);
  return stored === computed;
}

// ── Utilities ─────────────────────────────────────────────────────────────
function fmtDt(s, ms) {
  if (s > UTC_MIN) return new Date((s + ms / 1000) * 1000).toISOString().replace('T', ' ').replace('Z', '').slice(0, 23);
  if (s > 0) return 'unsynced(' + s + '.' + String(ms).padStart(3, '0') + ')';
  return 'unsynced';
}

function getUint64(dv, offset) {
  const lo = dv.getUint32(offset, true);
  const hi = dv.getUint32(offset + 4, true);
  return hi * 0x100000000 + lo;
}

function applyOffset(rec, offsetSec) {
  if (rec.s > UTC_MIN) return rec;
  const totalMs = rec.uptime_ms + Math.round(offsetSec * 1000);
  return Object.assign({}, rec, { s: Math.floor(totalMs / 1000), ms: totalMs % 1000 });
}

// ── Back-calculate UTC for pre-sync records ───────────────────────────────
// Finds the first record with a valid UTC timestamp and uses uptime_ms
// differences to reconstruct UTC for any pre-sync records before it.
// Formula: record_utc_ms = anchor_utc_ms - (anchor_uptime_ms - record_uptime_ms)
function backfillUtc(rows) {
  const anchor = rows.find(r => r.s > UTC_MIN);
  if (!anchor) return rows; // no valid UTC in file at all — nothing to back-calculate
  const anchorUtcMs = anchor.s * 1000 + anchor.ms;
  return rows.map(r => {
    if (r.s > UTC_MIN) return r; // already has valid UTC
    if (r.uptime_ms === 0) return r; // no uptime data, can't calculate
    const recUtcMs = anchorUtcMs - (anchor.uptime_ms - r.uptime_ms);
    if (recUtcMs < UTC_MIN * 1000) return r; // result is nonsensical, leave as-is
    return Object.assign({}, r, {
      s:  Math.floor(recUtcMs / 1000),
      ms: Math.round(recUtcMs % 1000),
    });
  });
}

// ── Fault-tolerant CSV builder ────────────────────────────────────────────
// Builds CSV row-by-row into a Blob to avoid hitting the JS max string limit.
// Returns { blob: Blob, rowsWritten: number, error: string|null, failIndex: number|null }
function buildCsvSafe(header, rows, rowFn) {
  const CHUNK = 5000;
  let parts = [header + '\n'];
  for (let i = 0; i < rows.length; i++) {
    try {
      parts.push(rowFn(rows[i]) + '\n');
    } catch (e) {
      return { blob: new Blob(parts, { type: 'text/csv' }), rowsWritten: i, error: e.message, failIndex: i };
    }
    if (parts.length >= CHUNK) {
      parts = [new Blob(parts, { type: 'text/csv' })];
    }
  }
  return { blob: new Blob(parts, { type: 'text/csv' }), rowsWritten: rows.length, error: null, failIndex: null };
}

// ── Modality definitions ──────────────────────────────────────────────────
const MODALITIES = {
  bme: {
    label: 'bme280', cls: 'b0', structSz: 40, recSz: 44,
    utcName: 'bme280.bin', bootPattern: /^bme[0-9]{4}\.bin$/i,
    parse(b) {
      const d = new DataView(b), r = [], N = Math.floor(b.byteLength / 44); let corrupt = 0;
      for (let i = 0; i < N; i++) {
        const o = i * 44; if (!verifyCrc(b, o, 40)) { corrupt++; continue; }
        r.push({ temp: d.getFloat64(o, true), rh: d.getFloat64(o + 8, true), press: d.getFloat64(o + 16, true),
          s: d.getUint32(o + 24, true), ms: d.getUint16(o + 28, true), uptime_ms: getUint64(d, o + 32) });
      }
      if (corrupt) logLine('bme280: ' + corrupt + ' corrupt record(s) skipped', 'er'); return r;
    },
    csv(rows) {
      return buildCsvSafe(
        'datetime,utc_sec,utc_ms,uptime_ms,temp_c,rh_pct,press_hPa',
        rows,
        x => [fmtDt(x.s, x.ms), x.s, x.ms, x.uptime_ms, x.temp.toFixed(2), x.rh.toFixed(2), x.press.toFixed(3)].join(',')
      );
    },
    toSql(row, dev) { return { device_id: dev, datetime: fmtDt(row.s, row.ms), temp_c: parseFloat(row.temp.toFixed(2)), rh_pct: parseFloat(row.rh.toFixed(2)), press_hPa: parseFloat(row.press.toFixed(3)) }; },
    bin(rows) {
      const buf = new ArrayBuffer(rows.length * 44), d = new DataView(buf);
      rows.forEach((x, i) => { const o = i * 44; d.setFloat64(o, x.temp, true); d.setFloat64(o + 8, x.rh, true); d.setFloat64(o + 16, x.press, true); d.setUint32(o + 24, x.s, true); d.setUint16(o + 28, x.ms, true); d.setUint32(o + 32, x.uptime_ms & 0xFFFFFFFF, true); d.setUint32(o + 36, Math.floor(x.uptime_ms / 0x100000000), true); d.setUint32(o + 40, crc32(buf, o, 40), true); });
      return buf;
    }
  },

  ens: {
    label: 'ens160', cls: 'b1', structSz: 32, recSz: 36,
    utcName: 'ens160.bin', bootPattern: /^ens[0-9]{4}\.bin$/i,
    parse(b) {
      const d = new DataView(b), r = [], N = Math.floor(b.byteLength / 36); let corrupt = 0;
      for (let i = 0; i < N; i++) {
        const o = i * 36; if (!verifyCrc(b, o, 32)) { corrupt++; continue; }
        r.push({ eco2: d.getInt32(o, true), tvoc: d.getInt32(o + 4, true), aqi: d.getInt32(o + 8, true), s: d.getUint32(o + 12, true), ms: d.getUint16(o + 16, true), uptime_ms: getUint64(d, o + 24) });
      }
      if (corrupt) logLine('ens160: ' + corrupt + ' corrupt record(s) skipped', 'er'); return r;
    },
    csv(rows) { return buildCsvSafe('datetime,utc_sec,utc_ms,uptime_ms,eco2_ppm,tvoc_ppb,aqi', rows, x => [fmtDt(x.s, x.ms), x.s, x.ms, x.uptime_ms, x.eco2, x.tvoc, x.aqi].join(',')); },
    toSql(row, dev) { return { device_id: dev, datetime: fmtDt(row.s, row.ms), eco2_ppm: row.eco2, tvoc_ppb: row.tvoc, aqi: row.aqi }; },
    bin(rows) { const buf = new ArrayBuffer(rows.length * 36), d = new DataView(buf); rows.forEach((x, i) => { const o = i * 36; d.setInt32(o, x.eco2, true); d.setInt32(o + 4, x.tvoc, true); d.setInt32(o + 8, x.aqi, true); d.setUint32(o + 12, x.s, true); d.setUint16(o + 16, x.ms, true); d.setUint32(o + 24, x.uptime_ms & 0xFFFFFFFF, true); d.setUint32(o + 28, Math.floor(x.uptime_ms / 0x100000000), true); d.setUint32(o + 32, crc32(buf, o, 32), true); }); return buf; }
  },

  as7: {
    label: 'as7343', cls: 'b2', structSz: 72, recSz: 76,
    utcName: 'as7343.bin', bootPattern: /^as7[0-9]{4}\.bin$/i,
    // uint32[13] ch(0-51) | uint16 utc_ms(52) | uint16 _pad(54) | uint32 utc_sec(56) | uint32 _pad2(60) | uint64 uptime_ms(64)
    parse(b) {
      const d = new DataView(b), r = [], N = Math.floor(b.byteLength / 76); let corrupt = 0;
      for (let i = 0; i < N; i++) {
        const o = i * 76; if (!verifyCrc(b, o, 72)) { corrupt++; continue; }
        const ch = []; for (let c = 0; c < AS7_NUM_CH; c++) ch.push(d.getUint32(o + c * 4, true));
        r.push({ ch, ms: d.getUint16(o + 52, true), s: d.getUint32(o + 56, true), uptime_ms: getUint64(d, o + 64) });
      }
      if (corrupt) logLine('as7343: ' + corrupt + ' corrupt record(s) skipped', 'er'); return r;
    },
    csv(rows) {
      const wl = ['F1_405nm_mWm2','F2_425nm_mWm2','FZ_450nm_mWm2','F3_475nm_mWm2','F4_515nm_mWm2','F5_550nm_mWm2','FY_555nm_mWm2','FXL_600nm_mWm2','F6_640nm_mWm2','F7_690nm_mWm2','F8_745nm_mWm2','NIR_855nm_mWm2','VIS_broadband_mWm2'].join(',');
      return buildCsvSafe(`datetime,utc_sec,utc_ms,uptime_ms,${wl}`, rows, x => [fmtDt(x.s, x.ms), x.s, x.ms, x.uptime_ms, ...x.ch.map(v => (v / 1000).toFixed(3))].join(','));
    },
    toSql(row, dev) { return { device_id: dev, datetime: fmtDt(row.s, row.ms), F1_405nm_mWm2: row.ch[0]/1000, F2_425nm_mWm2: row.ch[1]/1000, FZ_450nm_mWm2: row.ch[2]/1000, F3_475nm_mWm2: row.ch[3]/1000, F4_515nm_mWm2: row.ch[4]/1000, F5_550nm_mWm2: row.ch[5]/1000, FY_555nm_mWm2: row.ch[6]/1000, FXL_600nm_mWm2: row.ch[7]/1000, F6_640nm_mWm2: row.ch[8]/1000, F7_690nm_mWm2: row.ch[9]/1000, F8_745nm_mWm2: row.ch[10]/1000, NIR_855nm_mWm2: row.ch[11]/1000, VIS_broadband_mWm2: row.ch[12]/1000 }; },
    bin(rows) { const buf = new ArrayBuffer(rows.length * 76), d = new DataView(buf); rows.forEach((x, i) => { const o = i * 76; for (let c = 0; c < AS7_NUM_CH; c++) d.setUint32(o + c * 4, x.ch[c], true); d.setUint16(o + 52, x.ms, true); d.setUint32(o + 56, x.s, true); d.setUint32(o + 64, x.uptime_ms & 0xFFFFFFFF, true); d.setUint32(o + 68, Math.floor(x.uptime_ms / 0x100000000), true); d.setUint32(o + 72, crc32(buf, o, 72), true); }); return buf; }
  },

  mst: {
    label: 'moisture', cls: 'b3', structSz: 16, recSz: 20,
    utcName: 'moisture.bin', bootPattern: /^mst[0-9]{4}\.bin$/i,
    parse(b) {
      const d = new DataView(b), r = [], N = Math.floor(b.byteLength / 20); let corrupt = 0;
      for (let i = 0; i < N; i++) {
        const o = i * 20; if (!verifyCrc(b, o, 16)) { corrupt++; continue; }
        r.push({ vwc: d.getUint16(o, true) / 100, ms: d.getUint16(o + 2, true), s: d.getUint32(o + 4, true), uptime_ms: getUint64(d, o + 8) });
      }
      if (corrupt) logLine('moisture: ' + corrupt + ' corrupt record(s) skipped', 'er'); return r;
    },
    csv(rows) { return buildCsvSafe('datetime,utc_sec,utc_ms,uptime_ms,vwc_percent', rows, x => [fmtDt(x.s, x.ms), x.s, x.ms, x.uptime_ms, x.vwc.toFixed(2)].join(',')); },
    toSql(row, dev) { return { device_id: dev, datetime: fmtDt(row.s, row.ms), vwc_percent: parseFloat(row.vwc.toFixed(2)) }; },
    bin(rows) { const buf = new ArrayBuffer(rows.length * 20), d = new DataView(buf); rows.forEach((x, i) => { const o = i * 20; d.setUint16(o, Math.round(x.vwc * 100), true); d.setUint16(o + 2, x.ms, true); d.setUint32(o + 4, x.s, true); d.setUint32(o + 8, x.uptime_ms & 0xFFFFFFFF, true); d.setUint32(o + 12, Math.floor(x.uptime_ms / 0x100000000), true); d.setUint32(o + 16, crc32(buf, o, 16), true); }); return buf; }
  },

  snd: {
    label: 'sound', cls: 'b4', structSz: 712, recSz: 716,
    utcName: 'sound.bin', bootPattern: /^snd[0-9]{4}\.bin$/i,
    parse(b) {
      const hasUptime = (b.byteLength % 716 === 0), recSz = hasUptime ? 716 : 708, structSz = hasUptime ? 712 : 704;
      const d = new DataView(b), r = [], N = Math.floor(b.byteLength / recSz); let corrupt = 0;
      for (let i = 0; i < N; i++) {
        const o = i * recSz; if (!verifyCrc(b, o, structSz)) { corrupt++; continue; }
        const bins = [], binOff = hasUptime ? o + 16 : o + 8;
        for (let j = 0; j < SOUND_NUM_BINS; j++) bins.push(d.getUint16(binOff + j * 2, true));
        r.push({ s: d.getUint32(o, true), ms: d.getUint16(o + 4, true), rms: (d.getInt16(o + 6, true) / 100) + 120, uptime_ms: hasUptime ? getUint64(d, o + 8) : 0, bins });
      }
      if (corrupt) logLine('sound: ' + corrupt + ' corrupt record(s) skipped', 'er');
      if (!hasUptime) logLine('sound: old layout detected — uptime will be 0', 'mg');
      return r;
    },
    csv(rows) {
      const bh = Array.from({ length: SOUND_NUM_BINS }, (_, i) => `bin${i + 1}_${Math.round((i + 1) * 43.1)}hz_dbspl`).join(',');
      return buildCsvSafe(`datetime,utc_sec,utc_ms,uptime_ms,rms_dbfs,${bh}`, rows, x => [fmtDt(x.s, x.ms), x.s, x.ms, x.uptime_ms, x.rms.toFixed(2), ...x.bins.map(v => (v / 100.0).toFixed(2))].join(','));
    },
    toSql(row, dev) {
      return {
        device_id: dev,
        datetime:  fmtDt(row.s, row.ms),
        uptime_ms: row.uptime_ms,
        rms_dbfs:  parseFloat(row.rms.toFixed(2)),
        bins:      row.bins.map(v => parseFloat((v / 100.0).toFixed(2)))
      };
    },
    bin(rows) { const buf = new ArrayBuffer(rows.length * 716), d = new DataView(buf); rows.forEach((x, i) => { const o = i * 716; d.setUint32(o, x.s, true); d.setUint16(o + 4, x.ms, true); d.setInt16(o + 6, Math.round(x.rms * 100), true); d.setUint32(o + 8, x.uptime_ms & 0xFFFFFFFF, true); d.setUint32(o + 12, Math.floor(x.uptime_ms / 0x100000000), true); for (let j = 0; j < SOUND_NUM_BINS; j++) d.setUint16(o + 16 + j * 2, x.bins[j], true); d.setUint32(o + 712, crc32(buf, o, 712), true); }); return buf; }
  },

  ds18b20: {
    label: 'ds18b20', cls: 'b3', structSz: 24, recSz: 28,
    utcName: 'ds18b20.bin', bootPattern: /^d18[0-9]{4}\.bin$/i,
    // int32 temp_val1(0) | int32 temp_val2(4) | uint32 utc_sec(8) | uint16 utc_ms(12) | uint16 _pad(14) | uint64 uptime_ms(16)
    parse(b) {
      const d = new DataView(b), r = [], N = Math.floor(b.byteLength / 28); let corrupt = 0;
      for (let i = 0; i < N; i++) {
        const o = i * 28; if (!verifyCrc(b, o, 24)) { corrupt++; continue; }
        const val1 = d.getInt32(o, true), val2 = d.getInt32(o + 4, true);
        r.push({ temp_c: val1 + val2 / 1000000.0, val1, val2, s: d.getUint32(o + 8, true), ms: d.getUint16(o + 12, true), uptime_ms: getUint64(d, o + 16) });
      }
      if (corrupt) logLine('ds18b20: ' + corrupt + ' corrupt record(s) skipped', 'er'); return r;
    },
    csv(rows) { return buildCsvSafe('datetime,utc_sec,utc_ms,uptime_ms,temp_c', rows, x => [fmtDt(x.s, x.ms), x.s, x.ms, x.uptime_ms, x.temp_c.toFixed(4)].join(',')); },
    toSql(row, dev) { return { device_id: dev, datetime: fmtDt(row.s, row.ms), temp_c: parseFloat(row.temp_c.toFixed(4)) }; },
    bin(rows) { const buf = new ArrayBuffer(rows.length * 28), d = new DataView(buf); rows.forEach((x, i) => { const o = i * 28; d.setInt32(o, x.val1, true); d.setInt32(o + 4, x.val2, true); d.setUint32(o + 8, x.s, true); d.setUint16(o + 12, x.ms, true); d.setUint32(o + 16, x.uptime_ms & 0xFFFFFFFF, true); d.setUint32(o + 20, Math.floor(x.uptime_ms / 0x100000000), true); d.setUint32(o + 24, crc32(buf, o, 24), true); }); return buf; }
  }
};

// ── File classification ───────────────────────────────────────────────────
function classifyName(name) {
  const lower = name.toLowerCase();
  for (const [key, m] of Object.entries(MODALITIES)) {
    if (lower === m.utcName)        return { mod: key, isUtc: true,  isBoot: false };
    if (m.bootPattern.test(lower))  return { mod: key, isUtc: false, isBoot: true  };
  }
  return null;
}

// Detect modality from CSV filename or first header line
const CSV_HEADER_SIGNATURES = {
  bme:     ['temp_c', 'rh_pct', 'press_hpa'],
  ens:     ['eco2_ppm', 'tvoc_ppb', 'aqi'],
  as7:     ['f1_405nm', 'f2_425nm'],
  mst:     ['vwc_percent'],
  snd:     ['rms_dbfs', 'bin1_'],
  ds18b20: ['temp_c'],  // checked after bme to avoid collision — bme has more cols
};

const CSV_NAME_PATTERNS = {
  bme:     /bme280/i,
  ens:     /ens160/i,
  as7:     /as7343/i,
  mst:     /moisture|mst/i,
  snd:     /sound|snd/i,
  ds18b20: /ds18b20|d18/i,
};

function classifyCsvName(name) {
  for (const [key, pat] of Object.entries(CSV_NAME_PATTERNS)) {
    if (pat.test(name)) return key;
  }
  return null;
}

function classifyCsvHeader(headerLine) {
  const h = headerLine.toLowerCase();
  // Check more specific signatures first to avoid bme/ds18b20 collision
  const order = ['as7', 'snd', 'ens', 'mst', 'bme', 'ds18b20'];
  for (const key of order) {
    const sigs = CSV_HEADER_SIGNATURES[key];
    if (sigs.every(s => h.includes(s))) return key;
  }
  return null;
}

// Parse a CSV file into internal row format compatible with toSql()
function parseCsvRows(text, modKey) {
  const lines = text.split(/\r?\n/).filter(l => l.trim());
  if (lines.length < 2) return [];
  const header = lines[0].toLowerCase().split(',').map(h => h.trim());
  const rows = [];

  const col = name => header.indexOf(name);
  const get = (cells, name) => { const i = col(name); return i >= 0 ? cells[i] : null; };
  const getNum = (cells, name) => { const v = get(cells, name); return v !== null ? parseFloat(v) : null; };

  for (let i = 1; i < lines.length; i++) {
    const cells = lines[i].split(',');
    try {
      const dtStr = get(cells, 'datetime') || '';
      const s = parseInt(get(cells, 'utc_sec')) || 0;
      const ms = parseInt(get(cells, 'utc_ms')) || 0;
      const uptime_ms = parseInt(get(cells, 'uptime_ms')) || 0;

      let row = { s, ms, uptime_ms, _datetime: dtStr };

      if (modKey === 'bme') {
        row.temp  = getNum(cells, 'temp_c');
        row.rh    = getNum(cells, 'rh_pct');
        row.press = getNum(cells, 'press_hpa');
      } else if (modKey === 'ens') {
        row.eco2 = getNum(cells, 'eco2_ppm');
        row.tvoc = getNum(cells, 'tvoc_ppb');
        row.aqi  = getNum(cells, 'aqi');
      } else if (modKey === 'as7') {
        const wl = ['f1_405nm_mwm2','f2_425nm_mwm2','fz_450nm_mwm2','f3_475nm_mwm2',
                    'f4_515nm_mwm2','f5_550nm_mwm2','fy_555nm_mwm2','fxl_600nm_mwm2',
                    'f6_640nm_mwm2','f7_690nm_mwm2','f8_745nm_mwm2','nir_855nm_mwm2','vis_broadband_mwm2'];
        row.ch = wl.map(w => Math.round((getNum(cells, w) || 0) * 1000));
      } else if (modKey === 'mst') {
        row.vwc = getNum(cells, 'vwc_percent');
      } else if (modKey === 'snd') {
        row.rms  = getNum(cells, 'rms_dbfs');
        row.bins = [];
        for (let b = 0; b < SOUND_NUM_BINS; b++) {
          const idx = col('bin' + (b + 1) + '_' + Math.round((b + 1) * 43.1) + 'hz_dbspl');
          row.bins.push(idx >= 0 ? Math.round(parseFloat(cells[idx]) * 100) : 0);
        }
      } else if (modKey === 'ds18b20') {
        row.temp_c = getNum(cells, 'temp_c');
        row.val1   = Math.trunc(row.temp_c);
        row.val2   = Math.round((row.temp_c - row.val1) * 1000000);
      }
      rows.push(row);
    } catch (e) { /* skip bad rows */ }
  }
  return rows;
}

// Override toSql for CSV rows — datetime comes from the CSV string directly
function csvRowToSql(modKey, row, dev) {
  const m = MODALITIES[modKey];
  const base = m.toSql(row, dev);
  // If the CSV has a proper datetime string, prefer it over reconstructed fmtDt
  if (row._datetime && row._datetime.match(/^\d{4}-\d{2}-\d{2}/)) {
    base.datetime = row._datetime;
  }
  return base;
}

function getRecSz(key) { return MODALITIES[key].recSz; }

// ── State ─────────────────────────────────────────────────────────────────
const fileMap    = new Map();
const dz         = document.getElementById('dz');
const fi         = document.getElementById('fi');
const fl         = document.getElementById('fl');
const cb         = document.getElementById('cb');
const lg         = document.getElementById('lg');
const mp         = document.getElementById('mp');
const mergePairs = document.getElementById('mergePairs');

fi.addEventListener('change', function () { if (this.files && this.files.length > 0) processFiles(Array.from(this.files)); this.value = ''; });
dz.addEventListener('dragenter', e => { e.preventDefault(); dz.classList.add('over'); });
dz.addEventListener('dragover',  e => { e.preventDefault(); dz.classList.add('over'); });
dz.addEventListener('dragleave', e => { if (!dz.contains(e.relatedTarget)) dz.classList.remove('over'); });
dz.addEventListener('drop',      e => { e.preventDefault(); dz.classList.remove('over'); processFiles(Array.from(e.dataTransfer.files)); });
cb.addEventListener('click', doConvert);

// ── File processing ───────────────────────────────────────────────────────
function processFiles(list) {
  list.forEach(f => {
    const name = f.name.toLowerCase();

    // ── CSV path ──────────────────────────────────────────────────────────
    if (name.endsWith('.csv')) {
      const reader = new FileReader();
      reader.onload = e => {
        const text = e.target.result;
        const firstLine = text.split(/\r?\n/)[0] || '';
        // Try to classify by header first, fall back to filename
        const modKey = classifyCsvHeader(firstLine) || classifyCsvName(name);
        if (!modKey) {
          fileMap.set(f.name, { file: f, modKey: null, isCsv: true, recs: '?', err: 'unrecognised CSV' });
        } else {
          const lineCount = text.split(/\r?\n/).filter(l => l.trim()).length - 1;
          fileMap.set(f.name, { file: f, modKey, isCsv: true, isUtc: false, isBoot: false, recs: lineCount, err: null, csvText: text });
        }
        render();
        updateMergePanel();
      };
      reader.readAsText(f);
      return;
    }

    // ── BIN path ──────────────────────────────────────────────────────────
    if (!name.endsWith('.bin')) return;
    const cls = classifyName(name);
    if (!cls) { fileMap.set(name, { file: f, modKey: null, isUtc: false, isBoot: false, recs: '?', err: 'unrecognised' }); return; }
    const recSz = getRecSz(cls.mod);
    const recs  = Math.floor(f.size / recSz);
    fileMap.set(name, { file: f, modKey: cls.mod, isUtc: cls.isUtc, isBoot: cls.isBoot, recSz, recs, err: null });
  });
  render();
  updateMergePanel();
}

function removeFile(name) { fileMap.delete(name); render(); updateMergePanel(); }

function render() {
  fl.innerHTML = '';
  fileMap.forEach((info, name) => {
    const m     = info.modKey ? MODALITIES[info.modKey] : null;
    const known = m && !info.err;
    const tag   = info.isCsv ? 'CSV' : info.isUtc ? 'UTC' : info.isBoot ? 'BOOT' : '';
    const tagColor = info.isCsv ? '#ffcc44' : info.isUtc ? '#7fff6e' : '#6eb8ff';
    const div = document.createElement('div');
    div.className = 'fc';
    div.innerHTML = `
      <span class="badge ${known ? m.cls : 'bx'}">${known ? m.label : 'unknown'}</span>
      <div class="fi2">
        <div class="fn">${name} ${tag ? `<span style="font-size:10px;color:${tagColor}">[${tag}]</span>` : ''}</div>
        <div class="fm">${info.isCsv ? 'CSV file' : (info.file.size / 1024).toFixed(1) + ' KB &nbsp;&middot;&nbsp;' + (known ? info.recSz + 'b/rec' : info.err || '?')}</div>
      </div>
      <span class="fs ${known ? '' : 'u'}">${known ? info.recs + ' records' : '?'}</span>
      <button class="rb" data-name="${name}">&times;</button>
    `;
    div.querySelector('.rb').addEventListener('click', () => removeFile(name));
    fl.appendChild(div);
  });
  cb.disabled = [...fileMap.values()].every(i => i.isCsv || !i.modKey);
  updateDevMap();
}

// ── Merge panel ───────────────────────────────────────────────────────────
function findPairs() {
  const pairs = [], utcFiles = new Map(), bootFiles = new Map();
  fileMap.forEach((info, name) => {
    if (!info.modKey || info.err) return;
    if (info.isUtc)  utcFiles.set(info.modKey, name);
    if (info.isBoot) bootFiles.set(info.modKey, name);
  });
  utcFiles.forEach((utcName, mod) => { if (bootFiles.has(mod)) pairs.push({ mod, utcName, bootName: bootFiles.get(mod) }); });
  return pairs;
}

function updateMergePanel() {
  const pairs = findPairs();
  if (pairs.length === 0) { mp.classList.remove('visible'); return; }
  mp.classList.add('visible');
  mergePairs.innerHTML = '';
  pairs.forEach(p => {
    const m = MODALITIES[p.mod];
    const div = document.createElement('div');
    div.className = 'merge-pair';
    div.innerHTML = `<span class="badge ${m.cls}">${m.label}</span><span class="pair-utc">${p.utcName}</span><span class="pair-plus">+</span><span class="pair-upt">${p.bootName}</span><span class="pair-arrow">&rarr;</span><span class="pair-out">${p.utcName.replace('.bin', '_merged.csv')}</span>`;
    mergePairs.appendChild(div);
  });
}

// ── Offset / mode ─────────────────────────────────────────────────────────
let currentMode = 'unix';

function setMode(mode) {
  currentMode = mode;
  document.getElementById('modeUnix').classList.toggle('active', mode === 'unix');
  document.getElementById('modeDt').classList.toggle('active', mode === 'datetime');
  document.getElementById('rowUnix').style.display = mode === 'unix' ? 'flex' : 'none';
  document.getElementById('rowDt').style.display   = mode === 'datetime' ? 'flex' : 'none';
  updatePreview();
}

function getOffsetSec() {
  if (currentMode === 'unix') return parseFloat(document.getElementById('offsetUnix').value) || 0;
  const day   = parseInt(document.getElementById('dtDay').value)   || 0;
  const month = parseInt(document.getElementById('dtMonth').value) || 0;
  const year  = parseInt(document.getElementById('dtYear').value)  || 0;
  const hour  = parseInt(document.getElementById('dtHour').value)  || 0;
  const min   = parseInt(document.getElementById('dtMin').value)   || 0;
  const sec   = parseInt(document.getElementById('dtSec').value)   || 0;
  if (!day || !month || !year) return 0;
  return Date.UTC(year, month - 1, day, hour, min, sec) / 1000;
}

function updatePreview() {
  const sec      = getOffsetSec();
  const preview  = document.getElementById('offsetPreview');
  const resolved = document.getElementById('offsetResolved');
  if (sec > UTC_MIN) {
    resolved.textContent = sec + 's  →  ' + new Date(sec * 1000).toISOString().replace('T', ' ').replace('Z', '') + ' UTC';
    preview.style.display = 'flex';
  } else {
    preview.style.display = 'none';
  }
}

['offsetUnix', 'dtDay', 'dtMonth', 'dtYear', 'dtHour', 'dtMin', 'dtSec'].forEach(id =>
  document.getElementById(id).addEventListener('input', updatePreview)
);

// ── Download helpers ──────────────────────────────────────────────────────
function download(filename, data) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(data instanceof Blob ? data : new Blob([data], { type: 'text/csv' }));
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); document.body.removeChild(a); }, 200);
}

function downloadBin(filename, arrayBuffer) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([arrayBuffer], { type: 'application/octet-stream' }));
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); document.body.removeChild(a); }, 200);
}

// ── Convert ───────────────────────────────────────────────────────────────
async function doConvert() {
  lg.innerHTML = ''; lg.style.display = 'block'; let ok = 0, err = 0;
  const pairs = findPairs(), mergedMods = new Set(pairs.map(p => p.mod)), offsetSec = getOffsetSec(), hasOffset = offsetSec > UTC_MIN;

  for (const pair of pairs) {
    try {
      const m = MODALITIES[pair.mod], utcInfo = fileMap.get(pair.utcName), bootInfo = fileMap.get(pair.bootName);
      logLine('Merging ' + pair.bootName + ' + ' + pair.utcName + '...', 'mg');
      let utcRows = m.parse(await utcInfo.file.arrayBuffer()), bootRows = m.parse(await bootInfo.file.arrayBuffer());
      logLine('Parsed: ' + utcRows.length + ' UTC records, ' + bootRows.length + ' boot records', 'in');
      // Back-calculate pre-sync records using uptime_ms anchor from whichever file has valid UTC
      utcRows  = backfillUtc(utcRows);
      bootRows = backfillUtc(bootRows);
      // If boot file still has pre-sync records and a manual offset was provided, apply it as fallback
      if (hasOffset) {
        const before = bootRows.filter(r => r.s <= UTC_MIN).length;
        bootRows = bootRows.map(r => applyOffset(r, offsetSec));
        if (before > 0) logLine('Applied offset to ' + before + ' pre-sync boot records', 'mg');
      } else {
        const unsynced = bootRows.filter(r => r.s <= UTC_MIN).length;
        if (unsynced > 0) logLine('WARNING: ' + unsynced + ' boot records have no UTC', 'er');
      }
      const utcTimes = new Set(utcRows.map(r => r.s)), uniqueBoot = bootRows.filter(r => !utcTimes.has(r.s));
      logLine('After dedup: ' + uniqueBoot.length + ' unique boot records added', 'ok');
      const merged = [...utcRows, ...uniqueBoot].sort((a, b) => (a.s + a.ms / 1000) - (b.s + b.ms / 1000));
      logLine('Total merged: ' + merged.length + ' records', 'ok');
      const mergeResult = m.csv(merged);
      if (mergeResult.error) {
        logLine('CSV error at record ' + mergeResult.failIndex + '/' + merged.length + ': ' + mergeResult.error, 'er');
        if (mergeResult.rowsWritten > 0) {
          download(pair.utcName.replace('.bin', '_merged_partial.csv'), mergeResult.blob);
          logLine('Downloaded PARTIAL merged CSV — ' + mergeResult.rowsWritten + ' of ' + merged.length + ' rows', 'mg');
        }
        err++;
      } else {
        download(pair.utcName.replace('.bin', '_merged.csv'), mergeResult.blob); logLine('Downloaded CSV', 'ok');
        downloadBin(pair.utcName.replace('.bin', '_merged.bin'), m.bin(merged)); logLine('Downloaded BIN', 'ok');
        ok++;
      }
    } catch (e) { logLine('ERROR merging ' + pair.utcName + ': ' + e.message, 'er'); err++; }
  }

  for (const [name, info] of fileMap) {
    if (info.modKey && mergedMods.has(info.modKey) && (info.isUtc || info.isBoot)) continue;
    if (!info.modKey || info.err) { logLine('SKIP ' + name + (info.err ? ' — ' + info.err : ''), 'er'); err++; continue; }
    try {
      const m = MODALITIES[info.modKey];
      logLine('Reading ' + name + '...', 'in');
      const buf = await info.file.arrayBuffer(); let rows = m.parse(buf);
      if (info.isBoot && hasOffset) {
        const before = rows.filter(r => r.s <= UTC_MIN).length;
        rows = rows.map(r => applyOffset(r, offsetSec));
        if (before > 0) logLine('Applied UTC offset to ' + before + ' pre-sync records', 'mg');
      }
      logLine('Parsed ' + rows.length + ' records', 'ok');
      rows = backfillUtc(rows);
      const backfilled = rows.filter(r => r.s > UTC_MIN).length;
      const stillUnsynced = rows.filter(r => r.s <= UTC_MIN).length;
      if (backfilled > 0 && stillUnsynced === 0) logLine('Back-calculated UTC for all pre-sync records', 'mg');
      else if (backfilled > 0) logLine('Back-calculated UTC for some pre-sync records (' + stillUnsynced + ' remain unsynced)', 'mg');
      const result = m.csv(rows);
      if (result.error) {
        logLine('CSV error at record ' + result.failIndex + '/' + rows.length + ': ' + result.error, 'er');
        if (result.rowsWritten > 0) {
          download(name.replace('.bin', '_partial.csv'), result.blob);
          logLine('Downloaded PARTIAL CSV — ' + result.rowsWritten + ' of ' + rows.length + ' rows', 'mg');
        }
        err++;
      } else {
        download(name.replace('.bin', '.csv'), result.blob);
        logLine('Downloaded ' + name.replace('.bin', '.csv') + ' (' + result.rowsWritten + ' rows)', 'ok');
        ok++;
      }
    } catch (e) { logLine('ERROR ' + name + ': ' + e.message, 'er'); err++; }
  }
  logLine('Done — ' + ok + ' converted, ' + err + ' errors', ok > 0 ? 'ok' : 'er');
}

function logLine(msg, cls = '') {
  lg.style.display = 'block';
  const d = document.createElement('div');
  d.className = 'll ' + cls;
  d.textContent = '[' + new Date().toTimeString().slice(0, 8) + '] ' + msg;
  lg.appendChild(d);
  lg.scrollTop = lg.scrollHeight;
}

// ════════════════════════════════════════════════════════════════
// DIRECT SQL UPLOAD
// ════════════════════════════════════════════════════════════════
let skipUnsynced = true;

const SQL_MODALITY = {
  bme:     'bme280',
  ens:     'ens160',
  as7:     'as7343',
  mst:     'moisture',
  snd:     'sound',
  ds18b20: 'ds18b20',
};

function setSkip(v) {
  skipUnsynced = v;
  document.getElementById('skipOn').classList.toggle('active', v);
  document.getElementById('skipOff').classList.toggle('active', !v);
}

function ulog(msg, cls = '') {
  const ulg = document.getElementById('ulg');
  ulg.style.display = 'block';
  const d = document.createElement('div');
  d.className = 'll ' + cls;
  d.textContent = '[' + new Date().toTimeString().slice(0, 8) + '] ' + msg;
  ulg.appendChild(d);
  ulg.scrollTop = ulg.scrollHeight;
}

function updateDevMap() {
  const section = document.getElementById('devSection'), devMap = document.getElementById('devMap');
  devMap.innerHTML = '';
  const entries = [...fileMap.entries()].filter(([, i]) => i.modKey && !i.err);
  if (entries.length === 0) { section.style.display = 'none'; document.getElementById('sqlUploadBtn').disabled = true; return; }
  section.style.display = 'block'; document.getElementById('sqlUploadBtn').disabled = false;
  entries.forEach(([name, info]) => {
    const m = MODALITIES[info.modKey];
    const row = document.createElement('div');
    row.className = 'dev-map-row';
    const tag = info.isCsv ? ' <span style="font-size:10px;color:#ffcc44">[CSV]</span>' : '';
    row.innerHTML = `<span class="badge ${m.cls}" style="width:80px;text-align:center">${m.label}</span><span class="dev-map-file">${name}${tag}</span><select class="dev-map-select" id="ds_${name.replace(/\W/g, '_')}"><option value="dev-1" ${['bme','ens','snd'].includes(info.modKey)?'selected':''}>dev-1 (Node 1)</option><option value="dev-2" ${['as7','mst','ds18b20'].includes(info.modKey)?'selected':''}>dev-2 (Node 2)</option></select>`;
    devMap.appendChild(row);
  });
}

function getFileDevice(name) {
  const sel = document.getElementById('ds_' + name.replace(/\W/g, '_'));
  return sel ? sel.value : 'dev-1';
}

async function postToSql(modality, rows) {
  const resp = await fetch(`http://${RELAY_HOST}:8080/upload_csv`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ modality, rows })
  });
  if (!resp.ok) { const txt = await resp.text().catch(() => ''); throw new Error('HTTP ' + resp.status + (txt ? ': ' + txt.slice(0, 120) : '')); }
  const json = await resp.json();
  if (json.error) throw new Error(json.error);
  return json.ok;
}

async function doSqlUpload() {
  const ulg = document.getElementById('ulg');
  ulg.innerHTML = ''; ulg.style.display = 'block';
  document.getElementById('uploadStats').style.display = 'none';
  const btn = document.getElementById('sqlUploadBtn');
  btn.disabled = true; btn.textContent = '⏳ Uploading to SQL...';
  const pw = document.getElementById('upProgressWrap'), pb = document.getElementById('upProgressBar');
  pw.style.display = 'block'; pb.style.width = '0%';

  let totalSent = 0, totalErr = 0, totalSkipped = 0;
  const files = [...fileMap.entries()].filter(([, i]) => i.modKey && !i.err && MODALITIES[i.modKey].toSql);

  for (let f = 0; f < files.length; f++) {
    const [name, info] = files[f];
    const m = MODALITIES[info.modKey];
    const modality = SQL_MODALITY[info.modKey];
    const dev = getFileDevice(name);

    let sqlRows;

    if (info.isCsv) {
      // CSV path — parse from cached text
      const rows = parseCsvRows(info.csvText, info.modKey);
      if (rows.length === 0) { ulog('No parseable rows in ' + name, 'er'); totalErr++; continue; }
      const filtered = skipUnsynced ? rows.filter(r => r.s > UTC_MIN || (r._datetime && r._datetime.match(/^\d{4}-\d{2}-\d{2}/))) : rows;
      totalSkipped += rows.length - filtered.length;
      sqlRows = filtered.map(r => csvRowToSql(info.modKey, r, dev));
    } else {
      // BIN path — original logic
      const rows = m.parse(await info.file.arrayBuffer());
      const backfilled = backfillUtc(rows);
      const filtered = skipUnsynced ? backfilled.filter(r => r.s > UTC_MIN) : backfilled;
      totalSkipped += rows.length - filtered.length;
      if (filtered.length === 0) { ulog('Nothing to upload from ' + name, 'mg'); continue; }
      sqlRows = filtered.map(r => m.toSql(r, dev));
    }

    if (!sqlRows || sqlRows.length === 0) { ulog('Nothing to upload from ' + name, 'mg'); continue; }
    const batches = [];
    for (let i = 0; i < sqlRows.length; i += BATCH_SIZE) {
      batches.push(sqlRows.slice(i, i + BATCH_SIZE));
    }

    ulog('Starting ' + name + ' → SQL (' + sqlRows.length + ' records, ' + batches.length + ' batches)', 'in');
    let fileSent = 0, fileErr = 0;

    for (let i = 0; i < batches.length; i++) {
      try {
        const n = await postToSql(modality, batches[i]);
        fileSent += n;
      } catch (e) {
        ulog('Batch ' + (i + 1) + '/' + batches.length + ' FAILED: ' + e.message, 'er'); fileErr++;
      }
      pb.style.width = Math.round(((f / files.length) + ((i + 1) / batches.length / files.length)) * 100) + '%';
    }

    totalSent += fileSent; totalErr += fileErr;
    ulog(name + ' — ' + fileSent + ' inserted' + (fileErr > 0 ? ', ' + fileErr + ' batch errors' : ''), fileErr > 0 ? 'er' : 'ok');
  }

  pb.style.width = '100%';
  const summary = totalSent + ' inserted, ' + totalSkipped + ' skipped, ' + totalErr + ' batch errors';
  ulog('Complete — ' + summary, totalErr === 0 ? 'ok' : 'er');
  const st = document.getElementById('uploadStats');
  st.style.display = 'block'; st.textContent = summary;
  btn.disabled = false; btn.textContent = '🗄 Upload to SQL';
}

document.getElementById('sqlUploadBtn').addEventListener('click', doSqlUpload);