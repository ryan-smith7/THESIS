/**
 * @file sd_log.c
 * @brief SD card offline logging — binary format with dual-file targets.
 *
 * Two write targets per modality, selected explicitly by the caller:
 *
 *   sd_log_X_boot()   Boot archive file (e.g. /SD/BME0003.BIN)
 *     Written on every sample.  Never deleted.  Contains both uptime_ms
 *     and utc_sec so pre-sync records can be retroactively aligned to UTC.
 *
 *   sd_log_X_utc()    UTC upload file (e.g. SD_LOG_BME280)
 *     Written only for UTC-valid records while offline (or always, if
 *     CONFIG_SD_LOG_ALWAYS_WRITE=y).  Replayed over BLE then deleted.
 *
 * All routing decisions live in bme_ble_thread (and peer threads):
 *
 *   Connected + subscribed + UTC valid  →  BLE notify + boot file
 *   Connected + subscribed, no UTC      →  boot file only
 *   Not connected, UTC valid            →  boot file + UTC upload file
 *   Not connected, no UTC              →  boot file only
 *   CONFIG_SD_LOG_ALWAYS_WRITE=y        →  boot file + UTC file always
 *
 * Boot counter is read from /SD/bootcount.txt at init, incremented, and
 * written back.  UTC upload files are tail-healed at init to remove any
 * partial record left by a previous power-loss mid-write.
 *
 * DRAIN DESIGN
 * ------------
 * Each drain function reads from the END of the UTC file (most recent first),
 * sends the record over BLE, then immediately truncates that record off the
 * end of the file. f_sync() after each truncate makes it durable — a power
 * cycle between sends leaves the file containing only unsent records.
 *
 * On disconnect mid-drain the file contains exactly the unsent records.
 * On reconnect drain resumes from the new end — no position tracking needed,
 * no re-sends, no duplicates.
 *
 * Most-recent-first ordering means the gateway receives the freshest data
 * immediately after reconnect, backfilling older records afterwards.
 *
 * RAM saving vs old static buffers:
 *   snd_drain_rows[32]: 712x32 = 22,784 bytes  ->  712 bytes on drain stack
 *   bme_drain_rows[32]:  40x32 =  1,280 bytes  ->   40 bytes on drain stack
 *   ens_drain_rows[32]:  32x32 =  1,024 bytes  ->   32 bytes on drain stack
 *   Total saved: ~25 KB static DRAM
 */

#include "sd_log.h"
#include "file.h"
#include "bme_ble.h"
#include "ens_ble.h"
#include "as7_ble.h"
#include "mst_ble.h"
#include "ds18b20_ble.h"

// #if defined(CONFIG_SENSOR_NODE_1)
#include "sound.h"
#include "sound_ble.h"
// #endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

LOG_MODULE_REGISTER(sd_log, LOG_LEVEL_INF);

#define BOOT_PATH_MAX    24   /* e.g. "/SD/BME0003.BIN" = 16 chars, headroom */

/* ── Drain semaphore — triggered by connected() callback ─────────────────── */
K_SEM_DEFINE(sd_drain_sem, 0, 1);

/* ── SD state ────────────────────────────────────────────────────────────── */
static bool sd_ready    = false;
static bool sd_draining = false;

/* ── SD access mutex ─────────────────────────────────────────────────────── */
static K_MUTEX_DEFINE(sd_mutex);

/* ── Boot-numbered archive file paths — built once at init ──────────────── */
// #if defined(CONFIG_SENSOR_NODE_1)
static char boot_bme[BOOT_PATH_MAX];
static char boot_ens[BOOT_PATH_MAX];
static char boot_snd[BOOT_PATH_MAX];
// #elif defined(CONFIG_SENSOR_NODE_2)
static char boot_as7[BOOT_PATH_MAX];
static char boot_mst[BOOT_PATH_MAX];
static char boot_ds18b20[BOOT_PATH_MAX];
// #endif

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC STATE ACCESSORS
 * ═══════════════════════════════════════════════════════════════════════════ */

bool sd_log_is_ready(void) {
    return sd_ready;
}

bool sd_log_is_draining(void) {
    return sd_draining;
}

void sd_log_set_draining(bool draining) {
    sd_draining = draining;
    /* No k_event — BLE threads are never blocked during drain.
     * Live data flows alongside drain records. */
}

/* ── Boot counter ────────────────────────────────────────────────────────── */
static uint32_t read_and_increment_boot_count(void) {
    char fatpath[MAX_PATH_LEN];
    char buf[16];
    uint32_t count = 0;

    to_fatfs_path(SD_LOG_BOOTCOUNT, fatpath, sizeof(fatpath));

    FIL fil;

    if (f_open(&fil, fatpath, FA_READ) == FR_OK) {
        UINT br;
        memset(buf, 0, sizeof(buf));
        if (f_read(&fil, buf, sizeof(buf) - 1, &br) == FR_OK && br > 0) {
            count = (uint32_t)strtoul(buf, NULL, 10);
            LOG_INF("bootcount: read value %u", (unsigned)count);
        }
        f_close(&fil);
    }

    count++;

    FRESULT fr = f_open(&fil, fatpath, FA_CREATE_NEW | FA_WRITE);
    LOG_INF("bootcount: write open → %d (writing count=%u)", fr, (unsigned)count);
    if (fr == FR_EXIST) {
        f_open(&fil, fatpath, FA_WRITE);
        f_truncate(&fil);
    }

    UINT bw;
    snprintf(buf, sizeof(buf), "%u\n", (unsigned)count);
    f_write(&fil, buf, strlen(buf), &bw);
    f_sync(&fil);
    f_close(&fil);

    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UTC FILE TAIL HEAL
 *
 * Scans a UTC upload file and truncates it to the last byte offset at which
 * a complete, CRC-valid record ends.  Removes any partial record left by a
 * power-loss mid-write so that subsequent appends start from clean ground.
 *
 * Called at init for every UTC file that exists on the card.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void heal_utc_file_tail(const char *zpath, size_t record_len)
{
    char fatpath[MAX_PATH_LEN];
    to_fatfs_path(zpath, fatpath, sizeof(fatpath));

    /* Skip if file doesn't exist yet */
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        return;
    }

    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        LOG_WRN("Tail heal: could not open %s", fatpath);
        return;
    }

    /*
     * Walk records, tracking the file offset after each successful one.
     * record_len + 4 = payload + CRC32 trailer.
     */
    size_t  stride        = record_len + 4;
    FSIZE_t last_good_end = 0;
    FSIZE_t pos           = 0;
    uint8_t scratch[128]; /* large enough for any current msg struct + 4 */

    /* Guard: stride must fit in scratch */
    if (stride > sizeof(scratch)) {
        LOG_ERR("Tail heal: record stride %u > scratch %u — skipping %s",
                (unsigned)stride, (unsigned)sizeof(scratch), fatpath);
        f_close(&fil);
        return;
    }

    while (1) {
        UINT br;
        int rc = read_raw_verify_crc(&fil, scratch, record_len, &br);
        if (rc == -ENODATA) {
            break; /* clean EOF */
        }
        if (rc != 0) {
            /* Corrupt or truncated record — truncate file here */
            LOG_WRN("Tail heal %s: truncating at offset %llu (removed partial record)",
                    fatpath, (unsigned long long)last_good_end);
            f_lseek(&fil, last_good_end);
            f_truncate(&fil);
            break;
        }
        pos += stride;
        last_good_end = pos;
    }

    f_close(&fil);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * INIT
 * ═══════════════════════════════════════════════════════════════════════════ */
int sd_log_init(void) {

    int rc = fatfs_mount();
    if (rc < 0) {
        LOG_ERR("SD log: mount failed (%d)", rc);
        return rc;
    }
    sd_ready = true;

    uint32_t boot = read_and_increment_boot_count();
    LOG_INF("SD log: boot #%u", (unsigned)boot);

// #if defined(CONFIG_SENSOR_NODE_1)
    snprintf(boot_bme, sizeof(boot_bme), "/SD/BME%04u.BIN", (unsigned)boot);
    snprintf(boot_ens, sizeof(boot_ens), "/SD/ENS%04u.BIN", (unsigned)boot);
    snprintf(boot_snd, sizeof(boot_snd), "/SD/SND%04u.BIN", (unsigned)boot);

    /* Heal any partial tail left by previous power-loss */
    heal_utc_file_tail(SD_LOG_BME280, sizeof(struct bme280_msg));
    heal_utc_file_tail(SD_LOG_ENS160, sizeof(struct ens160_msg));
    heal_utc_file_tail(SD_LOG_SOUND,  sizeof(struct sound_spec_msg));

// #elif defined(CONFIG_SENSOR_NODE_2)
    snprintf(boot_as7, sizeof(boot_as7), "/SD/AS7%04u.BIN", (unsigned)boot);
    snprintf(boot_mst, sizeof(boot_mst), "/SD/MST%04u.BIN", (unsigned)boot);
    snprintf(boot_ds18b20, sizeof(boot_ds18b20), "/SD/D18%04u.BIN", (unsigned)boot);

    heal_utc_file_tail(SD_LOG_AS7343,  sizeof(struct as7343_msg));
    heal_utc_file_tail(SD_LOG_MOISTURE, sizeof(struct moisture_msg));
    heal_utc_file_tail(SD_LOG_DS18B20, sizeof(struct ds18b20_msg));
// #endif

    LOG_INF("SD log: ready");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BOOT PATH ACCESSORS
 *
 * Boot archive paths are static to this file, built once at init.
 * BLE threads retrieve the correct path via these accessors and pass it
 * directly to SD_LOG_BOOT() / SD_LOG_UTC() at the callsite.
 * ═══════════════════════════════════════════════════════════════════════════ */

// #if defined(CONFIG_SENSOR_NODE_1)
const char *sd_log_boot_path_bme(void) { return boot_bme; }
const char *sd_log_boot_path_ens(void) { return boot_ens; }
const char *sd_log_boot_path_snd(void) { return boot_snd; }
// #elif defined(CONFIG_SENSOR_NODE_2)
const char *sd_log_boot_path_as7(void) { return boot_as7; }
const char *sd_log_boot_path_mst(void) { return boot_mst; }
const char *sd_log_boot_path_ds18b20(void) { return boot_ds18b20; }
// #endif

/* ═══════════════════════════════════════════════════════════════════════════
 * LOG FUNCTIONS — two generic write primitives
 *
 * All routing decisions (connected/disconnected, UTC valid, ALWAYS_WRITE)
 * are made by the caller (bme_ble_thread etc.) before calling these.
 *
 * Use SD_LOG_BOOT(path, msg_ptr) and SD_LOG_UTC(path, msg_ptr) from
 * sd_log.h at callsites — these supply sizeof automatically from the
 * pointer type, preserving type safety without per-modality functions.
 * ═══════════════════════════════════════════════════════════════════════════ */

void sd_log_write(const char *path, const void *msg, size_t len) {
    if (!sd_ready) {
        return;
    }
    k_mutex_lock(&sd_mutex, K_FOREVER);
    int rc = write_raw_to_file(path, (const uint8_t *)msg, len);
    if (rc == -ENOSPC) {
        LOG_ERR("SD full — writes disabled until reboot");
        sd_ready = false;
    }
    k_mutex_unlock(&sd_mutex);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DRAIN HELPERS — UTC upload files only
 *
 * Truncate-from-end pattern — most recent record first, no re-sends.
 *
 * Each function:
 *   1. Opens the UTC file FA_READ | FA_WRITE
 *   2. Seeks to the last record (fsize - stride)
 *   3. Reads and CRC-verifies it
 *   4. Sends over BLE via pack_and_notify (returns false = disconnected)
 *   5. Truncates that record off the end + f_sync (durable before next send)
 *   6. Repeats until file is empty then deletes it
 *
 * On disconnect: file contains exactly the unsent records. Next connection
 * resumes from the new end — no position tracking, no duplicates.
 *
 * Corrupt tail record: truncated off silently, next record tried.
 *
 * Throttling:
 *   drain_snd: 2000ms — gateway JSON serialisation of ~2886 byte packets
 *   drain_as7: 100ms  — ~400 byte packets, cooperative yield only
 *   drain_bme, drain_ens, drain_mst: no throttle — small packets
 * =========================================================================== */

#if defined(CONFIG_SENSOR_NODE_1)

static void drain_bme(void)
{
    k_sem_take(&bme_notify_sem, K_SECONDS(30));
    const size_t stride = sizeof(struct bme280_msg) + 4;
    char fatpath[MAX_PATH_LEN];
    int  count = 0, corrupt = 0;
    struct bme280_msg row;

    to_fatfs_path(SD_LOG_BME280, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    if (fno.fsize % stride != 0) {
        LOG_WRN("SD drain BME: file size not multiple of stride — skipping");
        k_mutex_unlock(&sd_mutex);
        return;
    }
    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    k_mutex_unlock(&sd_mutex);

    while (1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        FSIZE_t fsize = f_size(&fil);
        if (fsize < stride) {
            f_close(&fil);
            f_unlink(fatpath);
            k_mutex_unlock(&sd_mutex);
            break;
        }
        FSIZE_t rec_pos = fsize - stride;
        f_lseek(&fil, rec_pos);
        UINT br;
        int rc = read_raw_verify_crc(&fil, (uint8_t *)&row,
                                     sizeof(struct bme280_msg), &br);
        if (rc != 0) {
            LOG_WRN("SD drain BME: corrupt tail — truncating");
            f_lseek(&fil, rec_pos);
            f_truncate(&fil);
            f_sync(&fil);
            corrupt++;
            k_mutex_unlock(&sd_mutex);
            continue;
        }
        k_mutex_unlock(&sd_mutex);

        if (!bme_pack_and_notify(&row)) {
            LOG_WRN("SD drain BME: disconnected at record %d — %d remain in file",
                    count, (int)(rec_pos / stride));
            k_mutex_lock(&sd_mutex, K_FOREVER);
            f_close(&fil);
            k_mutex_unlock(&sd_mutex);
            return;
        }
        count++;

        k_mutex_lock(&sd_mutex, K_FOREVER);
        f_lseek(&fil, rec_pos);
        f_truncate(&fil);
        f_sync(&fil);
        k_mutex_unlock(&sd_mutex);
    }

    LOG_INF("SD drain BME: %d OK, %d corrupt", count, corrupt);
}

static void drain_ens(void)
{
    k_sem_take(&ens_notify_sem, K_SECONDS(30));
    const size_t stride = sizeof(struct ens160_msg) + 4;
    char fatpath[MAX_PATH_LEN];
    int  count = 0, corrupt = 0;
    struct ens160_msg row;

    to_fatfs_path(SD_LOG_ENS160, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    if (fno.fsize % stride != 0) {
        LOG_WRN("SD drain ENS: file size not multiple of stride — skipping");
        k_mutex_unlock(&sd_mutex);
        return;
    }
    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    k_mutex_unlock(&sd_mutex);

    while (1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        FSIZE_t fsize = f_size(&fil);
        if (fsize < stride) {
            f_close(&fil);
            f_unlink(fatpath);
            k_mutex_unlock(&sd_mutex);
            break;
        }
        FSIZE_t rec_pos = fsize - stride;
        f_lseek(&fil, rec_pos);
        UINT br;
        int rc = read_raw_verify_crc(&fil, (uint8_t *)&row,
                                     sizeof(struct ens160_msg), &br);
        if (rc != 0) {
            LOG_WRN("SD drain ENS: corrupt tail — truncating");
            f_lseek(&fil, rec_pos);
            f_truncate(&fil);
            f_sync(&fil);
            corrupt++;
            k_mutex_unlock(&sd_mutex);
            continue;
        }
        k_mutex_unlock(&sd_mutex);

        if (!ens_pack_and_notify(&row)) {
            LOG_WRN("SD drain ENS: disconnected at record %d — %d remain in file",
                    count, (int)(rec_pos / stride));
            k_mutex_lock(&sd_mutex, K_FOREVER);
            f_close(&fil);
            k_mutex_unlock(&sd_mutex);
            return;
        }
        count++;

        k_mutex_lock(&sd_mutex, K_FOREVER);
        f_lseek(&fil, rec_pos);
        f_truncate(&fil);
        f_sync(&fil);
        k_mutex_unlock(&sd_mutex);
    }

    LOG_INF("SD drain ENS: %d OK, %d corrupt", count, corrupt);
}

static void drain_snd(void)
{
    k_sem_take(&snd_drain_sem, K_SECONDS(30));
    const size_t stride = sizeof(struct sound_spec_msg) + 4;
    char fatpath[MAX_PATH_LEN];
    int  count = 0, corrupt = 0;
    struct sound_spec_msg row;

    to_fatfs_path(SD_LOG_SOUND, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    if (fno.fsize % stride != 0) {
        LOG_WRN("SD drain SND: file size not multiple of stride — skipping");
        k_mutex_unlock(&sd_mutex);
        return;
    }
    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    k_mutex_unlock(&sd_mutex);

    while (1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        FSIZE_t fsize = f_size(&fil);
        if (fsize < stride) {
            f_close(&fil);
            f_unlink(fatpath);
            k_mutex_unlock(&sd_mutex);
            break;
        }
        FSIZE_t rec_pos = fsize - stride;
        f_lseek(&fil, rec_pos);
        UINT br;
        int rc = read_raw_verify_crc(&fil, (uint8_t *)&row,
                                     sizeof(struct sound_spec_msg), &br);
        if (rc != 0) {
            LOG_WRN("SD drain SND: corrupt tail — truncating");
            f_lseek(&fil, rec_pos);
            f_truncate(&fil);
            f_sync(&fil);
            corrupt++;
            k_mutex_unlock(&sd_mutex);
            continue;
        }
        k_mutex_unlock(&sd_mutex);

        if (!snd_is_connected()) {
            LOG_WRN("SD drain SND: disconnected at record %d — %d remain in file",
                    count, (int)(rec_pos / stride));
            k_mutex_lock(&sd_mutex, K_FOREVER);
            f_close(&fil);
            k_mutex_unlock(&sd_mutex);
            return;
        }

        send_spectrum(&row);
        count++;

        /* Truncate sent record off end — durable before next send */
        k_mutex_lock(&sd_mutex, K_FOREVER);
        f_lseek(&fil, rec_pos);
        f_truncate(&fil);
        f_sync(&fil);
        k_mutex_unlock(&sd_mutex);

        /* Throttle — gateway serialises ~2886 byte sound JSON;
         * live sound arrives at 1/sec so 2s gives a clear slot */
        k_sleep(K_MSEC(2000));
    }

    LOG_INF("SD drain SND: %d OK, %d corrupt", count, corrupt);
}

#elif defined(CONFIG_SENSOR_NODE_2)

static void drain_as7(void)
{
    k_sem_take(&as7_notify_sem, K_SECONDS(30));
    const size_t stride = sizeof(struct as7343_msg) + 4;
    char fatpath[MAX_PATH_LEN];
    int  count = 0, corrupt = 0;
    struct as7343_msg row;

    to_fatfs_path(SD_LOG_AS7343, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    if (fno.fsize % stride != 0) {
        LOG_WRN("SD drain AS7: file size not multiple of stride — skipping");
        k_mutex_unlock(&sd_mutex);
        return;
    }
    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    k_mutex_unlock(&sd_mutex);

    while (1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        FSIZE_t fsize = f_size(&fil);
        if (fsize < stride) {
            f_close(&fil);
            f_unlink(fatpath);
            k_mutex_unlock(&sd_mutex);
            break;
        }
        FSIZE_t rec_pos = fsize - stride;
        f_lseek(&fil, rec_pos);
        UINT br;
        int rc = read_raw_verify_crc(&fil, (uint8_t *)&row,
                                     sizeof(struct as7343_msg), &br);
        if (rc != 0) {
            LOG_WRN("SD drain AS7: corrupt tail — truncating");
            f_lseek(&fil, rec_pos);
            f_truncate(&fil);
            f_sync(&fil);
            corrupt++;
            k_mutex_unlock(&sd_mutex);
            continue;
        }
        k_mutex_unlock(&sd_mutex);

        if (!as7_pack_and_notify(&row)) {
            LOG_WRN("SD drain AS7: disconnected at record %d — %d remain in file",
                    count, (int)(rec_pos / stride));
            k_mutex_lock(&sd_mutex, K_FOREVER);
            f_close(&fil);
            k_mutex_unlock(&sd_mutex);
            return;
        }
        count++;

        k_mutex_lock(&sd_mutex, K_FOREVER);
        f_lseek(&fil, rec_pos);
        f_truncate(&fil);
        f_sync(&fil);
        k_mutex_unlock(&sd_mutex);

        /* Small yield — ~400 byte packets, cooperative scheduling */
        k_sleep(K_MSEC(100));
    }

    LOG_INF("SD drain AS7: %d OK, %d corrupt", count, corrupt);
}

static void drain_mst(void)
{
    k_sem_take(&mst_notify_sem, K_SECONDS(30));
    const size_t stride = sizeof(struct moisture_msg) + 4;
    char fatpath[MAX_PATH_LEN];
    int  count = 0, corrupt = 0;
    struct moisture_msg row;

    to_fatfs_path(SD_LOG_MOISTURE, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    if (fno.fsize % stride != 0) {
        LOG_WRN("SD drain MST: file size not multiple of stride — skipping");
        k_mutex_unlock(&sd_mutex);
        return;
    }
    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    k_mutex_unlock(&sd_mutex);

    while (1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        FSIZE_t fsize = f_size(&fil);
        if (fsize < stride) {
            f_close(&fil);
            f_unlink(fatpath);
            k_mutex_unlock(&sd_mutex);
            break;
        }
        FSIZE_t rec_pos = fsize - stride;
        f_lseek(&fil, rec_pos);
        UINT br;
        int rc = read_raw_verify_crc(&fil, (uint8_t *)&row,
                                     sizeof(struct moisture_msg), &br);
        if (rc != 0) {
            LOG_WRN("SD drain MST: corrupt tail — truncating");
            f_lseek(&fil, rec_pos);
            f_truncate(&fil);
            f_sync(&fil);
            corrupt++;
            k_mutex_unlock(&sd_mutex);
            continue;
        }
        k_mutex_unlock(&sd_mutex);

        if (!mst_pack_and_notify(&row)) {
            LOG_WRN("SD drain MST: disconnected at record %d — %d remain in file",
                    count, (int)(rec_pos / stride));
            k_mutex_lock(&sd_mutex, K_FOREVER);
            f_close(&fil);
            k_mutex_unlock(&sd_mutex);
            return;
        }
        count++;

        k_mutex_lock(&sd_mutex, K_FOREVER);
        f_lseek(&fil, rec_pos);
        f_truncate(&fil);
        f_sync(&fil);
        k_mutex_unlock(&sd_mutex);
    }

    LOG_INF("SD drain MST: %d OK, %d corrupt", count, corrupt);
}

/* ── 5. Add drain_ds18b20() in the NODE_2 block alongside drain_as7/mst ─── */
 
static void drain_ds18b20(void)
{
    k_sem_take(&ds18b20_notify_sem, K_SECONDS(30));
    const size_t stride = sizeof(struct ds18b20_msg) + 4;
    char fatpath[MAX_PATH_LEN];
    int  count = 0, corrupt = 0;
    struct ds18b20_msg row;
 
    to_fatfs_path(SD_LOG_DS18B20, fatpath, sizeof(fatpath));
 
    k_mutex_lock(&sd_mutex, K_FOREVER);
    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    if (fno.fsize % stride != 0) {
        LOG_WRN("SD drain DS18B20: file size not multiple of stride — skipping");
        k_mutex_unlock(&sd_mutex);
        return;
    }
    FIL fil;
    if (f_open(&fil, fatpath, FA_READ | FA_WRITE) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }
    k_mutex_unlock(&sd_mutex);
 
    while (1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        FSIZE_t fsize = f_size(&fil);
        if (fsize < stride) {
            f_close(&fil);
            f_unlink(fatpath);
            k_mutex_unlock(&sd_mutex);
            break;
        }
        FSIZE_t rec_pos = fsize - stride;
        f_lseek(&fil, rec_pos);
        UINT br;
        int rc = read_raw_verify_crc(&fil, (uint8_t *)&row,
                                     sizeof(struct ds18b20_msg), &br);
        if (rc != 0) {
            LOG_WRN("SD drain DS18B20: corrupt tail — truncating");
            f_lseek(&fil, rec_pos);
            f_truncate(&fil);
            f_sync(&fil);
            corrupt++;
            k_mutex_unlock(&sd_mutex);
            continue;
        }
        k_mutex_unlock(&sd_mutex);
 
        if (!ds18b20_pack_and_notify(&row)) {
            LOG_WRN("SD drain DS18B20: disconnected at record %d — %d remain in file",
                    count, (int)(rec_pos / stride));
            k_mutex_lock(&sd_mutex, K_FOREVER);
            f_close(&fil);
            k_mutex_unlock(&sd_mutex);
            return;
        }
        count++;
 
        k_mutex_lock(&sd_mutex, K_FOREVER);
        f_lseek(&fil, rec_pos);
        f_truncate(&fil);
        f_sync(&fil);
        k_mutex_unlock(&sd_mutex);
    }
 
    LOG_INF("SD drain DS18B20: %d OK, %d corrupt", count, corrupt);
}

#endif /* CONFIG_SENSOR_NODE_1 / 2 */

/* ═══════════════════════════════════════════════════════════════════════════
 * DRAIN THREAD
 *
 * Triggered by k_sem_give(&sd_drain_sem) from the BLE connected() callback.
 * BLE threads are NOT blocked during drain — live data flows alongside
 * drain records.  drain_snd() is throttled internally at 2000ms/record
 * to avoid overwhelming the gateway's JSON serialiser.
 * ═══════════════════════════════════════════════════════════════════════════ */

void sd_drain_thread(void) {
    sd_log_init();
    LOG_INF("SD drain thread ready");

    while (1) {
        k_sem_take(&sd_drain_sem, K_FOREVER);

        if (!sd_ready) {
            LOG_WRN("SD drain: SD not ready — skipping");
            continue;
        }

        LOG_INF("SD drain: replaying UTC-stamped records alongside live BLE...");
        sd_log_set_draining(true);
        k_sleep(K_SECONDS(5));

        #if defined(CONFIG_SENSOR_NODE_1)
        drain_bme();
        drain_ens();
        drain_snd();   /* throttled at 2000ms/record internally */
        #elif defined(CONFIG_SENSOR_NODE_2)
        drain_as7();   /* throttled at 100ms/record internally */
        drain_mst();
        drain_ds18b20();   /* ← add this line */
        #else
        // #error "No sensor node selected."
        #endif

        sd_log_set_draining(false);
        LOG_INF("SD drain: complete");
    }
}