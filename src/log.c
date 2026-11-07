#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cmd.h"
#include "sd.h"

#define LOG_LINE_MAX	256	/* ESP_LOGx line (longer is cut) */
#define DBGF_MAX	200	/* dbgf text */
#define LOG_LOCK_MS	100	/* busy: fall back to the console */
#define LOG_CFG_DIR	SD_MOUNT_POINT "/sys"
#define LOG_CFG		LOG_CFG_DIR "/debug.cfg"
#define LOG_CFG_MAX	4096	/* debug.cfg size limit */
#define LOG_VOL_FULL	4

/* Devices (debug.cfg / LOG DEBUG): groups of log tags. A tag ending in '*'
 * matches by prefix; case is ignored. Any other tag belongs to SYS. */
typedef struct {
	const char *name;
	const char *tags;	/* space-separated */
} logDev_t;

static const logDev_t s_devs[] = {
	{ "BLE",    "ble* nimble* BLE_INIT BTDM_INIT BT_* hci*" },
	{ "WIFI",   "wifi* pp net80211 phy* btbb_init esp_netif* lwip* dhcps" },
	{ "WEB",    "web httpd* esp_http_server dns" },
	{ "SD",     "sd* SD_* diskio* vfs_fat* usbdisk tusb* TinyUSB" },
	{ "SCRIPT", "script" },
	{ "IMU",    "imu" },
	{ "SYS",    "" },		/* last: everything else */
};
#define LOG_DEVS	(int)(sizeof(s_devs) / sizeof(s_devs[0]))

static const char s_cfgHead[] =
	"# debug.cfg - log volume per device, read at boot (no file: all 4)\n"
	"#   0 off, 1 errors, 2 +warnings, 3 +info, 4 +DBG lines (full volume)\n"
	"# Devices group the log tags:\n"
	"#   BLE     ble NimBLE BLE_INIT BTDM_INIT BT_* hci*\n"
	"#   WIFI    wifi pp net80211 phy* btbb_init esp_netif* lwip* dhcps\n"
	"#   WEB     web httpd* esp_http_server dns\n"
	"#   SD      sd sdmmc* SD_* diskio* vfs_fat* usbdisk tusb* TinyUSB\n"
	"#   SCRIPT  script\n"
	"#   IMU     imu\n"
	"#   SYS     every other tag\n"
	"# LOG DEBUG device n SAVE changes a line here; LOG DEBUG SAVE rewrites\n"
	"# the file with all current settings (the old one is shown first).\n"
	"# Messages before the SD card is mounted (ROM, bootloader, early\n"
	"# start-up) are not filtered.\n";

static volatile uint8_t s_vol[LOG_DEVS] = { 4, 4, 4, 4, 4, 4, 4 };	/* 0 off .. 4 full */

static SemaphoreHandle_t s_lock;		/* s_buf */
static char              s_buf[LOG_LINE_MAX + 2];
static vprintf_like_t    s_console;		/* the original output */
static volatile int      s_uiPort = -1;
static volatile bool     s_debug;
static volatile int      s_mode = LOG_AUTO;	/* LOG command, this boot only */

static const char *const s_modeNames[] = { "AUTO", "UART", "USB", "BOTH" };

static int  logVprintf(const char *fmt, va_list ap);
static void logOut(const char *text, size_t len);
static bool logShow(const char *line);
static int  logDevFind(const char *name, size_t len);
static const char *logCfgParse(char *text, bool apply);
static const char *logCfgSave(int dev, int vol);
static const char *logCfgSaveAll(cmdReply_t reply);
static const char *logCmd(int argc, char *argv[], cmdReply_t reply);
static const char *logCmdDebug(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t logInit(void)
{
	s_lock = xSemaphoreCreateMutex();
	if (!s_lock) {
		return ESP_ERR_NO_MEM;
	}
	s_console = esp_log_set_vprintf(logVprintf);
	return cmdRegister("LOG", 0, 4, logCmd,
	                   "LOG [UART|USB|BOTH|AUTO] | DEBUG [SAVE | device|ALL [0..4 [SAVE]]]  log port(s) / volume");
}

/* Read SD:/sys/debug.cfg (no file: all devices at 4). Call after sdInit(). */
void logLoadCfg(void)
{
	static EXT_RAM_BSS_ATTR char text[LOG_CFG_MAX + 1];
	size_t n = 0;

	sdLock();
	if (sdEnsureMounted() == ESP_OK) {
		FILE *f = fopen(LOG_CFG, "rb");
		if (f) {
			n = fread(text, 1, LOG_CFG_MAX, f);
			fclose(f);
		}
	}
	sdUnlock();
	text[n] = '\0';
	const char *err = logCfgParse(text, true);
	if (err) {
		ESP_LOGW("log", "debug.cfg: %s (line skipped)", err);
	}
}

void logSetUiPort(int port)
{
	s_uiPort = port;
	s_debug  = false;
}

void logSetDebug(bool on)
{
	s_debug = on;
}

bool logGetDebug(void)
{
	return s_debug;
}

void logSetMode(logMode_t mode)
{
	if (mode >= LOG_AUTO && mode <= LOG_BOTH) {
		s_mode = mode;
	}
}

logMode_t logGetMode(void)
{
	return (logMode_t)s_mode;
}

void dbgf(const char *fmt, ...)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOG_LOCK_MS)) != pdTRUE) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(s_buf, DBGF_MAX + 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		n = 0;
	} else if (n > DBGF_MAX) {
		n = DBGF_MAX;
	}
	s_buf[n++] = '\n';
	logOut(s_buf, n);
	xSemaphoreGive(s_lock);
}

/* ================= Internals ================= */

/* esp_log output: format, then send by the routing rule. Interrupts, the
 * time before the scheduler and a busy buffer use the original console. */
static int logVprintf(const char *fmt, va_list ap)
{
	if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
	    xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOG_LOCK_MS)) != pdTRUE) {
		return s_console(fmt, ap);
	}
	int n = vsnprintf(s_buf, LOG_LINE_MAX + 1, fmt, ap);
	if (n > LOG_LINE_MAX) {
		n = LOG_LINE_MAX;
		s_buf[n++] = '\n';				/* cut: keep the line end */
	}
	if (n > 0 && logShow(s_buf)) {
		logOut(s_buf, n);
	}
	xSemaphoreGive(s_lock);
	return n;
}

/* Volume filter. An esp_log line is "L (time) tag: text" (L = E W I D V):
 * shown if its level (E 1, W 2, I 3, D/V 4; text starting "DBG " 4) is at
 * most the volume of the tag's device. Other lines always show. */
static bool logShow(const char *line)
{
	static const char lv[] = "EWIDV";
	const char *l = strchr(lv, line[0]);
	if (!l || line[0] == '\0' || line[1] != ' ' || line[2] != '(') {
		return true;
	}
	const char *t = strchr(line, ')');
	if (!t || t[1] != ' ') {
		return true;
	}
	t += 2;
	const char *c = strchr(t, ':');
	if (!c) {
		return true;
	}
	int level = (int)(l - lv) + 1;
	if (level > 4) {
		level = 4;
	}
	if (strncmp(c + 1, " DBG ", 5) == 0) {
		level = 4;
	}
	int    dev = LOG_DEVS - 1;
	size_t tl  = c - t;
	for (int d = 0; d < LOG_DEVS - 1 && dev == LOG_DEVS - 1; d++) {
		for (const char *p = s_devs[d].tags; *p;) {
			size_t pl = strcspn(p, " ");
			bool   pre = pl && p[pl - 1] == '*';
			size_t ml  = pre ? pl - 1 : pl;
			if (pre ? (tl >= ml && strncasecmp(t, p, ml) == 0) : (tl == ml && strncasecmp(t, p, ml) == 0)) {
				dev = d;
				break;
			}
			p += pl;
			p += strspn(p, " ");
		}
	}
	return level <= s_vol[dev];
}

static int logDevFind(const char *name, size_t len)
{
	for (int d = 0; d < LOG_DEVS; d++) {
		if (strlen(s_devs[d].name) == len && strncasecmp(name, s_devs[d].name, len) == 0) {
			return d;
		}
	}
	return -1;
}

/* debug.cfg text: "DEVICE=n" lines, '#' comments, blank lines. apply: set
 * the volumes. Bad lines are skipped; returns the first problem (static
 * text) or NULL. The text is changed (cut at '#'). */
static const char *logCfgParse(char *text, bool apply)
{
	static char err[64];
	const char *ret  = NULL;
	int         no   = 0;
	char       *save = NULL;

	for (char *ln = strtok_r(text, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		no++;
		char *h = strchr(ln, '#');
		if (h) {
			*h = '\0';
		}
		while (*ln == ' ' || *ln == '\t') {
			ln++;
		}
		char *e = ln + strlen(ln);
		while (e > ln && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
			*--e = '\0';
		}
		if (*ln == '\0') {
			continue;
		}
		char  *eq = strchr(ln, '=');
		size_t nl = eq ? (size_t)(eq - ln) : 0;
		while (nl > 0 && (ln[nl - 1] == ' ' || ln[nl - 1] == '\t')) {
			nl--;
		}
		int   d = eq ? logDevFind(ln, nl) : -1;
		char *v = eq ? eq + 1 : NULL;
		while (v && (*v == ' ' || *v == '\t')) {
			v++;
		}
		if (d < 0 || !v || v[0] < '0' || v[0] > '4' || v[1] != '\0') {
			if (!ret) {
				snprintf(err, sizeof(err), "line %d: expected DEVICE=0..4", no);
				ret = err;
			}
			continue;
		}
		if (apply) {
			s_vol[d] = v[0] - '0';
		}
	}
	return ret;
}

/* LOG DEBUG ... SAVE: replace the device's line in debug.cfg (comments and
 * other lines kept), or add it; file and folder created if missing. */
static const char *logCfgSave(int dev, int vol)
{
	static EXT_RAM_BSS_ATTR char in[LOG_CFG_MAX + 1];
	static EXT_RAM_BSS_ATTR char out[LOG_CFG_MAX + 64];
	size_t      n = 0, o = 0;
	bool        done = false;
	const char *err  = NULL;

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	FILE *f = fopen(LOG_CFG, "rb");
	if (f) {
		n = fread(in, 1, LOG_CFG_MAX + 1, f);
		fclose(f);
		if (n > LOG_CFG_MAX) {
			sdUnlock();
			return "debug.cfg too big";
		}
	}
	in[n] = '\0';
	for (char *ln = in; *ln;) {
		size_t ll = strcspn(ln, "\n");
		char  *p  = ln + strspn(ln, " \t");
		size_t nl = strcspn(p, " \t=#\r\n");
		char  *q  = p + nl + strspn(p + nl, " \t");
		if (!done && *q == '=' && logDevFind(p, nl) == dev) {
			o += snprintf(out + o, sizeof(out) - o, "%s=%d\n", s_devs[dev].name, vol);
			done = true;
		} else {
			memcpy(out + o, ln, ll);
			o += ll;
			out[o++] = '\n';
		}
		ln += ll + (ln[ll] == '\n');
	}
	if (!done) {
		o += snprintf(out + o, sizeof(out) - o, "%s=%d\n", s_devs[dev].name, vol);
	}
	if (mkdir(LOG_CFG_DIR, 0775) != 0 && errno != EEXIST) {
		err = "cannot create /sys";
	} else if (!(f = fopen(LOG_CFG, "wb"))) {
		err = "cannot write debug.cfg";
	} else {
		bool ok = fwrite(out, 1, o, f) == o;
		if (fclose(f) != 0 || !ok) {
			err = "cannot write debug.cfg";
		}
	}
	sdUnlock();
	return err;
}

/* LOG DEBUG SAVE: show the old debug.cfg (UI and monitor), then write the
 * standard header and every current volume. */
static const char *logCfgSaveAll(cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR char in[LOG_CFG_MAX + 1];
	char        line[64];
	size_t      n   = 0;
	const char *err = NULL;
	bool        had = false;

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	FILE *f = fopen(LOG_CFG, "rb");
	if (f) {
		had = true;
		n   = fread(in, 1, LOG_CFG_MAX, f);
		fclose(f);
	}
	sdUnlock();
	in[n] = '\0';

	/* show the old file (sending may wait on the network: SD not held) */
	reply(had ? "--- old debug.cfg ---\n" : "(no debug.cfg)\n");
	dbgf("%s", had ? "--- old debug.cfg ---" : "(no debug.cfg)");
	if (had) {
		for (char *ln = in; *ln;) {
			size_t ll = strcspn(ln, "\n");
			size_t sl = ll;
			while (sl > 0 && ln[sl - 1] == '\r') {
				sl--;
			}
			snprintf(line, sizeof(line) - 1, "%.*s", (int)sl, ln);
			for (char *c = line; *c; c++) {
				if ((unsigned char)*c < ' ' || (unsigned char)*c > '~') {
					*c = '?';		/* ASCII only */
				}
			}
			dbgf("%s", line);
			strcat(line, "\n");
			reply(line);
			ln += ll + (ln[ll] == '\n');
		}
		reply("--- end ---\n");
		dbgf("--- end ---");
	}

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		err = "no SD card";
	} else if (mkdir(LOG_CFG_DIR, 0775) != 0 && errno != EEXIST) {
		err = "cannot create /sys";
	} else if (!(f = fopen(LOG_CFG, "wb"))) {
		err = "cannot write debug.cfg";
	} else {
		bool ok = fputs(s_cfgHead, f) >= 0;
		for (int d = 0; d < LOG_DEVS && ok; d++) {
			ok = fprintf(f, "%s=%d\n", s_devs[d].name, s_vol[d]) > 0;
		}
		if (fclose(f) != 0 || !ok) {
			err = "cannot write debug.cfg";
		}
	}
	sdUnlock();
	return err;
}

/* Caller holds s_lock. AUTO: every port except the UI port; UART / USB /
 * BOTH: exactly those ports. Debug on: all ports. */
static void logOut(const char *text, size_t len)
{
	int ui   = s_uiPort;
	int mode = s_mode;

	for (int p = 0; p < SER_COUNT; p++) {
		bool fixed = (mode == LOG_BOTH) || (mode == LOG_UART && p == SER_UART) ||
		             (mode == LOG_USB && p == SER_USB);
		if (s_debug || (mode == LOG_AUTO ? p != ui : fixed)) {
			serWrite((serPort_t)p, text, len, true);
		}
	}
}

/* ================= Commands ================= */

static const char *logCmd(int argc, char *argv[], cmdReply_t reply)
{
	if (argc >= 1 && strcasecmp(argv[0], "DEBUG") == 0) {
		return logCmdDebug(argc - 1, argv + 1, reply);
	}
	if (argc > 1) {
		return "usage: LOG [UART|USB|BOTH|AUTO] | DEBUG [SAVE | device|ALL [0..4 [SAVE]]]";
	}
	if (argc == 1) {
		int m;
		for (m = LOG_AUTO; m <= LOG_BOTH; m++) {
			if (strcasecmp(argv[0], s_modeNames[m]) == 0) {
				break;
			}
		}
		if (m > LOG_BOTH) {
			return "usage: LOG [UART|USB|BOTH|AUTO]";
		}
		s_mode = m;
	}
	char text[32];
	snprintf(text, sizeof(text), "LOG %s%s\n", s_modeNames[s_mode], s_debug ? " (debug on)" : "");
	reply(text);
	return NULL;
}

/* LOG DEBUG [device [0..4 [SAVE]]]: show / set the volume (this boot; SAVE:
 * also in debug.cfg). LOG DEBUG SAVE: rewrite debug.cfg with every volume. */
static const char *logCmdDebug(int argc, char *argv[], cmdReply_t reply)
{
	char text[32];
	int  d = -1;

	if (argc >= 1 && strcasecmp(argv[0], "ALL") == 0) {	/* LOG DEBUG ALL [n [SAVE]] */
		if (argc >= 2) {
			if (argv[1][0] < '0' || argv[1][0] > '4' || argv[1][1] != '\0' ||
			    (argc == 3 && strcasecmp(argv[2], "SAVE") != 0)) {
				return "usage: LOG DEBUG ALL 0..4 [SAVE]";
			}
			for (int i = 0; i < LOG_DEVS; i++) {
				s_vol[i] = argv[1][0] - '0';
			}
			if (argc == 3) {
				const char *err = logCfgSaveAll(reply);
				if (err) {
					return err;
				}
			}
		}
		argc = 0;				/* then list them all */
	} else if (argc == 1 && strcasecmp(argv[0], "SAVE") == 0) {
		const char *err = logCfgSaveAll(reply);
		if (err) {
			return err;
		}
		argc = 0;				/* then list what was saved */
	} else if (argc >= 1 && (d = logDevFind(argv[0], strlen(argv[0]))) < 0) {
		return "device: BLE WIFI WEB SD SCRIPT IMU SYS (or ALL)";
	}
	if (argc >= 2) {
		if (argv[1][0] < '0' || argv[1][0] > '4' || argv[1][1] != '\0' ||
		    (argc == 3 && strcasecmp(argv[2], "SAVE") != 0)) {
			return "usage: LOG DEBUG device 0..4 [SAVE]";
		}
		int v = argv[1][0] - '0';
		if (argc == 3) {
			const char *err = logCfgSave(d, v);
			if (err) {
				return err;
			}
		}
		s_vol[d] = v;
	}
	for (int i = 0; i < LOG_DEVS; i++) {
		if (d < 0 || i == d) {
			snprintf(text, sizeof(text), "%s=%d\n", s_devs[i].name, s_vol[i]);
			reply(text);
		}
	}
	return NULL;
}
