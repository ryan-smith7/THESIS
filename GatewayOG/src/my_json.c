#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/uuid.h>
#include <zephyr/sys/timeutil.h>

#include "my_json.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

LOG_MODULE_REGISTER(json_module);

/* Generate a v4 UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000") */
static void generate_uuid(char *uuid_str)
{
    struct uuid u;
    if (uuid_generate_v4(&u) != 0) {
        uuid_str[0] = '\0';
        return;
    }
    if (uuid_to_string(&u, uuid_str) != 0) {
        uuid_str[0] = '\0';
        return;
    }
}

/* Fill JSON packet from the compact tracker payload (49-byte layout) */
void fill_json_packet(const tracker_payload_t *p, struct json_full_packet *j)
{
    if (!p || !j) return;

    /* ---------------- Header ---------------- */
    char uuid_str[UUID_STR_LEN] = {0};
    generate_uuid(uuid_str);

    /* Reasonable defaults; adjust if you have real IDs/versions */
    snprintk(j->header.messageId,     sizeof j->header.messageId,     "%s", uuid_str[0] ? uuid_str : "00000000-0000-0000-0000-000000000000");
    snprintk(j->header.gatewayId,     sizeof j->header.gatewayId,     "GW-01");
    snprintk(j->header.schemaVersion, sizeof j->header.schemaVersion, "1");
    snprintk(j->header.messageType,   sizeof j->header.messageType,   "telemetry");

    /* ---------------- Payload (top) ---------------- */
    snprintk(j->payload.deviceId, sizeof j->payload.deviceId, "dev-%u", p->dev_id);

    /* Timestamp as ISO8601 UTC (YYYY-MM-DDTHH:MM:SSZ). If you prefer unix string, change format. */
    time_t raw = (time_t)p->time;
    struct tm tm_utc;
    if (gmtime_r(&raw, &tm_utc) == NULL) {
        /* Fallback to unix seconds as string */
        snprintk(j->payload.timestamp, sizeof j->payload.timestamp, "%u", p->time);
    } else {
        snprintk(j->payload.timestamp, sizeof j->payload.timestamp,
                 "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    }

    j->payload.uptime_ms = p->uptime_ms;
    j->payload.proto_ver = p->proto_ver;

    /* ---------------- Environment ---------------- */
    snprintk(j->payload.environment.temperature_c,
             sizeof j->payload.environment.temperature_c,
             "%.2f", p->temp_c_x100 / 100.0f);

    snprintk(j->payload.environment.humidity_percent,
             sizeof j->payload.environment.humidity_percent,
             "%.2f", p->rh_x100 / 100.0f);

    snprintk(j->payload.environment.pressure_hpa,
             sizeof j->payload.environment.pressure_hpa,
             "%.3f", p->press_hPa_x1000 / 1000.0f);

    snprintk(j->payload.environment.eco2_ppm,
             sizeof j->payload.environment.eco2_ppm,
             "%u", p->eco2_ppm);

    snprintk(j->payload.environment.tvoc_ppb,
             sizeof j->payload.environment.tvoc_ppb,
             "%u", p->tvoc_ppb);

    snprintk(j->payload.environment.aqi,
             sizeof j->payload.environment.aqi,
             "%u", p->aqi);

    snprintk(j->payload.environment.batt_mV,
             sizeof j->payload.environment.batt_mV,
             "%u", p->batt_mV);

    /* ---------------- Spectrum (AS7343) ---------------- */
    snprintk(j->payload.spectrum.AS7343_405nm,   sizeof j->payload.spectrum.AS7343_405nm,   "%u", p->as7343[0]);
    snprintk(j->payload.spectrum.AS7343_425nm,   sizeof j->payload.spectrum.AS7343_425nm,   "%u", p->as7343[1]);
    snprintk(j->payload.spectrum.AS7343_450nm,   sizeof j->payload.spectrum.AS7343_450nm,   "%u", p->as7343[2]);
    snprintk(j->payload.spectrum.AS7343_475nm,   sizeof j->payload.spectrum.AS7343_475nm,   "%u", p->as7343[3]);
    snprintk(j->payload.spectrum.AS7343_515nm,   sizeof j->payload.spectrum.AS7343_515nm,   "%u", p->as7343[4]);
    snprintk(j->payload.spectrum.AS7343_550nm,   sizeof j->payload.spectrum.AS7343_550nm,   "%u", p->as7343[5]);
    snprintk(j->payload.spectrum.AS7343_555nm,   sizeof j->payload.spectrum.AS7343_555nm,   "%u", p->as7343[6]);
    snprintk(j->payload.spectrum.AS7343_600nm,   sizeof j->payload.spectrum.AS7343_600nm,   "%u", p->as7343[7]);
    snprintk(j->payload.spectrum.AS7343_640nm,   sizeof j->payload.spectrum.AS7343_640nm,   "%u", p->as7343[8]);
    snprintk(j->payload.spectrum.AS7343_690nm,   sizeof j->payload.spectrum.AS7343_690nm,   "%u", p->as7343[9]);
    snprintk(j->payload.spectrum.AS7343_745nm,   sizeof j->payload.spectrum.AS7343_745nm,   "%u", p->as7343[10]);
    snprintk(j->payload.spectrum.AS7343_855nm,   sizeof j->payload.spectrum.AS7343_855nm,   "%u", p->as7343[11]);
    snprintk(j->payload.spectrum.AS7343_VISIBLE, sizeof j->payload.spectrum.AS7343_VISIBLE, "%u", p->as7343[12]);

    /* ---------------- Signature (kept for schema compat; no hash) ---------------- */
    snprintk(j->signature.alg,   sizeof j->signature.alg,   "none");
    snprintk(j->signature.keyId, sizeof j->signature.keyId, "na");
    snprintk(j->signature.value, sizeof j->signature.value, "");
}

/* Encode to JSON using JSON_FORMAT and print to UART/log */
void encode_and_print_json(const struct json_full_packet *j)
{
    static char json_output[JSON_BUFFER_SIZE];

    int ret = snprintk(json_output, sizeof json_output,
        JSON_FORMAT,
        /* header */
        j->header.messageId,
        j->header.gatewayId,
        j->header.schemaVersion,
        j->header.messageType,
        /* payload (top) */
        j->payload.deviceId,
        j->payload.timestamp,
        j->payload.uptime_ms,
        j->payload.proto_ver,
        /* environment */
        j->payload.environment.temperature_c,
        j->payload.environment.humidity_percent,
        j->payload.environment.pressure_hpa,
        j->payload.environment.eco2_ppm,
        j->payload.environment.tvoc_ppb,
        j->payload.environment.aqi,
        j->payload.environment.batt_mV,
        /* spectrum */
        j->payload.spectrum.AS7343_405nm,
        j->payload.spectrum.AS7343_425nm,
        j->payload.spectrum.AS7343_450nm,
        j->payload.spectrum.AS7343_475nm,
        j->payload.spectrum.AS7343_515nm,
        j->payload.spectrum.AS7343_550nm,
        j->payload.spectrum.AS7343_555nm,
        j->payload.spectrum.AS7343_600nm,
        j->payload.spectrum.AS7343_640nm,
        j->payload.spectrum.AS7343_690nm,
        j->payload.spectrum.AS7343_745nm,
        j->payload.spectrum.AS7343_855nm,
        j->payload.spectrum.AS7343_VISIBLE,
        /* signature */
        j->signature.alg,
        j->signature.keyId,
        j->signature.value
    );

    if (ret > 0 && ret < (int)sizeof json_output) {
        printk("%s\n", json_output);
    } else {
        printk("JSON encoding failed or buffer too small.\n");
    }

    /* ensure it flushes promptly over UART/logging backends */
    log_flush();
}
