/**
 * @file sd_log.h
 * @brief SD card offline logging for sensor node modalities.
 *
 * File routing:
 *   utc_sec > SD_LOG_UTC_MIN  →  primary .bin (constant name, replayed on reconnect)
 *   utc_sec <= SD_LOG_UTC_MIN →  _upt_NNNN.bin (boot-numbered, offline analysis only)
 *
 * The boot counter is stored in /SD/bootcount.txt and incremented once per
 * power cycle at sd_log_init(). Uptime filenames take the form:
 *   bme280_upt_0001.bin, bme280_upt_0002.bin, ...
 * so each boot produces a distinct set of files that never overwrite each other.
 *
 * File layout (all binary — fixed-size struct records):
 *   Node 1:
 *     /SD/bme280.bin            bme280_msg    (valid UTC — replayed on reconnect)
 *     /SD/bme280_upt_NNNN.bin   bme280_msg    (uptime — offline only)
 *     /SD/ens160.bin            ens160_msg    (valid UTC)
 *     /SD/ens160_upt_NNNN.bin   ens160_msg    (uptime)
 *     /SD/sound.bin             sound_spec_msg (valid UTC)
 *     /SD/sound_upt_NNNN.bin    sound_spec_msg (uptime)
 *   Node 2:
 *     /SD/as7343.bin            as7343_msg    (valid UTC)
 *     /SD/as7343_upt_NNNN.bin   as7343_msg    (uptime)
 *     /SD/moisture.bin          moisture_msg  (valid UTC)
 *     /SD/moisture_upt_NNNN.bin moisture_msg  (uptime)
 */

#ifndef SD_LOG_H
#define SD_LOG_H

#include <zephyr/kernel.h>
#include "sensor.h"

/* ── UTC validity threshold ──────────────────────────────────────────────── */
#define SD_LOG_UTC_MIN  1700000000U

/* ── Primary file paths (valid UTC — constant names) ─────────────────────── */
#define SD_LOG_BME280    "/SD/bme280.bin"
#define SD_LOG_ENS160    "/SD/ens160.bin"
#define SD_LOG_AS7343    "/SD/as7343.bin"
#define SD_LOG_MOISTURE  "/SD/moisture.bin"
#define SD_LOG_SOUND     "/SD/sound.bin"

/* ── Boot counter file ───────────────────────────────────────────────────── */
#define SD_LOG_BOOTCOUNT "/SD/bootcount.txt"

/* ── Drain semaphore ─────────────────────────────────────────────────────── */
extern struct k_sem sd_drain_sem;

/* ── Public API ──────────────────────────────────────────────────────────── */

int  sd_log_init(void);
bool sd_log_is_ready(void);
bool sd_log_is_draining(void);
void sd_log_set_draining(bool draining);

/* ── Per-modality log functions ──────────────────────────────────────────── */

#if defined(CONFIG_SENSOR_NODE_1)

#include "sound.h"

void sd_log_bme(const struct bme280_msg *msg);
void sd_log_ens(const struct ens160_msg *msg);
void sd_log_snd(const struct sound_spec_msg *spec);

#elif defined(CONFIG_SENSOR_NODE_2)

void sd_log_as7(const struct as7343_msg *msg);
void sd_log_mst(const struct moisture_msg *msg);

#endif

/* ── Drain thread ────────────────────────────────────────────────────────── */
void sd_drain_thread(void);

#endif /* SD_LOG_H */