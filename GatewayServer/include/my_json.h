#ifndef BASE_JSON_H
#define BASE_JSON_H

#include <zephyr/kernel.h>
#include <stdint.h>

/* ── Buffer sizes ───────────────────────────────────────── */
#define JSON_BUFFER_SIZE        1600   /* sensor telemetry JSON (increased for soil field) */
#define SOUND_JSON_BUFFER_SIZE  8192   /* sound spectrum JSON (348 floats)                 */

/* ── AS7343 ─────────────────────────────────────────────── */
#define AS7343_NUM_CH  13
static const uint16_t AS7343_WL[AS7343_NUM_CH] = {
    405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855, 999
};

/* ── Sound ──────────────────────────────────────────────── */
#define SOUND_NUM_BINS  348

/* ═══════════════════════════════════════════════════════════
 * Sensor packet payload  (61-byte UART frame, magic 0xAA)
 *
 * Byte layout:
 *   [0..3]   time              uint32 BE — UTC seconds
 *   [4..5]   time_ms           uint16 BE — UTC milliseconds (0–999)
 *   [6..9]   uptime_ms         uint32 BE
 *   [10]     proto_ver
 *   [11]     dev_id
 *   [12..13] temp_c_x100       int16  BE
 *   [14..15] rh_x100           int16  BE
 *   [16..19] press_hPa_x1000   int32  BE
 *   [20..21] eco2_ppm          uint16 BE
 *   [22..23] tvoc_ppb          uint16 BE
 *   [24]     aqi
 *   [25..50] AS7343 ×13        uint16 BE each (26 bytes)
 *   [51..52] batt_mV           uint16 BE
 *   [53..54] snd_rms_dbfs_x100 int16  BE
 *   [55..56] snd_peak_freq_hz  uint16 BE
 *   [57..58] snd_peak_mag_x10  uint16 BE
 *   [59..60] soil_vwc_x100     uint16 BE — VWC % × 100 (0–10000)
 * ═══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t time;
    uint16_t time_ms;          /* UTC sub-second milliseconds (0–999)    */
    uint32_t uptime_ms;
    uint8_t  proto_ver;
    uint8_t  dev_id;

    int16_t  temp_c_x100;
    int16_t  rh_x100;
    int32_t  press_hPa_x1000;

    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;

    uint16_t as7343[AS7343_NUM_CH];
    uint16_t batt_mV;

    int16_t  snd_rms_dbfs_x100;
    uint16_t snd_peak_freq_hz;
    uint16_t snd_peak_mag_x10;

    uint16_t soil_vwc_x100;    /* VWC % × 100, e.g. 4567 = 45.67%       */
} tracker_payload_t;

/* ═══════════════════════════════════════════════════════════
 * Sound spectrum payload  (698-byte UART frame, magic 0xBB)
 * ═══════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  dev_id;
    uint32_t uptime_ms;
    int16_t  rms_dbfs_x100;
    uint16_t bins[SOUND_NUM_BINS];
} sound_payload_t;

/* ── JSON struct blocks (sensor telemetry) ───────────────── */
struct json_payload_environment {
    double   temperature_c;
    double   humidity_percent;
    double   pressure_hpa;
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;
    uint16_t batt_mV;
};

struct json_payload_spectrum {
    uint16_t AS7343_405nm;
    uint16_t AS7343_425nm;
    uint16_t AS7343_450nm;
    uint16_t AS7343_475nm;
    uint16_t AS7343_515nm;
    uint16_t AS7343_550nm;
    uint16_t AS7343_555nm;
    uint16_t AS7343_600nm;
    uint16_t AS7343_640nm;
    uint16_t AS7343_690nm;
    uint16_t AS7343_745nm;
    uint16_t AS7343_855nm;
    uint16_t AS7343_VISIBLE;
};

struct json_payload_sound_summary {
    double   rms_dbfs;
    uint16_t peak_freq_hz;
    double   peak_mag;
};

struct json_payload {
    char     deviceId[20];
    char     timestamp[72];
    uint32_t uptime_ms;
    uint8_t  proto_ver;
    struct json_payload_environment   environment;
    struct json_payload_spectrum      spectrum;
    struct json_payload_sound_summary sound;
    double   soil_vwc;          /* VWC % as double, e.g. 45.67           */
};

struct json_header {
    char messageId[40];
    char gatewayId[12];
    char schemaVersion[8];
    char messageType[16];
};

struct json_signature {
    char alg[16];
    char keyId[24];
    char value[65];
};

struct json_full_packet {
    struct json_header    header;
    struct json_payload   payload;
    struct json_signature signature;
};

/* ── Sensor telemetry JSON format string ─────────────────── */
static const char JSON_FORMAT[] =
    "{"
      "\"header\":{"
        "\"messageId\":\"%s\","
        "\"gatewayId\":\"%s\","
        "\"schemaVersion\":\"%s\","
        "\"messageType\":\"%s\""
      "},"
      "\"payload\":{"
        "\"deviceId\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"uptime_ms\":%u,"
        "\"proto_ver\":%u,"
        "\"environment\":{"
          "\"temperature_c\":%.2f,"
          "\"humidity_percent\":%.2f,"
          "\"pressure_hpa\":%.3f,"
          "\"eco2_ppm\":%u,"
          "\"tvoc_ppb\":%u,"
          "\"aqi\":%u,"
          "\"batt_mV\":%u"
        "},"
        "\"spectrum\":{"
          "\"AS7343_405nm\":%u,"
          "\"AS7343_425nm\":%u,"
          "\"AS7343_450nm\":%u,"
          "\"AS7343_475nm\":%u,"
          "\"AS7343_515nm\":%u,"
          "\"AS7343_550nm\":%u,"
          "\"AS7343_555nm\":%u,"
          "\"AS7343_600nm\":%u,"
          "\"AS7343_640nm\":%u,"
          "\"AS7343_690nm\":%u,"
          "\"AS7343_745nm\":%u,"
          "\"AS7343_855nm\":%u,"
          "\"AS7343_VISIBLE\":%u"
        "},"
        "\"sound\":{"
          "\"rms_dbfs\":%.2f,"
          "\"peak_freq_hz\":%u,"
          "\"peak_mag\":%.1f"
        "},"
        "\"soil\":{"
          "\"vwc_percent\":%.2f"
        "}"
      "}"
    "}";

/* ── Public API ─────────────────────────────────────────── */
void fill_json_packet(const tracker_payload_t *payload,
                      struct json_full_packet  *packet);

void encode_and_print_json(const struct json_full_packet *packet);

int encode_sound_json(const sound_payload_t *sp, char *buf, size_t buf_size);

#endif /* BASE_JSON_H */