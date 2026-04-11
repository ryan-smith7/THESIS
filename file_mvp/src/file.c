/**
 * @file file.c
 * @brief File library – FAT32 on microSD card via SPI
 *
 * Mounts a FAT32-formatted microSD card and exposes shell commands for
 * basic file-system navigation and I/O.
 *
 * All file operations use the raw FatFS API (ff.h) directly rather than
 * Zephyr's fs abstraction layer. This is required because Zephyr 4.x does
 * not correctly translate /SD/ mount-point paths to the FatFS drive prefix
 * SD:/ when using the SPI SDHC driver. The raw API works reliably.
 *
 * Path convention:
 *   Shell / user facing  →  /SD/foo/bar.txt   (current_path based)
 *   FatFS internal       →  SD:/foo/bar.txt   (converted by to_fatfs_path)
 *
 * Reference: zephyr/samples/subsys/fs/fat_fs/src/main.c
 */

#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include "file.h"
#include <string.h>
#include <stdlib.h>

/* ── Logging ─────────────────────────────────────────────────────────────── */

LOG_MODULE_REGISTER(fs_module, LOG_LEVEL_INF);

/* ── Mount state ─────────────────────────────────────────────────────────── */

static FATFS fat_fs;

static struct fs_mount_t sd_mount = {
	.type        = FS_FATFS,
	.fs_data     = &fat_fs,
	.storage_dev = (void *)"SD",
	.mnt_point   = SD_MOUNT_POINT,
};

/* ── Current working directory (shell context) ───────────────────────────── */

static char current_path[MAX_PATH_LEN] = SD_MOUNT_POINT;

/* ── Logging state (stubbed until sensor module is integrated) ───────────── */

static uint8_t logging_active = OFF;
static char    logging_filepath[MAX_PATH_LEN] = {0};

static void set_logging_state(uint8_t state) { logging_active = state; }
static void set_logging_file(const char *path) { strncpy(logging_filepath, path, MAX_PATH_LEN - 1); }

/* ══════════════════════════════════════════════════════════════════════════
 * PATH HELPERS
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Convert a Zephyr-style /SD/... path to a FatFS SD:/... path.
 *
 * FatFS requires the volume label prefix "SD:/" while the shell and user
 * facing API uses "/SD/" to stay consistent with the Zephyr mount point.
 *
 * /SD/foo/bar.txt  →  SD:/foo/bar.txt
 * /SD             →  SD:/
 *
 * @param zpath   Input path beginning with /SD
 * @param fatpath Output buffer for the converted path
 * @param maxlen  Size of the output buffer
 */
static void to_fatfs_path(const char *zpath, char *fatpath, size_t maxlen)
{
	if (strncmp(zpath, "/SD", 3) == 0) {
		/* Replace leading /SD with SD: */
		snprintf(fatpath, maxlen, "SD:%s", zpath + 3);
		/* Ensure at least SD:/ for the root */
		if (strlen(fatpath) == 3) {
			strncat(fatpath, "/", maxlen - strlen(fatpath) - 1);
		}
	} else {
		/* Already a raw FatFS path or unknown – pass through */
		strncpy(fatpath, zpath, maxlen - 1);
		fatpath[maxlen - 1] = '\0';
	}
}

/**
 * @brief Append a relative segment to current_path into combined_path.
 */
static void combine_paths(const char *relative_path, char *combined_path)
{
	strcpy(combined_path, current_path);
	if (relative_path[0] == '/') {
		strcat(combined_path, relative_path);
	} else {
		strcat(combined_path, "/");
		strcat(combined_path, relative_path);
	}
}

/**
 * @brief Resolve a name to an absolute /SD/... path.
 */
static void determine_path(const char *name, char *combined_path)
{
	if (name[0] != '/') {
		combine_paths(name, combined_path);
	} else {
		strcpy(combined_path, name);
	}
}

/**
 * @brief Move current_path up one level, never above /SD root.
 */
static void move_up_level(void)
{
	if (strcmp(current_path, SD_MOUNT_POINT) == 0) {
		LOG_ERR("Already at mount root: %s", current_path);
		return;
	}

	char *last_slash = strrchr(current_path, '/');
	if (last_slash != NULL && last_slash != current_path) {
		*last_slash = '\0';
		if (strlen(current_path) < strlen(SD_MOUNT_POINT)) {
			strcpy(current_path, SD_MOUNT_POINT);
		}
		LOG_INF("Directory changed to: %s", current_path);
	} else {
		LOG_ERR("Already at mount root: %s", current_path);
	}
}

/* ══════════════════════════════════════════════════════════════════════════
 * MOUNT / INIT
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Verify disk is accessible then mount FAT32.
 *
 * @return 0 on success, negative errno on failure.
 */
int fatfs_mount(void)
{
	static const char *disk_pdrv = "SD";
	uint64_t memory_size_mb;
	uint32_t block_count;
	uint32_t block_size;

	if (disk_access_init(disk_pdrv) != 0) {
		LOG_ERR("Storage init ERROR – is the SD card inserted?");
		return -EIO;
	}

	if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_COUNT, &block_count) != 0) {
		LOG_ERR("Unable to get sector count");
		return -EIO;
	}
	LOG_INF("Block count: %u", block_count);

	if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_SIZE, &block_size) != 0) {
		LOG_ERR("Unable to get sector size");
		return -EIO;
	}
	LOG_INF("Sector size: %u bytes", block_size);

	memory_size_mb = (uint64_t)block_count * block_size;
	LOG_INF("Memory size: %u MB", (uint32_t)(memory_size_mb >> 20));

	int rc = fs_mount(&sd_mount);
	if (rc < 0) {
		LOG_ERR("FAIL: fs_mount at %s – rc=%d", sd_mount.mnt_point, rc);
		return rc;
	}

	LOG_INF("SD card mounted at %s", sd_mount.mnt_point);
	return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PUBLIC FILE I/O  (raw FatFS)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Write a string to a file using raw FatFS (append or overwrite).
 *
 * @param fname           /SD/... style path.
 * @param data            Null-terminated string to write.
 * @param trunc_or_append APPEND (1) or TRUNC (0).
 * @return 0 on success, -EIO on failure.
 */
int write_data_to_file(const char *fname, char *data, uint8_t trunc_or_append)
{
	char fatpath[MAX_PATH_LEN];
	to_fatfs_path(fname, fatpath, sizeof(fatpath));

	FIL fil;
	FRESULT fr;
	UINT bw;

	BYTE mode = (trunc_or_append == APPEND)
		    ? (FA_OPEN_APPEND | FA_WRITE)
		    : (FA_CREATE_ALWAYS | FA_WRITE);

	fr = f_open(&fil, fatpath, mode);
	if (fr != FR_OK) {
		LOG_ERR("FAIL: f_open %s: %d", fatpath, fr);
		return -EIO;
	}

	fr = f_write(&fil, data, strlen(data), &bw);
	if (fr != FR_OK || bw != strlen(data)) {
		LOG_ERR("FAIL: f_write %s: %d (wrote %u)", fatpath, fr, bw);
		f_close(&fil);
		return -EIO;
	}

	f_close(&fil);
	return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SHELL COMMANDS  (raw FatFS)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief ls – list directory contents.
 *
 *   ls [path]
 */
static int ls_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 2) {
		shell_print(shell, "Usage: ls [path]");
		return INVALID;
	}

	char zpath[MAX_PATH_LEN];
	char fatpath[MAX_PATH_LEN];

	if (argc == 2) {
		determine_path(argv[1], zpath);
	} else {
		strcpy(zpath, current_path);
	}

	to_fatfs_path(zpath, fatpath, sizeof(fatpath));
	LOG_INF("Listing: %s", fatpath);

	DIR dir;
	FRESULT fr = f_opendir(&dir, fatpath);
	if (fr != FR_OK) {
		LOG_ERR("Error opening directory %s [%d]", fatpath, fr);
		return -EIO;
	}

	FILINFO fno;
	for (;;) {
		fr = f_readdir(&dir, &fno);
		if (fr != FR_OK || fno.fname[0] == 0) {
			break;
		}
		if (fno.fattrib & AM_DIR) {
			shell_print(shell, "[DIR ] %s", fno.fname);
		} else {
			shell_print(shell, "[FILE] %s (size = %lu)",
				    fno.fname, (unsigned long)fno.fsize);
		}
	}

	f_closedir(&dir);
	return 0;
}

/**
 * @brief cd – change current working directory.
 *
 *   cd <path>   (use ".." to go up)
 */
static int cd_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(shell, "Usage: cd <path>");
		return INVALID;
	}

	if (strcmp(argv[1], "..") == 0) {
		move_up_level();
		return 0;
	}

	char zpath[MAX_PATH_LEN];
	char fatpath[MAX_PATH_LEN];
	determine_path(argv[1], zpath);
	to_fatfs_path(zpath, fatpath, sizeof(fatpath));

	/* Verify directory exists by opening it */
	DIR dir;
	FRESULT fr = f_opendir(&dir, fatpath);
	if (fr != FR_OK) {
		LOG_ERR("Directory not found: %s [%d]", fatpath, fr);
		return -ENOENT;
	}
	f_closedir(&dir);

	strcpy(current_path, zpath);
	LOG_INF("Directory changed to: %s", current_path);
	return 0;
}

/**
 * @brief mkdir – create a new directory.
 *
 *   mkdir <name>
 */
static int mkdir_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(shell, "Usage: mkdir <dir_name>");
		return INVALID;
	}

	char zpath[MAX_PATH_LEN];
	char fatpath[MAX_PATH_LEN];
	determine_path(argv[1], zpath);
	to_fatfs_path(zpath, fatpath, sizeof(fatpath));

	FRESULT fr = f_mkdir(fatpath);
	if (fr != FR_OK) {
		LOG_ERR("Error creating directory %s: %d", fatpath, fr);
		return -EIO;
	}

	LOG_INF("Directory created: %s", zpath);
	return 0;
}

/**
 * @brief write – append data to a file from the shell.
 *
 *   write <file> <data…>
 */
static int write_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_print(shell, "Usage: write <file> <data>");
		return INVALID;
	}

	/* Join all data arguments with spaces */
	size_t total_len = 0;
	for (int i = 2; i < argc; i++) {
		total_len += strlen(argv[i]) + 1;
	}

	char *data = malloc(total_len);
	if (!data) {
		LOG_ERR("Memory allocation failed");
		return INVALID;
	}
	data[0] = '\0';
	for (int i = 2; i < argc; i++) {
		strcat(data, argv[i]);
		if (i < argc - 1) {
			strcat(data, " ");
		}
	}

	char zpath[MAX_PATH_LEN];
	determine_path(argv[1], zpath);

	int rc = write_data_to_file(zpath, data, APPEND);
	free(data);

	if (rc < 0) {
		LOG_ERR("Failed to write to %s", zpath);
		return rc;
	}

	LOG_INF("Data written to %s", zpath);
	return 0;
}

/**
 * @brief read – print the contents of a file to the shell.
 *
 *   read <file>
 */
static int read_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(shell, "Usage: read <file>");
		return INVALID;
	}

	char zpath[MAX_PATH_LEN];
	char fatpath[MAX_PATH_LEN];
	determine_path(argv[1], zpath);
	to_fatfs_path(zpath, fatpath, sizeof(fatpath));

	/* Get file size via FatFS stat */
	FILINFO fno;
	FRESULT fr = f_stat(fatpath, &fno);
	if (fr != FR_OK) {
		LOG_ERR("Failed to stat %s: %d", fatpath, fr);
		return -EIO;
	}

	if (fno.fsize == 0) {
		shell_print(shell, "(file is empty)");
		return 0;
	}

	FIL fil;
	fr = f_open(&fil, fatpath, FA_READ);
	if (fr != FR_OK) {
		LOG_ERR("Failed to open %s: %d", fatpath, fr);
		return -EIO;
	}

	char *buffer = malloc(fno.fsize + 1);
	if (!buffer) {
		LOG_ERR("Memory allocation failed");
		f_close(&fil);
		return INVALID;
	}

	UINT br;
	fr = f_read(&fil, buffer, fno.fsize, &br);
	f_close(&fil);

	if (fr != FR_OK) {
		LOG_ERR("Error reading %s: %d", fatpath, fr);
		free(buffer);
		return -EIO;
	}

	buffer[br] = '\0';
	shell_print(shell, "--- %s ---\n%s", zpath, buffer);
	free(buffer);
	return 0;
}

/**
 * @brief pwd – print the current working directory.
 *
 *   pwd
 */
static int pwd_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_print(shell, "Usage: pwd");
		return INVALID;
	}
	shell_print(shell, "%s", current_path);
	return 0;
}

/**
 * @brief logging – start or pause sensor-data logging to a file.
 *
 *   logging s [file]   – start
 *   logging p          – pause
 */
static int logging_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 3 || argc < 2) {
		shell_print(shell, "Usage: logging <s [fname]|p>");
		return INVALID;
	}

	char cmd_type = argv[1][0];
	static char combined_path[MAX_PATH_LEN];

	if (cmd_type == 's') {
		if (argc == 3) {
			determine_path(argv[2], combined_path);
			set_logging_file(combined_path);
		}
		set_logging_state(ON);
	} else if (cmd_type == 'p' && argc == 2) {
		set_logging_state(OFF);
	} else {
		shell_print(shell, "logging s [fname]  – start");
		shell_print(shell, "logging p          – pause");
		return INVALID;
	}

	return 0;
}

/**
 * @brief rm – remove a file.
 *
 *   rm <file>
 */
static int remove_file_cmd(const struct shell *shell, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(shell, "Usage: rm <file>");
		return INVALID;
	}

	char zpath[MAX_PATH_LEN];
	char fatpath[MAX_PATH_LEN];
	determine_path(argv[1], zpath);
	to_fatfs_path(zpath, fatpath, sizeof(fatpath));

	FRESULT fr = f_unlink(fatpath);
	if (fr != FR_OK) {
		LOG_ERR("Failed to remove %s: %d", fatpath, fr);
		return -EIO;
	}

	LOG_INF("Removed: %s", zpath);
	return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THREAD ENTRY POINT
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Thread entry – mount the SD card then register shell commands.
 */
void file_control_thread(void)
{
	int rc = fatfs_mount();
	if (rc < 0) {
		LOG_ERR("Failed to mount FAT32 SD card: %d", rc);
	} else {
		/* Quick free-space sanity check via raw FatFS */
		FATFS *fs_ptr;
		DWORD fre_clust;
		FRESULT fr = f_getfree("SD:", &fre_clust, &fs_ptr);
		if (fr == FR_OK) {
			uint32_t fre_kb = (uint32_t)(fre_clust
					  * fs_ptr->csize / 2);
			LOG_INF("Free space: %u KiB", fre_kb);
		} else {
			LOG_WRN("Unable to read free space: %d", fr);
		}
	}

	LOG_INF("FAT32 file system ready – registering shell commands");

	SHELL_CMD_REGISTER(ls,      NULL, "List files/dirs",            ls_cmd);
	SHELL_CMD_REGISTER(cd,      NULL, "Change directory",           cd_cmd);
	SHELL_CMD_REGISTER(mkdir,   NULL, "Create directory",           mkdir_cmd);
	SHELL_CMD_REGISTER(write,   NULL, "Write data to file",         write_cmd);
	SHELL_CMD_REGISTER(read,    NULL, "Read and print file",        read_cmd);
	SHELL_CMD_REGISTER(pwd,     NULL, "Print working directory",    pwd_cmd);
	SHELL_CMD_REGISTER(logging, NULL, "Start/pause sensor logging", logging_cmd);
	SHELL_CMD_REGISTER(rm,      NULL, "Remove a file",              remove_file_cmd);
}