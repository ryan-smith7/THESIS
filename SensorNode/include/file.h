/**
 * @file file.h
 * @brief File library header - FAT32 on microSD via SPI
 *
 * Provides an interface for mounting a FAT32 formatted microSD card
 * over SPI and performing basic file I/O and shell commands.
 *
 * Board target : esp32_devkitc_wroom (ESP32 Thing Plus C)
 * File system  : FatFS (FAT32)
 * Interface    : SPI -> sdhc-spi-slot -> sdmmc-disk
 */

#ifndef FILE_H
#define FILE_H

#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>
#include <ff.h>

/* -- Mount point ---------------------------------------------------- */

/** Root mount point for the FAT32 SD card.
 *  Must match the disk-name in the device tree overlay ("SD"). */
#define SD_MOUNT_POINT  "/SD"

/* -- Thread configuration ------------------------------------------ */

#define FILE_STACK_SIZE  4096
#define FILE_PRIORITY    5

/* -- Path helpers --------------------------------------------------- */

#define MAX_PATH_LEN  256

/* -- Write mode flags ----------------------------------------------- */

#define INVALID  (-1)
#define APPEND     1
#define TRUNC      0

/* -- Public API ----------------------------------------------------- */

/**
 * @brief Mount the FAT32 SD card file system.
 * @return 0 on success, negative errno on failure.
 */
extern int fatfs_mount(void);

/**
 * @brief Write (append or truncate) a string to a file on the SD card.
 *
 * @param fname           Absolute or relative file path.
 * @param data            Null-terminated string to write.
 * @param trunc_or_append APPEND (1) to append, TRUNC (0) to overwrite.
 * @return 0 on success, negative errno on failure.
 */
extern int write_data_to_file(const char *fname, char *data,
                               uint8_t trunc_or_append);

/**
 * @brief Write raw binary data (append) to a file on the SD card.
 *
 * Unlike write_data_to_file, uses explicit length rather than strlen
 * so binary data containing null bytes is handled correctly.
 *
 * @param fname  Absolute or relative file path.
 * @param data   Pointer to binary data to write.
 * @param len    Number of bytes to write.
 * @return 0 on success, negative errno on failure.
 */
extern int write_raw_to_file(const char *fname, const uint8_t *data,
                              size_t len);

/**
 * @brief Read one line from a FatFS file into buf.
 *
 * Reads byte-by-byte until newline, EOF, or buffer full.
 * Always null-terminates buf.
 *
 * @return true if at least one byte was read, false on EOF.
 */
extern bool fatfs_readline(FIL *fil, char *buf, size_t maxlen);

/**
 * @brief Thread entry - mounts the SD card and registers shell commands.
 */
extern void file_control_thread(void);

#endif /* FILE_H */