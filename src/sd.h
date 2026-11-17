#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

/*
 * microSD card on the 4-bit SDMMC bus, FAT32, mounted at SD_MOUNT_POINT.
 */

#define SD_MOUNT_POINT	"/sdcard"
#define SD_NAME_MAX	64		/* max file name length incl. NUL */
#define SD_BASE_MAX	31		/* restricted names (as scripts): without extension */
#define SD_NAME_ERR	"name: letters, digits, '_' (max 31)"

#define SD_PIN_CLK	14
#define SD_PIN_CMD	15
#define SD_PIN_D0	16
#define SD_PIN_D1	18
#define SD_PIN_D2	17
#define SD_PIN_D3	21

/* Create the lock and try to mount. A missing card is not an error (logged only). */
esp_err_t sdInit(void);

/* Card access lock (recursive). Hold it around any file I/O on the card. */
void sdLock(void);
void sdUnlock(void);

/* ESP_OK when a card is available: a mounted card is checked (CMD13, ~1 ms;
 * there is no card-detect pin) and unmounted if it no longer answers; an
 * unmounted card is mounted. Hold sdLock(). */
esp_err_t sdEnsureMounted(void);

/* FatFs drive of the mounted card ("0:" ...) for direct f_*() calls, e.g. to
 * read FAT attributes. false if not mounted. Caller holds sdLock(). */
bool sdFatDrive(char drv[4]);

bool sdMounted(void);

/* Card size and free space in bytes (mounts if needed). ESP_ERR_NOT_FOUND if no card. */
esp_err_t sdInfo(uint64_t *total, uint64_t *free);

/* File name for a new file, restricted as script names (scripts may parse
 * it): letters, digits, '_', 1..SD_BASE_MAX chars, then ext (e.g. ".csv"),
 * which may already be typed (any case; not counted). out = name + ext.
 * Returns NULL or SD_NAME_ERR. */
const char *sdNameMake(const char *name, const char *ext, char *out, size_t n);

/* List regular files in folder path (full path, e.g. SD_MOUNT_POINT "/images")
 * whose name ends with ext (case-insensitive), sorted alphabetically ignoring
 * case. Names keep their on-card casing. A missing folder gives 0. Returns the
 * count, or -1 if the card can't be read (it is then unmounted so the next
 * sdEnsureMounted() retries). */
int sdListFiles(const char *path, const char *ext, char (*names)[SD_NAME_MAX], int max);

/* As sdListFiles, without a limit: every matching file with its size and
 * read-only flag, in a PSRAM array (*out, the caller frees it; NULL when
 * there are none). Caller holds sdLock() with the card mounted. Returns the
 * count, -1 = card not readable (unmounted), -2 = out of memory. */
typedef struct {
	char name[SD_NAME_MAX];			/* first: sorts like a name */
	long size;
	bool ro;
} sdEntry_t;
int sdListAll(const char *path, const char *ext, sdEntry_t **out);

/* Unmount FAT and re-open the card as a raw block device (e.g. for USB MSC).
 * Caller must hold sdLock(). The FAT mount is not restored - intended for
 * disk mode, which ends in a reboot. */
esp_err_t sdOpenRaw(sdmmc_card_t **card);
