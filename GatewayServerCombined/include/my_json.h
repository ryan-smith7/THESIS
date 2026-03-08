#ifndef BASE_JSON_H
#define BASE_JSON_H

#include <zephyr/kernel.h>

#define JSON_BUFFER_SIZE 1024
#define AS7343_NUM_CH 13

static const uint16_t AS7343_WL[AS7343_NUM_CH] = {
    405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855, 999
};

typedef struct {
    uint32_t time;
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
} tracker_payload_t;

struct json_payload_environment {
    char temperature_c[16];
    char humidity_percent[16];
    char pressure_hpa[16];
    char eco2_ppm[16];
    char tvoc_ppb[16];
    char aqi[8];
    char batt_mV[16];
};

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
    char AS7343_VISIBLE[12];
};

struct json_payload {
    char deviceId[20];
    char timestamp[72];
    uint32_t uptime_ms;
    uint8_t  proto_ver;
    struct json_payload_environment environment;
    struct json_payload_spectrum    spectrum;
};

struct json_header {
    char messageId[40];
    char gatewayId[12];
    char schemaVersion[8];
    char messageType[12];
};

struct json_signature {
    char alg[16];
    char keyId[24];
    char value[65];
};

struct json_full_packet {
    struct json_header  header;
    struct json_payload payload;
    struct json_signature signature;
};

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

extern void fill_json_packet(const tracker_payload_t *payload, struct json_full_packet *packet);
extern void encode_and_publish_json(const struct json_full_packet *packet);
extern void encode_and_print_json(const struct json_full_packet *packet);

#endif /* BASE_JSON_H */