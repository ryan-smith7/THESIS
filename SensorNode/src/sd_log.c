/**
 * @file sd_log.c
 * @brief SD card offline logging — binary format with UTC/uptime routing.
 *
 * UTC-valid records go to constant-named .bin files, replayed on reconnect.
 * Uptime/unsynced records go to boot-numbered _upt_NNNN.bin files, kept for
 * offline analysis only and never replayed over BLE.
 *
 * Boot counter is read from /SD/bootcount.txt at init, incremented, and
 * written back. Uptime filenames are built once at init:
 *   e.g. /SD/bme280_upt_0003.bin for boot number 3.
 */

#include "sd_log.h"
#include "file.h"
#include "bme_ble.h"
#include "ens_ble.h"
#include "as7_ble.h"
#include "mst_ble.h"

#if defined(CONFIG_SENSOR_NODE_1)
#include "sound.h"
#include "sound_ble.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(sd_log, LOG_LEVEL_INF);

#define MAX_DRAIN_ROWS   32
#define UPT_PATH_MAX_LEN 32   /* e.g. "/SD/bme280_upt_9999.bin" = 24 chars */

/* ── Drain semaphore ─────────────────────────────────────────────────────── */
K_SEM_DEFINE(sd_drain_sem, 0, 1);

/* ── SD state ────────────────────────────────────────────────────────────── */
static bool sd_ready    = false;
static bool sd_draining = false;

/* ── SD access mutex ─────────────────────────────────────────────────────── */
static K_MUTEX_DEFINE(sd_mutex);

/* ── Boot-numbered uptime file paths — built once at init ───────────────── */
#if defined(CONFIG_SENSOR_NODE_1)
static char upt_bme[UPT_PATH_MAX_LEN];
static char upt_ens[UPT_PATH_MAX_LEN];
static char upt_snd[UPT_PATH_MAX_LEN];
#elif defined(CONFIG_SENSOR_NODE_2)
static char upt_as7[UPT_PATH_MAX_LEN];
static char upt_mst[UPT_PATH_MAX_LEN];
#endif

/* ── Static drain buffers — BSS, never on stack ──────────────────────────── */
#if defined(CONFIG_SENSOR_NODE_1)
static struct bme280_msg     bme_drain_rows[MAX_DRAIN_ROWS];
static struct ens160_msg     ens_drain_rows[MAX_DRAIN_ROWS];
static struct sound_spec_msg snd_drain_rows[MAX_DRAIN_ROWS];
#elif defined(CONFIG_SENSOR_NODE_2)
static struct as7343_msg   as7_drain_rows[MAX_DRAIN_ROWS];
static struct moisture_msg mst_drain_rows[MAX_DRAIN_ROWS];
#endif

bool sd_log_is_ready(void)
{
    return sd_ready;
}

bool sd_log_is_draining(void)
{
    return sd_draining;
}

void sd_log_set_draining(bool draining)
{
    sd_draining = draining;
}

/* ── Path conversion ─────────────────────────────────────────────────────── */
static void zpath_to_fatfs(const char *zpath, char *out, size_t outlen)
{
    if (strncmp(zpath, "/SD", 3) == 0) {
        snprintf(out, outlen, "SD:%s", zpath + 3);
        if (strlen(out) == 3) {
            strncat(out, "/", outlen - strlen(out) - 1);
        }
    } else {
        strncpy(out, zpath, outlen - 1);
        out[outlen - 1] = '\0';
    }
}

/* ── Boot counter ────────────────────────────────────────────────────────── */
/*
 * Reads the current boot count from SD_LOG_BOOTCOUNT, increments it,
 * writes it back, and returns the new value.
 * Returns 0 if the file cannot be read/written (SD error).
 */
static uint32_t read_and_increment_boot_count(void)
{
    char fatpath[MAX_PATH_LEN];
    char buf[16];
    uint32_t count = 0;

    zpath_to_fatfs(SD_LOG_BOOTCOUNT, fatpath, sizeof(fatpath));

    FIL fil;

    /* Try to read existing count */
    if (f_open(&fil, fatpath, FA_READ) == FR_OK) {
        UINT br;
        memset(buf, 0, sizeof(buf));
        if (f_read(&fil, buf, sizeof(buf) - 1, &br) == FR_OK && br > 0) {
            count = (uint32_t)strtoul(buf, NULL, 10);
        }
        f_close(&fil);
    }

    /* Increment */
    count++;

    /* Write back */
    if (f_open(&fil, fatpath, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT bw;
        snprintf(buf, sizeof(buf), "%u\n", (unsigned)count);
        f_write(&fil, buf, strlen(buf), &bw);
        f_close(&fil);
    }

    return count;
}

/* ── Init ────────────────────────────────────────────────────────────────── */
int sd_log_init(void)
{
    int rc = fatfs_mount();
    if (rc < 0) {
        LOG_ERR("SD log: mount failed (%d)", rc);
        return rc;
    }
    sd_ready = true;

    /* Read and increment boot counter, then build uptime file paths */
    uint32_t boot = read_and_increment_boot_count();
    LOG_INF("SD log: boot #%u", (unsigned)boot);


#if defined(CONFIG_SENSOR_NODE_1)
    // snprintf(upt_bme, sizeof(upt_bme), "/SD/bme280_upt_%04u.bin",  (unsigned)boot);
    // snprintf(upt_ens, sizeof(upt_ens), "/SD/ens160_upt_%04u.bin",  (unsigned)boot);
    // snprintf(upt_snd, sizeof(upt_snd), "/SD/sound_upt_%04u.bin",   (unsigned)boot);
    snprintf(upt_bme, sizeof(upt_bme), "/SD/BME%04u.BIN", (unsigned)boot);
    snprintf(upt_ens, sizeof(upt_ens), "/SD/ENS%04u.BIN", (unsigned)boot);
    snprintf(upt_snd, sizeof(upt_snd), "/SD/SND%04u.BIN", (unsigned)boot);
#elif defined(CONFIG_SENSOR_NODE_2)
    snprintf(upt_as7, sizeof(upt_as7), "/SD/AS7%04u.BIN", (unsigned)boot);
    snprintf(upt_mst, sizeof(upt_mst), "/SD/MST%04u.BIN", (unsigned)boot);
    // snprintf(upt_as7, sizeof(upt_as7), "/SD/as7343_upt_%04u.bin",  (unsigned)boot);
    // snprintf(upt_mst, sizeof(upt_mst), "/SD/moisture_upt_%04u.bin",(unsigned)boot);
#endif

    LOG_INF("SD log: ready");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LOG FUNCTIONS — route to UTC or boot-numbered uptime file
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(CONFIG_SENSOR_NODE_1)

void sd_log_bme(const struct bme280_msg *msg)
{
    const char *path;

    if (!sd_ready) {
        return;
    }

    if (msg->utc_sec > SD_LOG_UTC_MIN) {
        path = SD_LOG_BME280;
    } else {
        path = upt_bme;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    write_raw_to_file(path, (const uint8_t *)msg, sizeof(*msg));
    k_mutex_unlock(&sd_mutex);
}

void sd_log_ens(const struct ens160_msg *msg)
{
    const char *path;

    if (!sd_ready) {
        return;
    }

    if (msg->utc_sec > SD_LOG_UTC_MIN) {
        path = SD_LOG_ENS160;
    } else {
        path = upt_ens;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    write_raw_to_file(path, (const uint8_t *)msg, sizeof(*msg));
    k_mutex_unlock(&sd_mutex);
}

void sd_log_snd(const struct sound_spec_msg *spec)
{
    const char *path;

    if (!sd_ready) {
        return;
    }

    if (spec->utc_sec > SD_LOG_UTC_MIN) {
        path = SD_LOG_SOUND;
    } else {
        path = upt_snd;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    write_raw_to_file(path, (const uint8_t *)spec, sizeof(*spec));
    k_mutex_unlock(&sd_mutex);
}

#elif defined(CONFIG_SENSOR_NODE_2)

void sd_log_as7(const struct as7343_msg *msg)
{
    const char *path;

    if (!sd_ready) {
        return;
    }

    if (msg->utc_sec > SD_LOG_UTC_MIN) {
        path = SD_LOG_AS7343;
    } else {
        path = upt_as7;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    write_raw_to_file(path, (const uint8_t *)msg, sizeof(*msg));
    k_mutex_unlock(&sd_mutex);
}

void sd_log_mst(const struct moisture_msg *msg)
{
    const char *path;

    if (!sd_ready) {
        return;
    }

    if (msg->utc_sec > SD_LOG_UTC_MIN) {
        path = SD_LOG_MOISTURE;
    } else {
        path = upt_mst;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    write_raw_to_file(path, (const uint8_t *)msg, sizeof(*msg));
    k_mutex_unlock(&sd_mutex);
}

#endif /* CONFIG_SENSOR_NODE_1 / 2 */

/* ═══════════════════════════════════════════════════════════════════════════
 * DRAIN HELPERS — only primary UTC files are replayed over BLE
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(CONFIG_SENSOR_NODE_1)

static void drain_bme(void)
{
    char fatpath[MAX_PATH_LEN];
    int count = 0;

    zpath_to_fatfs(SD_LOG_BME280, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);

    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    FIL fil;
    if (f_open(&fil, fatpath, FA_READ) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    UINT br;
    while (count < MAX_DRAIN_ROWS) {
        FRESULT fr = f_read(&fil, &bme_drain_rows[count],
                            sizeof(struct bme280_msg), &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        if (br == sizeof(struct bme280_msg)) {
            count++;
        }
    }

    f_close(&fil);
    f_unlink(fatpath);
    k_mutex_unlock(&sd_mutex);

    for (int i = 0; i < count; i++) {
        bme_ble_notify_offline(&bme_drain_rows[i]);
    }

    LOG_INF("SD drain BME: %d UTC records replayed", count);
}

static void drain_ens(void)
{
    char fatpath[MAX_PATH_LEN];
    int count = 0;

    zpath_to_fatfs(SD_LOG_ENS160, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);

    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    FIL fil;
    if (f_open(&fil, fatpath, FA_READ) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    UINT br;
    while (count < MAX_DRAIN_ROWS) {
        FRESULT fr = f_read(&fil, &ens_drain_rows[count],
                            sizeof(struct ens160_msg), &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        if (br == sizeof(struct ens160_msg)) {
            count++;
        }
    }

    f_close(&fil);
    f_unlink(fatpath);
    k_mutex_unlock(&sd_mutex);

    for (int i = 0; i < count; i++) {
        ens_ble_notify_offline(&ens_drain_rows[i]);
    }

    LOG_INF("SD drain ENS: %d UTC records replayed", count);
}

static void drain_snd(void)
{
    char fatpath[MAX_PATH_LEN];
    int count = 0;

    zpath_to_fatfs(SD_LOG_SOUND, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);

    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    FIL fil;
    if (f_open(&fil, fatpath, FA_READ) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    UINT br;
    while (count < MAX_DRAIN_ROWS) {
        FRESULT fr = f_read(&fil, &snd_drain_rows[count],
                            sizeof(struct sound_spec_msg), &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        if (br == sizeof(struct sound_spec_msg)) {
            count++;
        }
    }

    f_close(&fil);
    f_unlink(fatpath);
    k_mutex_unlock(&sd_mutex);

    for (int i = 0; i < count; i++) {
        sound_ble_notify_offline(&snd_drain_rows[i]);
        k_sleep(K_MSEC(100));
    }

    LOG_INF("SD drain SND: %d UTC records replayed", count);
}

#elif defined(CONFIG_SENSOR_NODE_2)

static void drain_as7(void)
{
    char fatpath[MAX_PATH_LEN];
    int count = 0;

    zpath_to_fatfs(SD_LOG_AS7343, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);

    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    FIL fil;
    if (f_open(&fil, fatpath, FA_READ) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    UINT br;
    while (count < MAX_DRAIN_ROWS) {
        FRESULT fr = f_read(&fil, &as7_drain_rows[count],
                            sizeof(struct as7343_msg), &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        if (br == sizeof(struct as7343_msg)) {
            count++;
        }
    }

    f_close(&fil);
    f_unlink(fatpath);
    k_mutex_unlock(&sd_mutex);

    for (int i = 0; i < count; i++) {
        as7_ble_notify_offline(&as7_drain_rows[i]);
    }

    LOG_INF("SD drain AS7: %d UTC records replayed", count);
}

static void drain_mst(void)
{
    char fatpath[MAX_PATH_LEN];
    int count = 0;

    zpath_to_fatfs(SD_LOG_MOISTURE, fatpath, sizeof(fatpath));

    k_mutex_lock(&sd_mutex, K_FOREVER);

    FILINFO fno;
    if (f_stat(fatpath, &fno) != FR_OK || fno.fsize == 0) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    FIL fil;
    if (f_open(&fil, fatpath, FA_READ) != FR_OK) {
        k_mutex_unlock(&sd_mutex);
        return;
    }

    UINT br;
    while (count < MAX_DRAIN_ROWS) {
        FRESULT fr = f_read(&fil, &mst_drain_rows[count],
                            sizeof(struct moisture_msg), &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        if (br == sizeof(struct moisture_msg)) {
            count++;
        }
    }

    f_close(&fil);
    f_unlink(fatpath);
    k_mutex_unlock(&sd_mutex);

    for (int i = 0; i < count; i++) {
        mst_ble_notify_offline(&mst_drain_rows[i]);
    }

    LOG_INF("SD drain MST: %d UTC records replayed", count);
}

#endif /* CONFIG_SENSOR_NODE_1 / 2 */

/* ═══════════════════════════════════════════════════════════════════════════
 * DRAIN THREAD
 * ═══════════════════════════════════════════════════════════════════════════ */

void sd_drain_thread(void)
{
    sd_log_init();
    LOG_INF("SD drain thread ready");

    while (1) {
        k_sem_take(&sd_drain_sem, K_FOREVER);

        if (!sd_ready) {
            LOG_WRN("SD drain: SD not ready — skipping");
            continue;
        }

        LOG_INF("SD drain: replaying UTC-stamped records...");
        k_sleep(K_SECONDS(10));

        #if defined(CONFIG_SENSOR_NODE_1)
        drain_bme();
        drain_ens();
        drain_snd();
        #elif defined(CONFIG_SENSOR_NODE_2)
        drain_as7();
        drain_mst();
        #else
        // #error "No sensor node selected."
        #endif

        sd_log_set_draining(false);
        LOG_INF("SD drain: complete — resuming live BLE");
    }
}