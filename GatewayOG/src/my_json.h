#ifndef BASE_JSON_H
#define BASE_JSON_H

#include <zephyr/kernel.h>

#define JSON_BUFFER_SIZE 1024
#define AS7343_NUM_CH 13

// wavelengths used to label AS7343 metrics
static const uint16_t AS7343_WL[AS7343_NUM_CH] = {
    405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855, 999 /*VISIBLE*/
};

// ------------- Gateway-side unpacked payload (matches 49B packet) -------------
typedef struct {
    uint32_t time;          // epoch s
    uint32_t uptime_ms;     // ms
    uint8_t  proto_ver;     // PROTO_VER from node
    uint8_t  dev_id;        // node id

    // BME280 (integer scaled on node)
    int16_t  temp_c_x100;   // e.g. 2350 -> 23.50 °C
    int16_t  rh_x100;       // e.g. 4567 -> 45.67 %
    int32_t  press_hPa_x1000; // e.g. 10132 -> 1013.2 hPa

    // ENS160
    uint16_t eco2_ppm;      // ppm
    uint16_t tvoc_ppb;      // ppb
    uint8_t  aqi;           // 0..5 (pass-through)

    // AS7343 spectrum (fixed order above)
    uint16_t as7343[AS7343_NUM_CH];

    // Battery
    uint16_t batt_mV;
} tracker_payload_t;


// ----------------------------- JSON blocks -----------------------------
// Environment block (stringified for printf-based JSON build)
struct json_payload_environment {
    char temperature_c[16];    // e.g., "23.50"
    char humidity_percent[16]; // e.g., "45.67"
    char pressure_hpa[16];     // e.g., "1013.2"
    char eco2_ppm[16];         // e.g., "415"
    char tvoc_ppb[16];         // e.g., "28"
    char aqi[8];               // e.g., "1"
    char batt_mV[16];          // e.g., "3820"
};

// Spectrum block (AS7343_* keys as strings)
struct json_payload_spectrum {
    char AS7343_405nm[12];
    char AS7343_425nm[12];
    char AS7343_450nm[12];
    char AS7343_475nm[12];
    char AS7343_515nm[12];
    char AS7343_550nm[12];
    char AS7343_555nm[12];
    char AS7343_600nm[12];
    char AS7343_640nm[12];
    char AS7343_690nm[12];
    char AS7343_745nm[12];
    char AS7343_855nm[12];
    char AS7343_VISIBLE[12];   // sum/visible channel
};

// Payload block (no location/acceleration)
struct json_payload {
    char deviceId[20];       // dev_id as string
    char timestamp[72];      // ISO8601 or unix string
    uint32_t uptime_ms;      // keep as number
    uint8_t  proto_ver;      // keep as number
    struct json_payload_environment environment;
    struct json_payload_spectrum    spectrum;
};

// Header block (kept for compatibility; adjust if you like)
struct json_header {
    char messageId[40];
    char gatewayId[12];
    char schemaVersion[8];
    char messageType[12];
};

// Signature block (kept for compatibility; unused for now)
struct json_signature {
    char alg[16];
    char keyId[24];
    char value[65];
};

// Final full packet
struct json_full_packet {
    struct json_header  header;
    struct json_payload payload;
    struct json_signature signature;
};

// ----------------------------- JSON format -----------------------------
// NOTE: Numbers that should remain numeric are unquoted in this format.
// Strings stay quoted. Adjust to your backend expectations.
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
          "\"temperature_c\":\"%s\","
          "\"humidity_percent\":\"%s\","
          "\"pressure_hpa\":\"%s\","
          "\"eco2_ppm\":\"%s\","
          "\"tvoc_ppb\":\"%s\","
          "\"aqi\":\"%s\","
          "\"batt_mV\":\"%s\""
        "},"
        "\"spectrum\":{"
          "\"AS7343_405nm\":\"%s\","
          "\"AS7343_425nm\":\"%s\","
          "\"AS7343_450nm\":\"%s\","
          "\"AS7343_475nm\":\"%s\","
          "\"AS7343_515nm\":\"%s\","
          "\"AS7343_550nm\":\"%s\","
          "\"AS7343_555nm\":\"%s\","
          "\"AS7343_600nm\":\"%s\","
          "\"AS7343_640nm\":\"%s\","
          "\"AS7343_690nm\":\"%s\","
          "\"AS7343_745nm\":\"%s\","
          "\"AS7343_855nm\":\"%s\","
          "\"AS7343_VISIBLE\":\"%s\""
        "}"
      "},"
      "\"signature\":{"
        "\"alg\":\"%s\","
        "\"keyId\":\"%s\","
        "\"value\":\"%s\""
      "}"
    "}";

// API
extern void fill_json_packet(const tracker_payload_t *payload, struct json_full_packet *packet);
extern void encode_and_print_json(const struct json_full_packet *packet);

#endif // BASE_JSON_H
