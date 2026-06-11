/**
 * @file sd_log.c
 * @brief SD card offline logging — binary records with CRC32, dual-file targets.
 *
 * Each modality writes to two files:
 *   Boot archive  (e.g. /SD/BME0003.BIN) — every sample, never deleted.
 *   UTC upload    (e.g. SD_LOG_BME280)   — UTC-valid records only; drained
 *                                          over BLE on reconnect then deleted.
 *
 * Routing decisions live in the modality BLE threads, not here.
 *
 * On init: boot counter is incremented, boot-numbered paths are built, and
 * all UTC upload files are tail-healed to remove any partial record left by
 * a previous power-loss.
 *
 * Drain: records are replayed most-recent-first by truncating from the end
 * of the UTC file after each successful BLE notify. A power-cycle mid-drain
 * leaves only unsent records in the file; resuming on reconnect requires no
 * position tracking and produces no duplicates.
 */

#include "sd_log.h"
#include "file.h"
#include "bme_ble.h"
#include "ens_ble.h"
#include "as7_ble.h"
#include "mst_ble.h"
#include "ds18b20_ble.h"

#include "sound.h"
#include "sound_ble.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

LOG_MODULE_REGISTER(sd_log, LOG_LEVEL_INF);

#define BOOT_PATH_MAX    24   /* e.g. "/SD/BME0003.BIN" = 16 chars, headroom */
#define DRAIN_DELAY 30

/* ── Drain semaphore — triggered by connected() callback */
K_SEM_DEFINE(sd_drain_sem, 0, 1);

/* ── SD state */
static bool sd_ready    = false;
static bool sd_draining = false;

/* ── SD access mutex */
static K_MUTEX_DEFINE(sd_mutex);

/* ── Boot-numbered archive file paths — built once at init */
// #if defined(CONFIG_SENSOR_NODE_1)
static char boot_bme[BOOT_PATH_MAX];
static char boot_ens[BOOT_PATH_MAX];
static char boot_snd[BOOT_PATH_MAX];
// #elif defined(CONFIG_SENSOR_NODE_2)
static char boot_as7[BOOT_PATH_MAX];
static char boot_mst[BOOT_PATH_MAX];
static char boot_ds18b20[BOOT_PATH_MAX];
// #endif

// #if defined(CONFIG_SENSOR_NODE_1)
const char *sd_log_boot_path_bme(void) { return boot_bme; }
const char *sd_log_boot_path_ens(void) { return boot_ens; }
const char *sd_log_boot_path_snd(void) { return boot_snd; }
// #elif defined(CONFIG_SENSOR_NODE_2)
const char *sd_log_boot_path_as7(void) { return boot_as7; }
const char *sd_log_boot_path_mst(void) { return boot_mst; }
const char *sd_log_boot_path_ds18b20(void) { return boot_ds18b20; }
// #endif

/** @brief Returns true if the SD card is mounted and ready. */
bool sd_log_is_ready(void) {
    return sd_ready;
}

/** @brief Returns true if a drain is currently in progress. */
bool sd_log_is_draining(void) {
    return sd_draining;
}

/** @brief Set the draining state flag. */
void sd_log_set_draining(bool draining) {
    sd_draining = draining;
    /* No k_event — BLE threads are never blocked during drain.
     * Live data flows alongside drain records. */
}
/**
 * @brief Read, increment, and persist the boot counter from /SD/bootcount.txt.
 *
 * @return Incremented boot count.
 */
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

/**
 * @brief Truncate a UTC upload file to the last CRC-valid record boundary.
 *
 * Removes any partial record left by a power-loss mid-write so subsequent
 * appends start from a clean record boundary.
 *
 * @param zpath       Zephyr-style path to the UTC upload file.
 * @param record_len  Struct payload size (without CRC trailer).
 */
static void heal_utc_file_tail(const char *zpath, size_t record_len) {

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

/**
 * @brief Mount the SD card, build boot-numbered archive paths, and tail-heal
 * all UTC upload files.
 *
 * @return 0 on success, negative errno on mount failure.
 */
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

/**
 * @brief Write a binary record to an SD file under the SD mutex.
 *
 * Disables further writes if the card is full (-ENOSPC).
 *
 * @param path  Destination file path.
 * @param msg   Record payload.
 * @param len   Payload size in bytes.
 */
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

/**
 * @brief Drain BME280 UTC upload file over BLE, most-recent-first.
 *
 * Waits for bme_notify_sem, then reads from the end of SD_LOG_BME280,
 * notifies via bme_pack_and_notify(), and truncates each sent record.
 * Returns immediately on disconnect; corrupt tail records are silently
 * truncated.
 */
#if defined(CONFIG_SENSOR_NODE_1)

static void drain_bme(void) {

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
        k_sleep(K_MSEC(DRAIN_DELAY));
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
/**
 * @brief Drain ENS160 UTC upload file over BLE, most-recent-first.
 */
static void drain_ens(void) {

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
        k_sleep(K_MSEC(DRAIN_DELAY));
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

/**
 * @brief Drain sound UTC upload file over BLE, most-recent-first.
 *
 * Throttled at 2000 ms per record to allow the gateway time to serialise
 * the ~2886 byte sound JSON payload.
 */
static void drain_snd(void) {

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
        // k_sleep(K_MSEC(30));
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

/**
 * @brief Drain AS7343 UTC upload file over BLE, most-recent-first.
 *
 * Throttled at 100 ms per record for cooperative scheduling.
 */
static void drain_as7(void) {

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
        k_sleep(K_MSEC(DRAIN_DELAY));
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

/**
 * @brief Drain soil moisture UTC upload file over BLE, most-recent-first.
 */
static void drain_mst(void) {

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
        k_sleep(K_MSEC(DRAIN_DELAY));
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

/**
 * @brief Drain DS18B20 UTC upload file over BLE, most-recent-first.
 */
static void drain_ds18b20(void) {

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
        k_sleep(K_MSEC(DRAIN_DELAY));
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

/**
 * @brief SD drain thread — initialises SD logging then waits on sd_drain_sem.
 *
 * Triggered by the BLE connected() callback. Runs all modality drain
 * functions sequentially alongside live BLE data flow; does not block
 * the modality BLE threads.
 */
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