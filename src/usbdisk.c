#include "usbdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "cmd.h"
#include "img.h"
#include "lcd.h"
#include "led.h"
#include "nv.h"
#include "sd.h"
#include "ui.h"
#include "fm.h"

static const char *TAG = "usbdisk";

#define USB_DISK_REPLY_FLUSH_MS	300	/* let replies reach the client before BLE goes down */
#define USB_DISK_TASK_STACK	4096
#define USB_DISK_TASK_PRIO	5
#define USB_DISK_FAT_PATH	"/usbdisk"	/* used by the library if it remounts for the app */
#define USB_DISK_BACKLIGHT	30		/* % while in disk mode */
#define USB_DISK_IMG_FILE	SD_MOUNT_POINT "/sys/usbdrive.png"	/* replaces the built-in image */
#define USB_DISK_IMG_MAX	(512 * 1024)	/* larger files are ignored */

#define EV_ATTACHED	BIT0	/* PC has enumerated the device */
#define EV_RELEASED	BIT1	/* drive ejected or cable unplugged */

/* Drive image, embedded from assets/usbdrive.png (see CMakeLists.txt) */
extern const uint8_t usbdrive_png_start[] asm("_binary_usbdrive_png_start");
extern const uint8_t usbdrive_png_end[]   asm("_binary_usbdrive_png_end");

static EventGroupHandle_t s_events;
static volatile bool      s_active;

static void        usbDiskTask(void *arg);
static void        usbDiskShowImage(void);
static void        usbDiskOnUsbEvent(tinyusb_event_t *event, void *arg);
static void        usbDiskOnMscEvent(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *event, void *arg);
static void        usbDiskReboot(const char *why);
static const char *usbDiskCmd(int argc, char *argv[], cmdReply_t reply);
static void        usbDiskLoadColour(const char *key, uint8_t rgb[3]);

/* LED activity (disk mode only): set before USB starts, read in the USB task */
static volatile bool s_ledActivity;
static uint8_t       s_readRgb[3], s_writeRgb[3];

/* ================= Public API ================= */

esp_err_t usbDiskInit(void)
{
	s_events = xEventGroupCreate();
	if (!s_events) {
		return ESP_ERR_NO_MEM;
	}
	return cmdRegister("FILES", 0, 2, usbDiskCmd,
	                   "FILES [WEB [STOP|EXIT] | FMEXPORT | SYNEXPORT]  USB disk / web SD manager / export page, colours");
}

esp_err_t usbDiskStart(void)
{
	if (s_active) {
		return ESP_ERR_INVALID_STATE;
	}
	s_active = true;

	if (xTaskCreate(usbDiskTask, "usbdisk", USB_DISK_TASK_STACK, NULL, USB_DISK_TASK_PRIO, NULL) != pdPASS) {
		s_active = false;
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

/* ================= Internals ================= */

static void usbDiskTask(void *arg)
{
	vTaskDelay(pdMS_TO_TICKS(USB_DISK_REPLY_FLUSH_MS));

	/* 1. shut down the user interface */
	uiDisable();

	/* 2. take the card for good - nothing else may touch it until reboot */
	sdLock();

	/* 3. drive image, LED off, backlight down */
	usbDiskShowImage();
	ledOff();
	lcdBacklight(USB_DISK_BACKLIGHT);

	/* 4. hand the raw card to USB; from here card access lights the LED */
	usbDiskLoadColour("disk.read", s_readRgb);
	usbDiskLoadColour("disk.write", s_writeRgb);
	s_ledActivity = true;

	sdmmc_card_t *card = NULL;
	if (sdOpenRaw(&card) != ESP_OK) {
		usbDiskReboot("no SD card");
	}

	tinyusb_msc_driver_config_t mscCfg = {
		.callback     = usbDiskOnMscEvent,
		.callback_arg = NULL,
	};
	tinyusb_msc_storage_config_t storageCfg = {
		.medium.card = card,
		.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
		.fat_fs      = {
			.base_path     = USB_DISK_FAT_PATH,
			.do_not_format = true,			/* never format the user's card */
			.config        = { .max_files = 2 },
		},
	};
	tinyusb_msc_storage_handle_t storage;
	tinyusb_config_t usbCfg = TINYUSB_DEFAULT_CONFIG(usbDiskOnUsbEvent, NULL);

	if (tinyusb_msc_install_driver(&mscCfg) != ESP_OK ||
	    tinyusb_msc_new_storage_sdmmc(&storageCfg, &storage) != ESP_OK ||
	    tinyusb_driver_install(&usbCfg) != ESP_OK) {
		usbDiskReboot("USB start failed");
	}
	ESP_LOGI(TAG, "USB drive ready - waiting for PC");

	/* 5. wait for a PC, then for eject / unplug */
	EventBits_t ev = xEventGroupWaitBits(s_events, EV_ATTACHED, pdFALSE, pdFALSE,
	                                     pdMS_TO_TICKS(USB_DISK_ATTACH_TIMEOUT_MS));
	if (!(ev & EV_ATTACHED)) {
		usbDiskReboot("no PC connected");
	}
	ESP_LOGI(TAG, "PC connected");

	xEventGroupWaitBits(s_events, EV_RELEASED, pdFALSE, pdFALSE, portMAX_DELAY);
	usbDiskReboot("drive released");
}

/* SD:/sys/usbdrive.png read into PSRAM, or NULL (no file, too big, not a PNG
 * or larger than the screen: logged). Caller holds sdLock(); card mounted. */
static uint8_t *usbDiskLoadImage(size_t *len)
{
	struct stat st;
	if (stat(USB_DISK_IMG_FILE, &st) != 0) {
		return NULL;					/* no file: built-in image */
	}
	if (st.st_size <= 0 || st.st_size > USB_DISK_IMG_MAX) {
		ESP_LOGW(TAG, "sys/usbdrive.png: size %ld (max %d) - using built-in image",
		         (long)st.st_size, USB_DISK_IMG_MAX);
		return NULL;
	}
	uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
	FILE    *f   = buf ? fopen(USB_DISK_IMG_FILE, "rb") : NULL;
	size_t   n   = f ? fread(buf, 1, st.st_size, f) : 0;
	if (f) {
		fclose(f);
	}
	int w, h;
	const char *why = !buf ? "no memory"
	                : n != (size_t)st.st_size ? "read failed"
	                : imgPngSize(buf, n, &w, &h) != ESP_OK ? "not a PNG"
	                : (w > lcdWidth() || h > lcdHeight()) ? "larger than the screen"
	                : NULL;
	if (why) {
		ESP_LOGW(TAG, "sys/usbdrive.png: %s - using built-in image", why);
		free(buf);
		return NULL;
	}
	*len = n;
	return buf;
}

/* Black screen, portrait, drive image centred: SD:/sys/usbdrive.png if usable,
 * else the built-in one. Caller holds sdLock() (card not yet handed to USB). */
static void usbDiskShowImage(void)
{
	size_t         len  = 0;
	uint8_t       *file = NULL;
	const uint8_t *png;
	int            w, h;

	lcdSetRotation(0);					/* before reading the screen size */
	if (sdEnsureMounted() == ESP_OK) {
		file = usbDiskLoadImage(&len);
	}
	if (file) {
		png = file;
		ESP_LOGI(TAG, "drive image from sys/usbdrive.png");
	} else {
		png = usbdrive_png_start;
		len = usbdrive_png_end - usbdrive_png_start;
	}
	lcdFill(lcdRgb(0, 0, 0));
	if (imgPngSize(png, len, &w, &h) == ESP_OK) {
		esp_err_t err = imgShowPngMem(png, len, (lcdWidth() - w) / 2, (lcdHeight() - h) / 2);
		if (err != ESP_OK && file) {			/* decode failed: built-in after all */
			ESP_LOGW(TAG, "sys/usbdrive.png: %s - using built-in image", esp_err_to_name(err));
			png = usbdrive_png_start;
			len = usbdrive_png_end - usbdrive_png_start;
			lcdFill(lcdRgb(0, 0, 0));
			if (imgPngSize(png, len, &w, &h) == ESP_OK) {
				imgShowPngMem(png, len, (lcdWidth() - w) / 2, (lcdHeight() - h) / 2);
			}
		}
	}
	free(file);
	lcdUpdate();
}

static void usbDiskOnUsbEvent(tinyusb_event_t *event, void *arg)
{
	switch (event->id) {
	case TINYUSB_EVENT_ATTACHED:
		xEventGroupSetBits(s_events, EV_ATTACHED);
		break;
	case TINYUSB_EVENT_DETACHED:
		if (xEventGroupGetBits(s_events) & EV_ATTACHED) {
			xEventGroupSetBits(s_events, EV_RELEASED);	/* unplugged */
		}
		break;
	default:
		break;
	}
}

/* Storage handed back to the app = PC ejected the drive (or detached). */
static void usbDiskOnMscEvent(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *event, void *arg)
{
	if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE &&
	    event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP &&
	    (xEventGroupGetBits(s_events) & EV_ATTACHED)) {
		xEventGroupSetBits(s_events, EV_RELEASED);
	}
}

static void usbDiskReboot(const char *why)
{
	ESP_LOGI(TAG, "%s - rebooting", why);
	vTaskDelay(pdMS_TO_TICKS(200));
	esp_restart();
}

/* ================= Commands ================= */

const char *usbDiskBegin(void)
{
	if (s_active) {
		return "already in disk mode";
	}
	if (usbDiskStart() != ESP_OK) {
		return "disk mode failed";
	}
	cmdDisable();			/* nothing else may run alongside disk mode */
	return NULL;
}

static const char *usbDiskCmd(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 1 && strcasecmp(argv[0], "FMEXPORT") == 0) {
		size_t      n;
		const char *err = fmExport(&n);
		if (!err) {
			char text[48];
			snprintf(text, sizeof(text), "sys/fm_builtin.html (%u bytes)\n", (unsigned)n);
			reply(text);
		}
		return err;
	}
	if (argc == 1 && strcasecmp(argv[0], "SYNEXPORT") == 0) {
		size_t      n;
		const char *err = fmSynExport(&n);
		if (!err) {
			char text[48];
			snprintf(text, sizeof(text), "sys/syntax_builtin.cfg (%u bytes)\n", (unsigned)n);
			reply(text);
		}
		return err;
	}
	if (argc >= 1) {				/* FILES WEB [STOP|EXIT]: SD manager (fm.c) */
		if (strcasecmp(argv[0], "WEB") != 0 ||
		    (argc == 2 && strcasecmp(argv[1], "STOP") != 0 && strcasecmp(argv[1], "EXIT") != 0)) {
			return "usage: FILES [WEB [STOP|EXIT] | FMEXPORT | SYNEXPORT]";
		}
		if (argc == 2) {
			if (!fmActive()) {
				return "SD manager not active";
			}
			fmStop();
			return NULL;
		}
		const char *err = fmStart();
		if (!err) {
			reply("<<GOTO /fm>>\n");	/* the console page opens (or re-opens) the manager */
		}
		return err;
	}
	const char *err = usbDiskBegin();
	if (!err) {
		reply("Disk Mode Enabled\n");
	}
	return err;
}

/* ================= LED activity =================
 * The linker wraps the card's sector read/write (see CMakeLists.txt: --wrap),
 * so every access - from USB or FAT - comes through here. Only in disk mode
 * is the LED lit: colour for the duration of the access, then off. */

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start, size_t count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start, size_t count);
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start, size_t count);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start, size_t count);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start, size_t count)
{
	if (!s_ledActivity) {
		return __real_sdmmc_read_sectors(card, dst, start, count);
	}
	ledSetRgb(s_readRgb[0], s_readRgb[1], s_readRgb[2]);
	esp_err_t err = __real_sdmmc_read_sectors(card, dst, start, count);
	ledOff();
	return err;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start, size_t count)
{
	if (!s_ledActivity) {
		return __real_sdmmc_write_sectors(card, src, start, count);
	}
	ledSetRgb(s_writeRgb[0], s_writeRgb[1], s_writeRgb[2]);
	esp_err_t err = __real_sdmmc_write_sectors(card, src, start, count);
	ledOff();
	return err;
}

/* NV "r g b" -> rgb[3] (the value is already checked by NV). */
static void usbDiskLoadColour(const char *key, uint8_t rgb[3])
{
	int r = 0, g = 0, b = 0;
	sscanf(nvGet(key), "%d %d %d", &r, &g, &b);
	rgb[0] = r;
	rgb[1] = g;
	rgb[2] = b;
}
