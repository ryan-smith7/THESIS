/**
 * @file sd_log.h
 * @brief SD card offline logging for sensor node modalities.
 *
 * Two write targets per modality:
 *
 *   Boot archive file  (boot-numbered, never deleted)
 *     e.g. /SD/BME0003.BIN
 *     Written on every sample that falls into the SD path (see routing in
 *     ble_threads.c). Contains both utc_sec and uptime_ms so pre-sync
 *     records can be retroactively aligned to UTC once a sync arrives.
 *
 *   UTC upload file  (constant name, deleted after each BLE drain)
 *     e.g. /SD/bme280.bin
 *     Written only for UTC-valid records while offline (or always if
 *     CONFIG_SD_LOG_ALWAYS_WRITE=y). Replayed over BLE on reconnect.
 *
 * Routing decisions live entirely in bme_ble_thread / ens_ble_thread etc.
 * (ble_threads.c). sd_log_write_boot() and sd_log_write_utc() are pure
 * write primitives — they do not inspect message contents.
 *
 * UTC upload file layout (binary — fixed-size struct records):
 *   Node 1:
 *     /SD/bme280.bin            bme280_msg
 *     /SD/ens160.bin            ens160_msg
 *     /SD/sound.bin             sound_spec_msg
 *   Node 2:
 *     /SD/as7343.bin            as7343_msg
 *     /SD/moisture.bin          moisture_msg
 *
 * Boot archive layout:
 *   /SD/BMEnnnn.BIN   bme280_msg       (node 1, boot nnnn)
 *   /SD/ENSnnnn.BIN   ens160_msg       (node 1, boot nnnn)
 *   /SD/SNDnnnn.BIN   sound_spec_msg   (node 1, boot nnnn)
 *   /SD/AS7nnnn.BIN   as7343_msg       (node 2, boot nnnn)
 *   /SD/MSTnnnn.BIN   moisture_msg     (node 2, boot nnnn)
 *
 * Each record in every file is written as:
 *   [ struct bytes (N) ][ CRC32 little-endian (4) ]
 * CRC covers the struct payload only. f_sync() is called after every write.
 * UTC upload files are tail-healed at init to remove any partial record
 * left by a power-loss mid-write.
 */

#ifndef SD_LOG_H
#define SD_LOG_H

#include <stddef.h>
#include <zephyr/kernel.h>
#include "sensor.h"

/* ── UTC validity threshold ──────────────────────────────────────────────── */
#define SD_LOG_UTC_MIN  1700000000U

/* ── UTC upload file paths (constant names, deleted after drain) ─────────── */
#define SD_LOG_BME280    "/SD/bme280.bin"
#define SD_LOG_ENS160    "/SD/ens160.bin"
#define SD_LOG_AS7343    "/SD/as7343.bin"
#define SD_LOG_MOISTURE  "/SD/moisture.bin"
#define SD_LOG_SOUND     "/SD/sound.bin"

/* ── Boot counter file ───────────────────────────────────────────────────── */
#define SD_LOG_BOOTCOUNT "/SD/boot.txt"

/* ── Always-write config ─────────────────────────────────────────────────── *
 * Set in prj.conf:
 *   CONFIG_SD_LOG_ALWAYS_WRITE=y
 *     Boot archive written even while BLE is connected and UTC is valid.
 *     UTC upload file also written while connected (bench / always-on use).
 *   CONFIG_SD_LOG_ALWAYS_WRITE=n  (default)
 *     Boot archive written only when offline or UTC not yet synced.
 *     UTC upload file written only while offline.
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── Drain semaphore ─────────────────────────────────────────────────────── */
extern struct k_sem sd_drain_sem;

// extern struct k_sem sd_drain_bme;
// extern struct k_sem sd_drain_ens;
// extern struct k_sem sd_drain_snd;
// extern struct k_sem sd_drain_spec;
// extern struct k_sem sd_drain_mst;

/* ── Core API ────────────────────────────────────────────────────────────── */

int  sd_log_init(void);
bool sd_log_is_ready(void);
bool sd_log_is_draining(void);
void sd_log_set_draining(bool draining);
void sd_drain_thread(void);

/* ── Generic write primitives ────────────────────────────────────────────── *
 * Do not call these directly. Use SD_LOG_BOOT() and SD_LOG_UTC() below,
 * which derive sizeof from the pointer type automatically.
 * ─────────────────────────────────────────────────────────────────────────── */
void sd_log_write_boot(const char *path, const void *msg, size_t len);
void sd_log_write_utc (const char *path, const void *msg, size_t len);

/* ── Typed callsite macros ───────────────────────────────────────────────── *
 * sizeof(*(msg_ptr)) is resolved at compile time from the pointer type,
 * so the size passed to the write function is always correct for the struct.
 *
 * Usage:
 *   SD_LOG_BOOT(sd_log_boot_path_bme(), &msg);
 *   SD_LOG_UTC (SD_LOG_BME280,          &msg);
 * ─────────────────────────────────────────────────────────────────────────── */
#define SD_LOG_BOOT(path, msg_ptr) \
    sd_log_write_boot((path), (msg_ptr), sizeof(*(msg_ptr)))

#define SD_LOG_UTC(path, msg_ptr) \
    sd_log_write_utc((path), (msg_ptr), sizeof(*(msg_ptr)))

/* ── Boot archive path accessors ─────────────────────────────────────────── *
 * Returns the boot-numbered path built once at sd_log_init().
 * Pass the result directly to SD_LOG_BOOT().
 * ─────────────────────────────────────────────────────────────────────────── */
// #if defined(CONFIG_SENSOR_NODE_1)
const char *sd_log_boot_path_bme(void);
const char *sd_log_boot_path_ens(void);
const char *sd_log_boot_path_snd(void);
// #elif defined(CONFIG_SENSOR_NODE_2)
const char *sd_log_boot_path_as7(void);
const char *sd_log_boot_path_mst(void);
// #endif

#endif /* SD_LOG_H */