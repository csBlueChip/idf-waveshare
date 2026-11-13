#include "reset.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nv.h"
#include "sd.h"
#include "script.h"

static const char *TAG = "reset";

#define RESET_DIR		SD_MOUNT_POINT "/sys"
#define RESET_FILE		RESET_DIR "/reset.nvram"
#define RESET_DONE		RESET_DIR "/reset.done"
#define RESET_BAD		RESET_DIR "/reset.bad"
#define RESET_NV_NAME		"reset"		/* nvResetImport(): SD:/sys/reset.nvram */
#define RESET_SCR_NAME		"sys/reset"	/* SD:/sys/reset.s */
#define RESET_REBOOT_MS		1500		/* "Rebooting..." stays this long */

void resetBootFile(void)
{
	struct stat st;
	char        name[40];

	sdLock();
	bool have = sdEnsureMounted() == ESP_OK && stat(RESET_FILE, &st) == 0 && S_ISREG(st.st_mode);
	sdUnlock();
	if (!have) {
		return;
	}
	ESP_LOGW(TAG, "/sys/reset.nvram found");
	const char *e = nvBackup(name, sizeof(name));
	if (e) {
		ESP_LOGE(TAG, "backup failed (%s): resetting anyway", e);
	} else {
		ESP_LOGW(TAG, "settings saved to /sys/%s", name);
	}
	e = nvResetImport(RESET_NV_NAME);
	const char *to = e ? RESET_BAD : RESET_DONE;
	sdLock();
	unlink(to);
	bool moved = (rename(RESET_FILE, to) == 0);
	sdUnlock();
	if (e) {
		ESP_LOGE(TAG, "reset.nvram: %s - nothing changed", e);
	} else {
		ESP_LOGW(TAG, "NV reset to defaults + reset.nvram");
	}
	if (!moved) {
		ESP_LOGE(TAG, "could not rename reset.nvram");
	}
}

const char *resetRun(resetMode_t m, bool noSd, void (*msg)(const char *a, const char *b))
{
	char name[40] = "";

	if (m != RESET_NV && (noSd || !scriptHas(RESET_SCR_NAME))) {
		return "no /sys/reset.s";		/* checked first: nothing done */
	}
	if (m != RESET_SCRIPT) {
		char info[48] = "(no backup)";
		if (!noSd) {
			msg("Saving backup...", "");
			const char *e = nvBackup(name, sizeof(name));
			if (e) {
				name[0] = '\0';
				snprintf(info, sizeof(info), "No backup: %s", e);
				ESP_LOGE(TAG, "backup failed (%s): resetting anyway", e);
			} else {
				snprintf(info, sizeof(info), "%s", name);
				ESP_LOGW(TAG, "settings saved to /sys/%s", name);
			}
		}
		msg("Resetting NV...", info);
		const char *e = nvResetAll();
		if (e) {
			return e;
		}
		ESP_LOGW(TAG, "NV reset to defaults");
	}
	if (m != RESET_NV) {
		msg("Running", "/sys/reset.s");
		if (!scriptRunWait(RESET_SCR_NAME, m == RESET_FULL ? "FULL" : "SCRIPT")) {
			return "reset.s did not start";
		}
		msg(NULL, NULL);			/* the script may have drawn over it */
		if (m == RESET_SCRIPT) {
			return NULL;
		}
	}
	msg(name[0] ? name : "NV reset", "Rebooting...");
	vTaskDelay(pdMS_TO_TICKS(RESET_REBOOT_MS));
	esp_restart();
	return NULL;
}
