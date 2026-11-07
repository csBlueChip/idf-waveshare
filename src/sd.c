#include "sd.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sd";

static sdmmc_card_t     *s_card;	/* FAT-mounted card (NULL when not mounted) */
static SemaphoreHandle_t s_lock;

static void      sdConfig(sdmmc_host_t *host, sdmmc_slot_config_t *slot);
static esp_err_t sdMount(void);
static void      sdUnmount(void);
static int       sdCompare(const void *a, const void *b);

/* ================= Public API ================= */

esp_err_t sdInit(void)
{
	s_lock = xSemaphoreCreateRecursiveMutex();
	if (!s_lock) {
		return ESP_ERR_NO_MEM;
	}
	if (sdMount() != ESP_OK) {
		ESP_LOGW(TAG, "no card - will retry on use");
	}
	return ESP_OK;
}

bool sdFatDrive(char drv[4])
{
	if (!s_card) {
		return false;
	}
	BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
	if (pdrv == 0xFF) {
		return false;
	}
	drv[0] = (char)('0' + pdrv);
	drv[1] = ':';
	drv[2] = '\0';
	return true;
}

void sdLock(void)
{
	xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void sdUnlock(void)
{
	xSemaphoreGiveRecursive(s_lock);
}

/* No card-detect pin: a mounted card is asked for its status (CMD13, ~1 ms)
 * so a card pulled since the last use is noticed - unmounted, then a mount is
 * tried (a card swapped in gets mounted). Caller holds sdLock(). */
esp_err_t sdEnsureMounted(void)
{
	if (s_card && sdmmc_get_status(s_card) != ESP_OK) {
		ESP_LOGW(TAG, "card gone - unmounting");
		sdUnmount();
	}
	return s_card ? ESP_OK : sdMount();
}

bool sdMounted(void)
{
	return s_card != NULL;
}

esp_err_t sdInfo(uint64_t *total, uint64_t *free)
{
	sdLock();
	esp_err_t err = sdEnsureMounted();
	if (err == ESP_OK) {
		err = esp_vfs_fat_info(SD_MOUNT_POINT, total, free);
	} else {
		err = ESP_ERR_NOT_FOUND;
	}
	sdUnlock();
	return err;
}

int sdListFiles(const char *path, const char *ext, char (*names)[SD_NAME_MAX], int max)
{
	DIR *dir = opendir(path);
	if (!dir) {
		DIR *root = opendir(SD_MOUNT_POINT);
		if (root) {				/* card fine, folder missing: no files */
			closedir(root);
			return 0;
		}
		ESP_LOGW(TAG, "card not readable - unmounting");
		sdUnmount();
		return -1;
	}

	size_t extLen = strlen(ext);
	int    n      = 0;

	struct dirent *e;
	while ((e = readdir(dir)) != NULL && n < max) {
		size_t len = strlen(e->d_name);
		if (e->d_type != DT_REG || len <= extLen || len >= SD_NAME_MAX) {
			continue;
		}
		if (strcasecmp(e->d_name + len - extLen, ext) != 0) {
			continue;
		}
		memcpy(names[n++], e->d_name, len + 1);
	}
	closedir(dir);

	qsort(names, n, SD_NAME_MAX, sdCompare);
	return n;
}

esp_err_t sdOpenRaw(sdmmc_card_t **out)
{
	sdUnmount();

	sdmmc_host_t        host;
	sdmmc_slot_config_t slot;
	sdConfig(&host, &slot);

	sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
	if (!card) {
		return ESP_ERR_NO_MEM;
	}

	esp_err_t err = sdmmc_host_init();
	if (err == ESP_OK) {
		err = sdmmc_host_init_slot(host.slot, &slot);
	}
	if (err == ESP_OK) {
		err = sdmmc_card_init(&host, card);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "raw open failed: %s", esp_err_to_name(err));
		sdmmc_host_deinit();
		free(card);
		return err;
	}

	*out = card;
	return ESP_OK;
}

/* ================= Internals ================= */

static void sdConfig(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
	*host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();		/* 20 MHz */

	*slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
	slot->width  = 4;
	slot->clk    = SD_PIN_CLK;
	slot->cmd    = SD_PIN_CMD;
	slot->d0     = SD_PIN_D0;
	slot->d1     = SD_PIN_D1;
	slot->d2     = SD_PIN_D2;
	slot->d3     = SD_PIN_D3;
	slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static esp_err_t sdMount(void)
{
	esp_vfs_fat_sdmmc_mount_config_t mountCfg = {
		.format_if_mount_failed = false,
		.max_files              = 4,
		.allocation_unit_size   = 16 * 1024,
	};

	sdmmc_host_t        host;
	sdmmc_slot_config_t slot;
	sdConfig(&host, &slot);

	esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mountCfg, &s_card);
	if (err != ESP_OK) {
		s_card = NULL;
		ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "mounted %s, %llu MB", s_card->cid.name,
	         ((unsigned long long)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
	return ESP_OK;
}

static void sdUnmount(void)
{
	if (s_card) {
		esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
		s_card = NULL;
	}
}

static int sdCompare(const void *a, const void *b)
{
	return strcasecmp((const char *)a, (const char *)b);
}
