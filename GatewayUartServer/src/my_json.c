#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/uuid.h>
#include "my_json.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

LOG_MODULE_REGISTER(json_module);

/* ── UUID helper ────────────────────────────────────────── */
static void generate_uuid(char *uuid_str)
{
    struct uuid u;
    if (uuid_generate_v4(&u) != 0) { uuid_str[0] = '\0'; return; }
    if (uuid_to_string(&u, uuid_str) != 0) { uuid_str[0] = '\0'; }
}

/* ── fill_json_packet ───────────────────────────────────── */
void fill_json_packet(const tracker_payload_t *p, struct json_full_packet *j)
{
    if (!p || !j) return;
    memset(j, 0, sizeof(*j));

    /* Header */
    char uuid_str[UUID_STR_LEN] = {0};
    generate_uuid(uuid_str);
    snprintk(j->header.messageId, sizeof(j->header.messageId),
             "%s", uuid_str[0] ? uuid_str : "00000000-0000-0000-0000-000000000000");
    snprintk(j->header.gatewayId,     sizeof(j->header.gatewayId),     "GW-01");
    snprintk(j->header.schemaVersion, sizeof(j->header.schemaVersion), "1");
    snprintk(j->header.messageType,   sizeof(j->header.messageType),   "telemetry");

    /* Payload — top level */
    snprintk(j->payload.deviceId, sizeof(j->payload.deviceId), "dev-%u", p->dev_id);

    time_t raw = (time_t)p->time;
    struct tm tm_utc;
    if (gmtime_r(&raw, &tm_utc) == NULL) {
        snprintk(j->payload.timestamp, sizeof(j->payload.timestamp), "%u", p->time);
    } else {
        snprintk(j->payload.timestamp, sizeof(j->payload.timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, p->time_ms);
    }
    j->payload.uptime_ms = p->uptime_ms;
    j->payload.proto_ver = p->proto_ver;

    /* Environment */
    j->payload.environment.temperature_c    = (double)p->temp_c_x100    / 100.0;
    j->payload.environment.humidity_percent = (double)p->rh_x100        / 100.0;
    j->payload.environment.pressure_hpa     = (double)p->press_hPa_x1000 / 1000.0;
    j->payload.environment.eco2_ppm = p->eco2_ppm;
    j->payload.environment.tvoc_ppb = p->tvoc_ppb;
    j->payload.environment.aqi      = p->aqi;
    j->payload.environment.batt_mV  = p->batt_mV;

    /* AS7343 light spectrum */
    j->payload.spectrum.AS7343_405nm   = p->as7343[0];
    j->payload.spectrum.AS7343_425nm   = p->as7343[1];
    j->payload.spectrum.AS7343_450nm   = p->as7343[2];
    j->payload.spectrum.AS7343_475nm   = p->as7343[3];
    j->payload.spectrum.AS7343_515nm   = p->as7343[4];
    j->payload.spectrum.AS7343_550nm   = p->as7343[5];
    j->payload.spectrum.AS7343_555nm   = p->as7343[6];
    j->payload.spectrum.AS7343_600nm   = p->as7343[7];
    j->payload.spectrum.AS7343_640nm   = p->as7343[8];
    j->payload.spectrum.AS7343_690nm   = p->as7343[9];
    j->payload.spectrum.AS7343_745nm   = p->as7343[10];
    j->payload.spectrum.AS7343_855nm   = p->as7343[11];
    j->payload.spectrum.AS7343_VISIBLE = p->as7343[12];

    /* Sound summary */
    j->payload.sound.rms_dbfs     = (double)p->snd_rms_dbfs_x100 / 100.0;
    j->payload.sound.peak_freq_hz = p->snd_peak_freq_hz;
    j->payload.sound.peak_mag     = (double)p->snd_peak_mag_x10  / 10.0;

    /* Soil moisture */
    j->payload.soil_vwc = (double)p->soil_vwc_x100 / 100.0;
}

/* ── encode_and_print_json ──────────────────────────────── */
void encode_and_print_json(const struct json_full_packet *j)
{
    static char json_output[JSON_BUFFER_SIZE];

    int ret = snprintk(json_output, sizeof(json_output),
        JSON_FORMAT,
        j->header.messageId,
        j->header.gatewayId,
        j->header.schemaVersion,
        j->header.messageType,
        j->payload.deviceId,
        j->payload.timestamp,
        j->payload.uptime_ms,
        j->payload.proto_ver,
        j->payload.environment.temperature_c,
        j->payload.environment.humidity_percent,
        j->payload.environment.pressure_hpa,
        j->payload.environment.eco2_ppm,
        j->payload.environment.tvoc_ppb,
        j->payload.environment.aqi,
        j->payload.environment.batt_mV,
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
        j->payload.sound.rms_dbfs,
        j->payload.sound.peak_freq_hz,
        j->payload.sound.peak_mag,
        j->payload.soil_vwc
    );

    if (ret > 0 && ret < (int)sizeof(json_output)) {
        printk("%s\n", json_output);
    } else {
        printk("JSON encoding failed (ret=%d)\n", ret);
    }
    log_flush();
}

/* ── encode_sound_json ──────────────────────────────────── */
int encode_sound_json(const sound_payload_t *sp, char *buf, size_t buf_size)
{
    if (!sp || !buf) return -EINVAL;

    char uuid_str[UUID_STR_LEN] = {0};
    generate_uuid(uuid_str);

    /* Fixed header section */
    int pos = snprintk(buf, buf_size,
        "{"
          "\"header\":{"
            "\"messageId\":\"%s\","
            "\"gatewayId\":\"GW-01\","
            "\"schemaVersion\":\"1\","
            "\"messageType\":\"sound_spectrum\""
          "},"
          "\"payload\":{"
            "\"deviceId\":\"dev-%u\","
            "\"uptime_ms\":%u,"
            "\"rms_dbfs\":%.2f,"
            "\"bin_low_hz\":43,"
            "\"bin_res_hz\":43,"
            "\"bins\":[",
        uuid_str[0] ? uuid_str : "00000000-0000-0000-0000-000000000000",
        (unsigned)sp->dev_id,
        (unsigned)sp->uptime_ms,
        (double)sp->rms_dbfs_x100 / 100.0
    );

    if (pos <= 0 || (size_t)pos >= buf_size) return -ENOMEM;

    /* 348 bin values — built iteratively to avoid a giant format string */
    for (int i = 0; i < SOUND_NUM_BINS; i++) {
        double mag = (double)sp->bins[i] / 10.0;
        int w = snprintk(buf + pos, buf_size - pos,
                         (i < SOUND_NUM_BINS - 1) ? "%.1f," : "%.1f", mag);
        if (w <= 0 || (size_t)(pos + w) >= buf_size) {
            LOG_ERR("Sound JSON buffer overflow at bin %d", i);
            return -ENOMEM;
        }
        pos += w;
    }

    /* Close */
    int tail = snprintk(buf + pos, buf_size - pos, "]}}");
    if (tail <= 0) return -ENOMEM;
    pos += tail;

    return pos;
}