#include "esp_log.h"
#include "nvs_flash.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmd.h"
#include "led.h"
#include "lcd.h"
#include "gfx.h"
#include "text.h"
#include "font.h"
#include "i2c.h"
#include "imu.h"
#include "sd.h"
#include "img.h"
#include "usbdisk.h"
#include "ui.h"
#include "btn.h"
#include "nv.h"
#include "bat.h"
#include "sys.h"
#include "ser.h"
#include "log.h"
#include "fm.h"
#include "script.h"
#include "reset.h"

static const char *TAG = "main";

/* ---------- System ---------- */
static void sysNvsInit(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
}

/* ---------- App ---------- */
static void appRun(void)
{
	char line[CMD_LINE_MAX + 1];

	for (;;) {
		uiReadLine(line, sizeof(line));		/* blocks, no CPU */
		ESP_LOGI(TAG, "rx: %s", line);
		scriptConsoleLine(line);		/* while a script runs: STOP / PAUSE / INPUT only */
	}
}

#define MAIN_AR_FLASH_MS	100

/* Start-up LED flash: NV sys.aron if sys.autorun is 1, else sys.aroff;
 * 0 0 0 = no flash (and no delay). */
static void mainAutorunFlash(void)
{
	int r = 0, g = 0, b = 0;
	sscanf(nvGet(nvGetInt("sys.autorun") ? "sys.aron" : "sys.aroff"), "%d %d %d", &r, &g, &b);
	if (r || g || b) {
		ledSetRgb(r, g, b);
		vTaskDelay(pdMS_TO_TICKS(MAIN_AR_FLASH_MS));
		ledOff();
	}
}

void app_main(void)
{
	sysNvsInit();
	ESP_ERROR_CHECK(cmdInit());
	ESP_ERROR_CHECK(nvInit());
	ESP_ERROR_CHECK(serInit(uiSerRx));
	ESP_ERROR_CHECK(logInit());
	ESP_ERROR_CHECK(sysInit());
	ESP_ERROR_CHECK(ledInit());
	ESP_ERROR_CHECK(sdInit());
	resetBootFile();				/* SD:/sys/reset.nvram: backup, defaults, import */
	mainAutorunFlash();				/* autorun state: sys.aron / sys.aroff */
	logLoadCfg();					/* SD:/sys/debug.cfg: log volume per device */
	ESP_ERROR_CHECK(lcdInit());
	ESP_ERROR_CHECK(gfxInit());
	ESP_ERROR_CHECK(textInit());
	ESP_ERROR_CHECK(fontInit());
	ESP_ERROR_CHECK(i2cInit());
	ESP_ERROR_CHECK(imuInit());
	ESP_ERROR_CHECK(batInit());
	ESP_ERROR_CHECK(imgInit());
	ESP_ERROR_CHECK(usbDiskInit());
	ESP_ERROR_CHECK(fmInit());
	ESP_ERROR_CHECK(uiInit());
	ESP_ERROR_CHECK(btnInit());
	ESP_ERROR_CHECK(scriptInit());
	scriptBoot();					/* onBoot (NV sys.autorun 1) */
	uiBegin();					/* interface, Wait screen, serial banner */
	appRun();
}
