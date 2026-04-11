/*
 * uart_bridge.c — WiFi Board (ESP32 #2)
 *
 * Receives framed binary packets from the BLE gateway board via UART2
 * (GPIO16 RX), decodes them, encodes JSON, and publishes to Azure IoT Hub.
 *
 * Two frame types:
 *
 *   Sensor:  [ 0xAA ] [ 0x00 ] [ 0x3D ] [ 61 bytes  ]
 *   Sound   [ 0xBB ][ len_hi ][ len_lo ][ 698 bytes ]
 *
 * Length is big-endian uint16:
 *   Sensor = 57  (0x00 0x39)
 *   Sound  = 698 (0x02 0xBA)
 *
 * Sensor packet → messageType "telemetry"       (~1.5 KB JSON)
 * Sound  packet → messageType "sound_spectrum"  (~4 KB JSON)
 *
 * Sensor binary layout (57 bytes):
 *   [0..3]   time           uint32 BE
 *   [4..7]   uptime_ms      uint32 BE
 *   [8]      proto_ver
 *   [9]      dev_id
 *   [10..11] temp_c_x100    int16 BE
 *   [12..13] rh_x100        int16 BE
 *   [14..17] press_hPa_x1000 int32 BE
 *   [18..19] eco2_ppm       uint16 BE
 *   [20..21] tvoc_ppb       uint16 BE
 *   [22]     aqi
 *   [23..48] AS7343 ×13     uint16 BE each (26 bytes)
 *   [49..50] batt_mV        uint16 BE
 *   [51..52] snd_rms_dbfs_x100 int16 BE
 *   [53..54] snd_peak_freq_hz  uint16 BE
 *   [55..56] snd_peak_mag_x10  uint16 BE
 *   [57..58] soil_vwc_x100     uint16 BE  — VWC % × 100
 *
 * Sound binary layout (698 bytes):
 *   [0..1]   rms_dbfs_x100  int16 BE
 *   [2..697] bins[348]      uint16 BE each, magnitude × 10
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "uart_bridge.h"
#include "azure_mqtt.h"
#include "my_json.h"

LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

#define UART_DEVICE_NODE  DT_NODELABEL(uart2)

/* ── Frame constants ────────────────────────────────────── */
#define MAGIC_SENSOR       0xAA
#define MAGIC_SOUND        0xBB

#define SENSOR_PAYLOAD_LEN  61    /* +2 bytes for soil_vwc_x100 */
#define SOUND_PAYLOAD_LEN   698   /* 2 (rms) + 348×2 (bins) */
#define MAX_PAYLOAD_LEN     SOUND_PAYLOAD_LEN

/*
 * Ring buffer must comfortably hold the largest frame in flight.
 * Largest frame = 3 header bytes + 698 payload = 701 bytes.
 * Use 1024 for safe headroom.
 */
#define RING_BUF_SIZE   1024
#define UART_STACK_SIZE 4096   /* larger: sound JSON encode needs ~4 KB stack headroom */
#define UART_PRIO       8
/* ── UTC TX frame ───────────────────────────────────────── */
#define FRAME_MAGIC_UTC  0xCC

static const struct device *uart_dev;

/* ── Interrupt-driven ring buffer ───────────────────────── */
static uint8_t  rx_ring[RING_BUF_SIZE];
static volatile int rx_head = 0;
static volatile int rx_tail = 0;

static void uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        if (uart_fifo_read(dev, &byte, 1) != 1) break;
        int next = (rx_head + 1) % RING_BUF_SIZE;
        if (next != rx_tail) {
            rx_ring[rx_head] = byte;
            rx_head = next;
        } else {
            LOG_WRN("Ring buffer full — dropping byte");
        }
    }
}

static inline int ring_available(void)
{
    return (rx_head - rx_tail + RING_BUF_SIZE) % RING_BUF_SIZE;
}

static inline uint8_t ring_read(void)
{
    uint8_t b = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RING_BUF_SIZE;
    return b;
}

/* ── Sensor decode: 61 bytes → tracker_payload_t ────────── */
static void decode_sensor(const uint8_t *d, tracker_payload_t *p)
{
    size_t i = 0;

    p->time = ((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
              ((uint32_t)d[i+2]<<8)|d[i+3];       i += 4;
    p->time_ms = ((uint16_t)d[i]<<8)|d[i+1];      i += 2;
    p->uptime_ms = ((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                   ((uint32_t)d[i+2]<<8)|d[i+3];  i += 4;

    p->proto_ver = d[i++];
    p->dev_id    = d[i++];

    p->temp_c_x100     = (int16_t)((d[i]<<8)|d[i+1]);  i += 2;
    p->rh_x100         = (int16_t)((d[i]<<8)|d[i+1]);  i += 2;
    p->press_hPa_x1000 = (int32_t)(((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                                    ((uint32_t)d[i+2]<<8)|d[i+3]); i += 4;
    p->eco2_ppm = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->tvoc_ppb = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->aqi      = d[i++];

    for (int k = 0; k < AS7343_NUM_CH; k++) {
        p->as7343[k] = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    }

    p->batt_mV           = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->snd_rms_dbfs_x100 = (int16_t) ((d[i]<<8)|d[i+1]); i += 2;
    p->snd_peak_freq_hz  = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->snd_peak_mag_x10  = (uint16_t)((d[i]<<8)|d[i+1]); i += 2;
    p->soil_vwc_x100     = (uint16_t)((d[i]<<8)|d[i+1]);
}

/* ── Sound decode: 698 bytes → sound_payload_t ──────────── */
static void decode_sound(const uint8_t *d, sound_payload_t *sp)
{
    sp->rms_dbfs_x100 = (int16_t)(((uint16_t)d[0] << 8) | d[1]);
    for (int i = 0; i < SOUND_NUM_BINS; i++) {
        sp->bins[i] = (uint16_t)(((uint16_t)d[2 + i*2] << 8) | d[2 + i*2 + 1]);
    }
    /* dev_id and uptime_ms filled by caller */
}

/* ── State machine ──────────────────────────────────────── */
typedef enum {
    STATE_HUNT,      /* scanning for 0xAA or 0xBB     */
    STATE_LEN_HI,    /* high byte of 16-bit length     */
    STATE_LEN_LO,    /* low byte  of 16-bit length     */
    STATE_PAYLOAD,   /* accumulating payload bytes     */
} frame_state_t;

/*
 * Static payload accumulation buffer — 698 bytes.
 * Lives in BSS, never on the thread stack.
 */
static uint8_t s_payload[MAX_PAYLOAD_LEN];

K_THREAD_STACK_DEFINE(uart_bridge_stack, UART_STACK_SIZE);
static struct k_thread uart_bridge_tid;

static void uart_bridge_thread(void)
{
    LOG_INF("UART bridge started (UART2 GPIO16 RX)");

    frame_state_t state        = STATE_HUNT;
    uint8_t       frame_magic  = 0;
    uint16_t      expected_len = 0;
    uint16_t      payload_idx  = 0;

    /*
     * All working buffers are static — the sound JSON is ~4 KB which
     * would overflow a 4 KB thread stack if placed on it.
     */
    static tracker_payload_t   sensor_pkt;
    static sound_payload_t     sound_pkt;
    static struct json_full_packet jp;
    static char sensor_json[JSON_BUFFER_SIZE];
    static char sound_json[SOUND_JSON_BUFFER_SIZE];

    while (true) {
        if (ring_available() == 0) {
            k_sleep(K_MSEC(2));
            continue;
        }

        uint8_t b = ring_read();

        switch (state) {

        /* ── Hunt for a known magic byte ── */
        case STATE_HUNT:
            if (b == MAGIC_SENSOR || b == MAGIC_SOUND) {
                frame_magic = b;
                state       = STATE_LEN_HI;
            }
            break;

        /* ── High byte of payload length ── */
        case STATE_LEN_HI:
            expected_len = (uint16_t)b << 8;
            state        = STATE_LEN_LO;
            break;

        /* ── Low byte of payload length — validate ── */
        case STATE_LEN_LO:
            expected_len |= b;

            if (frame_magic == MAGIC_SENSOR && expected_len == SENSOR_PAYLOAD_LEN) {
                payload_idx = 0;
                state       = STATE_PAYLOAD;
            } else if (frame_magic == MAGIC_SOUND && expected_len == SOUND_PAYLOAD_LEN) {
                payload_idx = 0;
                state       = STATE_PAYLOAD;
            } else {
                LOG_WRN("Bad frame: magic=0x%02x len=%u — re-hunting",
                        frame_magic, expected_len);
                state = STATE_HUNT;
            }
            break;

        /* ── Accumulate payload bytes ── */
        case STATE_PAYLOAD:
            s_payload[payload_idx++] = b;

            if (payload_idx < expected_len) {
                break;  /* still collecting */
            }

            /* ════ Complete frame received ════ */

            if (frame_magic == MAGIC_SENSOR) {
                /* Decode → fill JSON structs → encode → publish */
                memset(&sensor_pkt, 0, sizeof(sensor_pkt));
                memset(&jp,         0, sizeof(jp));

                decode_sensor(s_payload, &sensor_pkt);
                fill_json_packet(&sensor_pkt, &jp);

                int ret = snprintk(sensor_json, sizeof(sensor_json),
                    JSON_FORMAT,
                    jp.header.messageId,
                    jp.header.gatewayId,
                    jp.header.schemaVersion,
                    jp.header.messageType,
                    jp.payload.deviceId,
                    jp.payload.timestamp,
                    jp.payload.uptime_ms,
                    jp.payload.proto_ver,
                    jp.payload.environment.temperature_c,
                    jp.payload.environment.humidity_percent,
                    jp.payload.environment.pressure_hpa,
                    jp.payload.environment.eco2_ppm,
                    jp.payload.environment.tvoc_ppb,
                    jp.payload.environment.aqi,
                    jp.payload.environment.batt_mV,
                    jp.payload.spectrum.AS7343_405nm,
                    jp.payload.spectrum.AS7343_425nm,
                    jp.payload.spectrum.AS7343_450nm,
                    jp.payload.spectrum.AS7343_475nm,
                    jp.payload.spectrum.AS7343_515nm,
                    jp.payload.spectrum.AS7343_550nm,
                    jp.payload.spectrum.AS7343_555nm,
                    jp.payload.spectrum.AS7343_600nm,
                    jp.payload.spectrum.AS7343_640nm,
                    jp.payload.spectrum.AS7343_690nm,
                    jp.payload.spectrum.AS7343_745nm,
                    jp.payload.spectrum.AS7343_855nm,
                    jp.payload.spectrum.AS7343_VISIBLE,
                    jp.payload.sound.rms_dbfs,
                    jp.payload.sound.peak_freq_hz,
                    jp.payload.sound.peak_mag,
                    jp.payload.soil_vwc
                );

                if (ret > 0 && ret < (int)sizeof(sensor_json)) {
                    int pub = azure_mqtt_publish(sensor_json);
                    if (pub < 0) LOG_WRN("Sensor publish failed (%d)", pub);
                    else         LOG_INF("Sensor published dev-%u (%d bytes)",
                                         sensor_pkt.dev_id, ret);
                } else {
                    LOG_ERR("Sensor JSON encode failed (ret=%d)", ret);
                }

            } else { /* MAGIC_SOUND */
                memset(&sound_pkt, 0, sizeof(sound_pkt));

                decode_sound(s_payload, &sound_pkt);
                sound_pkt.uptime_ms = sntp_get_last_utc(NULL);
                /* dev_id unknown at this layer — gateway has no per-conn context */
                sound_pkt.dev_id = 0;

                int ret = encode_sound_json(&sound_pkt,
                                            sound_json, sizeof(sound_json));
                if (ret > 0) {
                    // int pub = azure_mqtt_publish(sound_json);
                    // if (pub < 0) LOG_WRN("Sound publish failed (%d)", pub);
                    // else         LOG_INF("Sound spectrum published (%d bytes JSON)", ret);
                    static uint8_t sound_pkt_count = 0;
                    if (++sound_pkt_count >= 10) {
                        sound_pkt_count = 0;
                        int pub = azure_mqtt_publish(sound_json);
                        if (pub < 0) LOG_WRN("Sound publish failed (%d)", pub);
                        else         LOG_INF("Sound spectrum published (%d bytes JSON)", ret);
                    } else {
                        LOG_DBG("Sound spectrum skipped (%u/10)", sound_pkt_count);
                    }
                } else {
                    LOG_ERR("Sound JSON encode failed (%d)", ret);
                }
            }

            state = STATE_HUNT;
            break;
        }
    }
}

/* ── Public API ─────────────────────────────────────────── */
void uart_bridge_start(void)
{
    uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART2 not ready — check overlay");
        return;
    }

    uart_irq_callback_set(uart_dev, uart_isr);
    uart_irq_rx_enable(uart_dev);

    k_thread_create(&uart_bridge_tid,
                    uart_bridge_stack,
                    K_THREAD_STACK_SIZEOF(uart_bridge_stack),
                    (k_thread_entry_t)uart_bridge_thread,
                    NULL, NULL, NULL,
                    UART_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&uart_bridge_tid, "uart_bridge");

    LOG_INF("UART bridge started");
}

void uart_bridge_send_utc(uint32_t utc_time, uint16_t utc_ms)
{
    if (!uart_dev || !device_is_ready(uart_dev)) {
        LOG_WRN("uart_bridge_send_utc: UART not ready");
        return;
    }

    uint8_t frame[7] = {
        FRAME_MAGIC_UTC,
        (uint8_t)(utc_time >> 24),
        (uint8_t)(utc_time >> 16),
        (uint8_t)(utc_time >>  8),
        (uint8_t)(utc_time       ),
        (uint8_t)(utc_ms   >>  8),
        (uint8_t)(utc_ms         ),
    };

    for (int i = 0; i < (int)sizeof(frame); i++) {
        uart_poll_out(uart_dev, frame[i]);
    }

    LOG_INF("UTC frame sent: %" PRIu32 ".%03u", utc_time, utc_ms);
}