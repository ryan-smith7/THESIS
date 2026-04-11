/**
 * @file main.c
 * @brief Minimal FAT32 SD card test – mount, write, read back.
 *
 * Expected serial output on success:
 *   [inf] fs_module: SD card mounted at /SD
 *   [inf] main: Write OK
 *   [inf] main: Read OK: Hello from FAT32 SD card!
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include "file.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define TEST_FILE_Z   SD_MOUNT_POINT "/test.txt"   /* Zephyr style – for write_data_to_file */
#define TEST_FILE_F   "SD:/test.txt"               /* FatFS style  – for raw f_open         */
#define TEST_DATA     "Hello from FAT32 SD card!\n"

int main(void)
{
	/* ── 1. Mount ───────────────────────────────────────────────────── */
	int rc = fatfs_mount();
	if (rc < 0) {
		LOG_ERR("Mount failed (%d)", rc);
		return rc;
	}

	/* ── 2. Write via file library ──────────────────────────────────── */
	rc = write_data_to_file(TEST_FILE_Z, TEST_DATA, TRUNC);
	if (rc < 0) {
		LOG_ERR("Write failed (%d)", rc);
		return rc;
	}
	LOG_INF("Write OK");

	/* ── 3. Read back via raw FatFS ─────────────────────────────────── */
	FIL fil;
	FRESULT fr = f_open(&fil, TEST_FILE_F, FA_READ);
	if (fr != FR_OK) {
		LOG_ERR("Open for read failed (%d)", fr);
		return -EIO;
	}

	char buf[64] = {0};
	UINT br;
	fr = f_read(&fil, buf, sizeof(buf) - 1, &br);
	f_close(&fil);

	if (fr != FR_OK) {
		LOG_ERR("Read failed (%d)", fr);
		return -EIO;
	}

	buf[br] = '\0';
	LOG_INF("Read OK: %s", buf);

	return 0;
}