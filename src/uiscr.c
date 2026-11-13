#include "uiscr.h"

#include <stdio.h>
#include <string.h>

#include "font.h"
#include "img.h"
#include "imu.h"
#include "lcd.h"
#include "nv.h"
#include "scrtr.h"
#include "sd.h"
#include "ser.h"
#include "telnet.h"
#include "web.h"

#define UISCR_FONT		1
#define UISCR_MARGIN		10	/* both sides: keeps clear of the boot square */
#define UISCR_FADE_OUT_MS	200	/* to black: UI switch, disconnect, before the splash */
#define UISCR_FADE_IN_MS	200	/* Wait details */
#define UISCR_SPLASH_FADE_MS	800	/* connect splash */
#define UISCR_LINK_INK		lcdRgb(255, 255, 255)
#define UISCR_ROW_LINK		4
#define UISCR_INK		lcdRgb(220, 110, 20)
#define UISCR_PAPER		lcdRgb(0, 0, 0)
#define UISCR_ROW_PWD		2
#define UISCR_HIDDEN		"(hold BOOT)"
#define UISCR_SPLASH		SD_MOUNT_POINT "/sys/splash.png"
#define UISCR_SPLASH_NAME	"sys/splash.png"
#define UISCR_ROW_RSTMODE	1
#define UISCR_ROW_RSTMSG	3	/* and the row below */

static bool   s_shown;		/* details screen is up */
static bool   s_link;		/* Wi-Fi station with an IP */
static uiIf_t s_ifc;
static int    s_rstMode;	/* RESET! screen: selected mode */
static bool   s_rstNoSd;

static void uiScrLine(int row, const char *prefix, const char *value, uint16_t valueInk);
static void uiScrFade(int ms);
static void uiScrLinkLine(void);
static void uiScrWifi(const char *name, int port);
static void uiBleOnStart(void);
static void uiTelnetOnStart(void);
static void uiWebOnStart(void);
static void uiUartOnStart(void);
static void uiUsbOnStart(void);
static void uiMscOnStart(void);
static void uiResetOnStart(void);
static void uiScrModeLine(void);

/* ================= Public API ================= */

void uiScrStart(uiIf_t ifc)
{
	void (*onStart)(void) = NULL;
	switch (ifc) {
	case UI_BLE:    onStart = uiBleOnStart; break;
	case UI_TELNET: onStart = uiTelnetOnStart; break;
	case UI_WEB:    onStart = uiWebOnStart; break;
	case UI_UART:   onStart = uiUartOnStart; break;
	case UI_USB:    onStart = uiUsbOnStart; break;
	case UI_MSC:    onStart = uiMscOnStart; break;
	case UI_RESET:  onStart = uiResetOnStart; break;
	default:        return;
	}

	/* Wait: ROT 1|3 ; SCR AUTO 0 ; SCR TR FADE 200 ; <details> ; SCR UPD ;
	 * IMU ORIENT L AUTO (details turn 180 deg with the device) */
	uint8_t rot = lcdGetRotation();
	if ((rot & 1) == 0) {
		lcdSetRotation(rot + 1);		/* landscape: 0 -> 1, 2 -> 3 */
	}
	lcdSetAutoUpdate(false);
	onStart();
	s_ifc   = ifc;
	s_shown = true;
	uiScrFade(UISCR_FADE_IN_MS);
	imuOrientSet(IMU_OR_L, true);
}

void uiScrFm(const char *url)
{
	/* fade to black ; ROT 1|3 ; SCR AUTO 0 ; SD manager lines ; fade in */
	s_shown = false;				/* not a Wait screen: no BOOT gestures */
	lcdSetAutoUpdate(true);
	lcdCls(0, 0, 0);
	uiScrFade(UISCR_FADE_OUT_MS);
	uint8_t rot = lcdGetRotation();
	if ((rot & 1) == 0) {
		lcdSetRotation(rot + 1);		/* landscape, as the Wait screen */
	}
	lcdSetAutoUpdate(false);
	uiScrLine(0, "SD CARD MANAGER", "", UISCR_INK);
	uiScrLine(1, "URL : ", url, UISCR_INK);
	uiScrLine(2, "BOOT: press to exit", "", UISCR_INK);
	uiScrFade(UISCR_FADE_IN_MS);
}

void uiScrBoot(void)
{
	/* SCR AUTO 1 ; SCR TR CUT ; CLS 0 0 0 ; BL scr.bl ; boot square (instant) */
	lcdSetAutoUpdate(true);
	lcdCls(0, 0, 0);
	lcdBacklight(nvGetInt("scr.bl"));
	lcdBootMarker();
	lcdUpdateNoTr();
}

void uiScrDisconnect(uiIf_t ifc)
{
	/* SCR AUTO 1 ; SCR TR FADE 200 ; CLS 0 0 0 ; then Wait */
	s_shown = false;
	lcdSetAutoUpdate(true);
	lcdCls(0, 0, 0);
	uiScrFade(UISCR_FADE_OUT_MS);
	uiScrStart(ifc);
}

void uiScrConnect(void)
{
	/* IMU ORIENT NONE (view flip off) ; SCR AUTO 0 ; ROT 0 ; SCR TR FADE 200 ;
	 * CLS 0 0 0 ; SCR UPD ; IMG splash ; SCR TR FADE 800 ; SCR UPD ; SCR AUTO 1 */
	s_shown = false;
	imuOrientSet(IMU_OR_OFF, false);
	lcdSetAutoUpdate(false);
	lcdSetRotation(0);
	lcdCls(0, 0, 0);
	uiScrFade(UISCR_FADE_OUT_MS);

	bool ok = false;
	sdLock();
	if (sdEnsureMounted() == ESP_OK) {
		ok = (imgShowPng(UISCR_SPLASH, 0, 0) == ESP_OK);
	}
	sdUnlock();
	if (!ok) {
		lcdDrawText(fontGet(1), 2, 6, UISCR_SPLASH_NAME, UISCR_INK, UISCR_PAPER);	/* 168 px: just fits */
	}
	uiScrFade(UISCR_SPLASH_FADE_MS);
	lcdSetAutoUpdate(true);
}

void uiScrPassword(bool show)
{
	if (!uiScrWantsButton()) {
		return;
	}
	uiScrLine(UISCR_ROW_PWD, "Pwd : ", show ? nvGet("wifi.pwd") : UISCR_HIDDEN, UISCR_INK);
	lcdUpdateNoTr();
}

void uiScrSetLink(bool up)
{
	s_link = up;
}

void uiScrLink(bool up)
{
	s_link = up;
	if (uiScrWantsButton()) {			/* Wi-Fi details screen is up */
		uiScrLinkLine();
		lcdUpdateNoTr();
	}
}

bool uiScrWantsButton(void)
{
	return s_shown && (s_ifc == UI_TELNET || s_ifc == UI_WEB);
}

bool uiScrWaiting(void)
{
	return s_shown;
}

void uiScrResetSet(int mode, bool noSd)
{
	s_rstMode  = mode;
	s_rstNoSd  = noSd;
}

void uiScrResetMode(void)
{
	if (s_shown && s_ifc == UI_RESET) {
		uiScrModeLine();
		lcdUpdateNoTr();
	}
}

void uiScrResetMsg(const char *a, const char *b)
{
	if (s_shown && s_ifc == UI_RESET) {
		uiScrLine(UISCR_ROW_RSTMSG, "", a, UISCR_LINK_INK);
		uiScrLine(UISCR_ROW_RSTMSG + 1, "", b, UISCR_LINK_INK);
		lcdUpdateNoTr();
	}
}

/* Update the screen with a FADE of ms (firmware one-off; SCR TR/TRP untouched). */
static void uiScrFade(int ms)
{
	scrtrOverride("FADE", ms, 0);
	lcdUpdate();
	scrtrOverride(NULL, 0, 0);			/* unused if nothing changed */
}

/* ================= Per-interface onStart ================= */

static void uiBleOnStart(void)
{
	uiScrLine(0, "UI  : ", "BLE", UISCR_LINK_INK);
	uiScrLine(1, "Name: ", nvGet("ble.name"), UISCR_INK);
}

static void uiTelnetOnStart(void)
{
	uiScrWifi("TELNET", nvGetInt("telnet.port"));
}

static void uiWebOnStart(void)
{
	uiScrWifi("WEB", nvGetInt("web.port"));
}

static void uiUartOnStart(void)
{
	char text[24];
	uiScrLine(0, "UI  : ", "UART", UISCR_LINK_INK);
	uiScrLine(1, "Baud: ", nvGet("ser.baud"), UISCR_INK);
	snprintf(text, sizeof(text), "TX%d RX%d", SER_UART_TX_GPIO, SER_UART_RX_GPIO);
	uiScrLine(2, "Pins: ", text, UISCR_INK);
}

static void uiUsbOnStart(void)
{
	uiScrLine(0, "UI  : ", "USB", UISCR_LINK_INK);
}

static void uiMscOnStart(void)
{
	uiScrLine(0, "UI  : ", "MSC - Flash Drive", UISCR_LINK_INK);
	uiScrLine(1, "BOOT: ", "hold to start", UISCR_LINK_INK);
}

static void uiResetOnStart(void)
{
	s_ifc = UI_RESET;				/* uiScrModeLine() is drawn before s_ifc is set */
	uiScrLine(0, "UI  : ", "RESET!", UISCR_LINK_INK);
	uiScrModeLine();
	uiScrLine(2, "BOOT: ", "TAP=mode, LONG=OK", UISCR_INK);
}

/* ================= Internals ================= */

/* MODE: NVRAM | Script | FULL (the selected one, in white), or
 * MODE: NVRAM - no backup (no SD card). */
static void uiScrModeLine(void)
{
	static const char *const names[] = { "NVRAM", "Script", "FULL" };
	const char *m = s_rstNoSd ? "NVRAM - no backup" : names[(unsigned)s_rstMode < 3 ? s_rstMode : 0];
	uiScrLine(UISCR_ROW_RSTMODE, "MODE: ", m, UISCR_LINK_INK);
}

static void uiScrWifi(const char *name, int port)
{
	char text[48];
	snprintf(text, sizeof(text), "%s:%d", name, port);
	uiScrLine(0, "UI  : ", text, UISCR_LINK_INK);	/* the interface in white */
	uiScrLine(1, "SSID: ", nvGet("wifi.ssid"), UISCR_INK);
	uiScrLine(UISCR_ROW_PWD, "Pwd : ", UISCR_HIDDEN, UISCR_INK);
	snprintf(text, sizeof(text), "%s %s", nvGet("wifi.ip"), nvGet("wifi.cidr"));
	uiScrLine(3, "IP  : ", text, UISCR_INK);
	uiScrLinkLine();
}

/* WiFi: Waiting... | Link Active (in white) */
static void uiScrLinkLine(void)
{
	if (s_link) {
		uiScrLine(UISCR_ROW_LINK, "WiFi: ", "Link Active", UISCR_LINK_INK);
	} else {
		uiScrLine(UISCR_ROW_LINK, "WiFi: ", "Waiting...", UISCR_INK);
	}
}

/* Clear text row 'row' and draw prefix (orange) + value (valueInk), cut to the
 * screen width with '~'. */
static void uiScrLine(int row, const char *prefix, const char *value, uint16_t valueInk)
{
	const font_t *f     = fontGet(UISCR_FONT);
	int           lineH = f->height + f->lead;
	int           y     = UISCR_MARGIN + row * lineH;
	int           avail = lcdWidth() - 2 * UISCR_MARGIN;	/* 300 px landscape = 25 chars */
	char          text[128];

	snprintf(text, sizeof(text), "%s%s", prefix, value);

	int len = strlen(text), w = 0;
	for (int i = 0; i < len; i++) {
		w += fontCharWidth(f, text[i]);
	}
	if (w > avail) {
		int tilde = fontCharWidth(f, '~');
		while (len > 0 && w + tilde > avail) {
			w -= fontCharWidth(f, text[--len]);
		}
		text[len++] = '~';
		text[len]   = '\0';
	}

	lcdFillRect(UISCR_MARGIN, y, avail, lineH, UISCR_PAPER);

	int  plen = strlen(prefix);
	char save = 0;
	if (plen < len) {				/* split: prefix, then value in its colour */
		save       = text[plen];
		text[plen] = '\0';
	}
	lcdDrawText(f, UISCR_MARGIN, y, text, UISCR_INK, UISCR_PAPER);
	if (save) {
		int x = UISCR_MARGIN;
		for (int i = 0; i < plen; i++) {
			x += fontCharWidth(f, text[i]);
		}
		text[plen] = save;
		lcdDrawText(f, x, y, text + plen, valueInk, UISCR_PAPER);
	}
}
