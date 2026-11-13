#include "sys.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"

#include "cmd.h"
#include "sd.h"
#include "nv.h"
#include "log.h"
#include "text.h"

#define SYS_WAIT_MAX	60000
#define SYS_REBOOT_MS	300	/* let OK reach the client first */
#define SYS_STAMP_TRIES	100	/* -1 .. -99 suffixes */
#define SYS_CLOCK_SET	1577836800	/* 2020-01-01: below = never set (boots at 1970) */
#define SYS_DATA_DIR	SD_MOUNT_POINT "/scripts/data"
#define SYS_NV_DIR	SD_MOUNT_POINT "/sys"	/* name.nvram files */
#define SYS_PATH_MAX	(sizeof(SYS_DATA_DIR) + SYS_FILE_MAX + 8)
#define SYS_FILE_MAX	31	/* FLOG / FCAT ... file name length */
#define SYS_LINE_MAX	200	/* FCAT: longer lines are split */
#define SYS_READ_BLOCK	1024	/* FCAT: bytes read per card access */
#define SYS_LIST_MAX	64	/* FDIR: files listed */

static int s_boot;		/* this boot's number (NV sys.bootcnt) */

static const char *sysResetReason(void);
static const char *sysCmdInfo(int argc, char *argv[], cmdReply_t reply);
static const char *sysCmdWait(int argc, char *argv[], cmdReply_t reply);
static const char *sysCmdReboot(int argc, char *argv[], cmdReply_t reply);
static void        sysRestart(void *arg);
static const char *sysCmdMem(int argc, char *argv[], cmdReply_t reply);
static const char *sysCmdFdir(int argc, char *argv[], cmdReply_t reply);
static const char *sysCmdFstat(int argc, char *argv[], cmdReply_t reply);
static const char *sysCmdFdelete(int argc, char *argv[], cmdReply_t reply);
static const char *sysDataPath(const char *file, char *path, size_t n, bool *nvram);
static void        sysWaitDone(void *arg);
static const char *sysText(const char *args, char *out, size_t n);
static const char *sysCmdPrint(const char *args, cmdReply_t reply);
static const char *sysCmdDebug(const char *args, cmdReply_t reply);
static const char *sysCmdAppend(const char *args, cmdReply_t reply);
static const char *sysCmdLogcat(int argc, char *argv[], cmdReply_t reply);
static const char *sysFileName(const char *s, size_t len, char *out);
static const char *sysLogList(const char *type, cmdReply_t reply);
static void        sysLogLine(const char *line, bool debug, bool redact, cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t sysInit(void)
{
	s_boot = (nvGetInt("sys.bootcnt") + 1) & 0xFFFF;	/* count this boot */
	char        num[8];
	const char *k[] = { "sys.bootcnt" }, *v[] = { num };
	snprintf(num, sizeof(num), "%d", s_boot);
	if (nvSetMany(1, k, v, NULL) != NULL) {
		ESP_LOGW("sys", "sys.bootcnt not stored");
	}

	esp_err_t err = cmdRegister("INFO", 0, 0, sysCmdInfo, "INFO  hardware and firmware info");
	if (err == ESP_OK) {
		err = cmdRegister("WAIT", 1, 1, sysCmdWait, "WAIT ms  pause 0..60000 ms (1 ms resolution)");
	}
	if (err == ESP_OK) {
		err = cmdRegisterRaw("PRINT", sysCmdPrint, "PRINT text  reply text {\\n \\\\ \\# \\$}");
	}
	if (err == ESP_OK) {
		err = cmdRegisterRaw("DEBUG", sysCmdDebug, "DEBUG text  text to the log port(s)");
	}
	if (err == ESP_OK) {
		err = cmdRegisterRaw("FLOG", sysCmdAppend, "FLOG file text  add a line to SD:/scripts/data/file.txt");
	}
	if (err == ESP_OK) {
		err = cmdRegister("FCAT", 1, 3, sysCmdLogcat, "FCAT file[.var|.nvram] [start,count] [>DEBUG]  show a data file");
	}
	if (err == ESP_OK) {
		err = cmdRegister("FDIR", 0, 1, sysCmdFdir, "FDIR [txt|var|nvram|*]  list data files (name size ro|rw)");
	}
	if (err == ESP_OK) {
		err = cmdRegister("FSTAT", 1, 1, sysCmdFstat, "FSTAT file[.var|.nvram]  size:ro|rw");
	}
	if (err == ESP_OK) {
		err = cmdRegister("FDELETE", 1, 1, sysCmdFdelete, "FDELETE file[.var|.nvram]  delete a data file");
	}
	if (err == ESP_OK) {
		err = cmdRegister("REBOOT", 0, 0, sysCmdReboot, "REBOOT  restart the device");
	}
	if (err == ESP_OK) {
		err = cmdRegister("MEM", 0, 0, sysCmdMem, "MEM  RAM use: internal, PSRAM, task stacks");
	}
	return err;
}

void sysTimestamp(char *out, size_t n)
{
	time_t now = time(NULL);
	if (now >= SYS_CLOCK_SET) {			/* clock set by a browser (POST /time) */
		struct tm t;
		gmtime_r(&now, &t);			/* the browser sent local time */
		snprintf(out, n, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		         t.tm_hour, t.tm_min, t.tm_sec);
		return;
	}
	uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
	snprintf(out, n, "b%04X_%02" PRIu32 "%02" PRIu32 "%02" PRIu32,
	         (unsigned)s_boot, s / 3600, (s / 60) % 60, s % 60);
}

const char *sysTimestampFile(const char *dir, const char *ext, char *out, size_t n)
{
	char        base[24], path[160];
	struct stat st;

	sysTimestamp(base, sizeof(base));
	for (int i = 0; i < SYS_STAMP_TRIES; i++) {
		if (i == 0) {
			snprintf(out, n, "%s%s", base, ext);
		} else {
			snprintf(out, n, "%s-%d%s", base, i, ext);
		}
		snprintf(path, sizeof(path), "%s/%s", dir, out);
		if (stat(path, &st) != 0) {
			return NULL;
		}
	}
	return "no free name";
}

/* ================= Internals ================= */

static const char *sysResetReason(void)
{
	switch (esp_reset_reason()) {
	case ESP_RST_POWERON:   return "power-on";
	case ESP_RST_EXT:       return "external pin";
	case ESP_RST_SW:        return "software";
	case ESP_RST_PANIC:     return "panic";
	case ESP_RST_INT_WDT:   return "interrupt watchdog";
	case ESP_RST_TASK_WDT:  return "task watchdog";
	case ESP_RST_WDT:       return "watchdog";
	case ESP_RST_DEEPSLEEP: return "deep sleep";
	case ESP_RST_BROWNOUT:  return "brownout";
	case ESP_RST_SDIO:      return "SDIO";
	case ESP_RST_USB:       return "USB";
	case ESP_RST_JTAG:      return "JTAG";
	default:                return "unknown";
	}
}

/* ================= Commands ================= */

static const char *sysCmdInfo(int argc, char *argv[], cmdReply_t reply)
{
	char text[160];

	esp_chip_info_t chip;
	esp_chip_info(&chip);
	snprintf(text, sizeof(text), "Chip: %s rev %d.%d, %d cores, %d MHz\n",
	         CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores,
	         CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
	reply(text);

	uint32_t flash = 0;
	esp_flash_get_size(NULL, &flash);
	snprintf(text, sizeof(text), "Flash: %" PRIu32 " MB  PSRAM: %u MB\n",
	         flash / (1024 * 1024), (unsigned)(esp_psram_get_size() / (1024 * 1024)));
	reply(text);

	snprintf(text, sizeof(text), "Free: %u KB internal, %u KB PSRAM\n",
	         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
	         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	reply(text);

	uint8_t wifi[6] = { 0 }, bt[6] = { 0 };
	esp_read_mac(wifi, ESP_MAC_WIFI_SOFTAP);
	esp_read_mac(bt, ESP_MAC_BT);
	snprintf(text, sizeof(text), "MAC: Wi-Fi " MACSTR "  BT " MACSTR "\n", MAC2STR(wifi), MAC2STR(bt));
	reply(text);

	const esp_app_desc_t *app = esp_app_get_description();
	snprintf(text, sizeof(text), "FW: %s %s (%s %s)\n", app->project_name, app->version, app->date, app->time);
	reply(text);

	snprintf(text, sizeof(text), "IDF: %s\n", esp_get_idf_version());
	reply(text);

	const esp_partition_t *part = esp_ota_get_running_partition();
	snprintf(text, sizeof(text), "Partition: %s\n", part ? part->label : "?");
	reply(text);

	snprintf(text, sizeof(text), "Reset: %s\n", sysResetReason());
	reply(text);

	char stamp[24];
	sysTimestamp(stamp, sizeof(stamp));
	snprintf(text, sizeof(text), "Timestamp: %s\n", stamp);
	reply(text);

	uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
	snprintf(text, sizeof(text), "Uptime: %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 "\n",
	         s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
	reply(text);

	uint64_t total, free;
	if (sdInfo(&total, &free) == ESP_OK) {
		snprintf(text, sizeof(text), "SD: %.1f GB, %.1f GB free\n",
		         total / 1073741824.0, free / 1073741824.0);
	} else {
		snprintf(text, sizeof(text), "SD: none\n");
	}
	reply(text);
	return NULL;
}

/* High-resolution wait: a one-shot esp_timer (us) instead of the 5 ms tick.
 * Each call has its own timer and semaphore, so waits from several clients
 * do not interfere. */
static const char *sysCmdWait(int argc, char *argv[], cmdReply_t reply)
{
	int ms;
	if (!cmdParseInt(argv[0], 0, SYS_WAIT_MAX, &ms)) {
		return "ms must be 0..60000";
	}
	if (ms == 0) {
		return NULL;
	}
	SemaphoreHandle_t  done  = xSemaphoreCreateBinary();
	esp_timer_handle_t timer = NULL;
	if (done) {
		esp_timer_create_args_t a = { .callback = sysWaitDone, .arg = done, .name = "wait" };
		if (esp_timer_create(&a, &timer) != ESP_OK) {
			timer = NULL;
		}
	}
	if (!timer) {
		if (done) {
			vSemaphoreDelete(done);
		}
		return "out of memory";
	}
	esp_timer_start_once(timer, (uint64_t)ms * 1000);
	xSemaphoreTake(done, portMAX_DELAY);
	esp_timer_delete(timer);
	vSemaphoreDelete(done);
	return NULL;
}

static void sysWaitDone(void *arg)
{
	xSemaphoreGive((SemaphoreHandle_t)arg);
}

/* Text argument: quotes (textUnquote) then escapes (textUnescape). */
static const char *sysText(const char *args, char *out, size_t n)
{
	const char *err = textUnquote(args, out, n);
	if (!err) {
		textUnescape(out, out, n);
	}
	return err;
}

static const char *sysCmdPrint(const char *args, cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR char text[CMD_LINE_MAX + 2];
	const char *err = sysText(args, text, sizeof(text) - 1);
	if (err) {
		return err;
	}
	strcat(text, "\n");
	reply(text);
	return NULL;
}

static const char *sysCmdDebug(const char *args, cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR char text[CMD_LINE_MAX + 1];
	const char *err = sysText(args, text, sizeof(text));
	if (err) {
		return err;
	}
	dbgf("%s", text);
	return NULL;
}

/* FLOG file text: file = letters, digits, '_' (max 31); one line added to
 * SD:/scripts/data/file.txt, folders and file created when missing. */
static const char *sysCmdAppend(const char *args, cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR char text[CMD_LINE_MAX + 1];
	char        name[SYS_FILE_MAX + 1];
	char        path[sizeof(SYS_DATA_DIR) + SYS_FILE_MAX + 8];
	size_t      len = 0;

	while (*args == ' ' || *args == '\t') {
		args++;
	}
	while (args[len] && args[len] != ' ' && args[len] != '\t') {
		len++;
	}
	if (len == 0) {
		return "usage: FLOG file text";
	}
	const char *err = sysFileName(args, len, name);
	if (err) {
		return err;
	}
	args += len;
	err = sysText(*args ? args + 1 : args, text, sizeof(text));
	if (err) {
		return err;
	}
	snprintf(path, sizeof(path), "%s/%s.txt", SYS_DATA_DIR, name);

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	err = "write failed";
	if ((mkdir(SD_MOUNT_POINT "/scripts", 0775) == 0 || errno == EEXIST) &&
	    (mkdir(SYS_DATA_DIR, 0775) == 0 || errno == EEXIST)) {
		struct stat ws;
		FILE *f = (stat(path, &ws) == 0 && !(ws.st_mode & S_IWUSR)) ? NULL : fopen(path, "a");
		if (!f && stat(path, &ws) == 0 && !(ws.st_mode & S_IWUSR)) {
			err = "read-only";
		}
		if (f) {
			bool ok = (fprintf(f, "%s\n", text) >= 0);
			if (fclose(f) == 0 && ok) {
				err = NULL;
			}
		}
	}
	sdUnlock();
	return err;
}

/* File name of len chars: letters, digits, '_' (max 31) -> out (NUL added). */
static const char *sysFileName(const char *s, size_t len, char *out)
{
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return "file: letters, digits, '_' only";
		}
	}
	if (len > SYS_FILE_MAX) {
		return "file: max 31 characters";
	}
	memcpy(out, s, len);
	out[len] = '\0';
	return NULL;
}

/* FCAT file [start,count] [>DEBUG]: show file.txt (or file.var, file.nvram)
 * line by line (or count lines from line start), to the client or the log.
 * In a .nvram file a line starting with '!' shows as "!key=[redacted]".
 * The card is read in
 * blocks and released between them, so sending (which may wait on the
 * network) never holds the SD lock. */
static const char *sysCmdLogcat(int argc, char *argv[], cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR char blk[SYS_READ_BLOCK];
	char        path[SYS_PATH_MAX];
	char        line[SYS_LINE_MAX + 1];
	size_t      ll     = 0;
	long        pos    = 0;
	bool        debug  = false;
	bool        nvram  = false;
	bool        piece  = false;	/* the line goes on after a SYS_LINE_MAX piece */
	bool        secret = false;	/* .nvram line starting with '!' */
	struct stat st;
	long        from = 1, count = -1, lineNo = 1;	/* lines to show (count -1 = all) */

	for (int i = 1; i < argc; i++) {
		char *e;
		if (strcasecmp(argv[i], ">DEBUG") == 0 && i == argc - 1) {
			debug = true;
		} else if (i == 1 && (from = strtol(argv[1], &e, 10)) >= 1 && e != argv[1] &&
		           (*e == '\0' || (*e == ',' && (count = strtol(e + 1, &e, 10)) >= 0 && *e == '\0'))) {
			/* start[,count] */
		} else {
			return "usage: FCAT file [start,count] [>DEBUG]";
		}
	}
	const char *err = sysDataPath(argv[0], path, sizeof(path), &nvram);
	if (err) {
		return err;
	}

	for (;;) {
		size_t n = 0;
		sdLock();
		if (sdEnsureMounted() != ESP_OK) {
			sdUnlock();
			return pos ? "read failed" : "no SD card";
		}
		if (pos == 0 && (stat(path, &st) != 0 || !S_ISREG(st.st_mode))) {
			sdUnlock();
			return "no such file";
		}
		FILE *f = fopen(path, "rb");
		if (f) {
			if (fseek(f, pos, SEEK_SET) == 0) {
				n = fread(blk, 1, sizeof(blk), f);
			}
			err = ferror(f) ? "read failed" : NULL;
			fclose(f);
		} else {
			err = "read failed";
		}
		sdUnlock();
		if (err) {
			return err;
		}
		if (n == 0) {
			break;
		}
		pos += n;
		for (size_t i = 0; i < n; i++) {
			char c = blk[i];
			if (c == '\r') {
				continue;			/* CR LF files from a PC */
			}
			bool show = lineNo >= from && (count < 0 || lineNo < from + count);
			if (c == '\n') {
				line[ll] = '\0';
				if (show && !(piece && secret)) {
					sysLogLine(line, debug, secret, reply);
				}
				ll     = 0;
				piece  = false;
				secret = false;
				lineNo++;
				continue;
			}
			if (ll == 0 && !piece) {
				secret = nvram && c == '!';
			}
			line[ll++] = ((c >= ' ' && c <= '~') || c == '\t') ? c : '?';	/* ASCII only */
			if (ll == SYS_LINE_MAX) {
				line[ll] = '\0';
				if (show && !(piece && secret)) {
					sysLogLine(line, debug, secret, reply);	/* long line: in pieces */
				}
				ll    = 0;
				piece = true;
			}
		}
		if (count >= 0 && lineNo >= from + count) {
			return NULL;				/* past the range: stop reading */
		}
	}
	if (ll > 0 && lineNo >= from && (count < 0 || lineNo < from + count) && !(piece && secret)) {
		line[ll] = '\0';			/* last line without a line end */
		sysLogLine(line, debug, secret, reply);
	}
	return NULL;
}

/* redact: "!key=value" -> "!key=[redacted]" (no '=' : "![redacted]") */
static void sysLogLine(const char *line, bool debug, bool redact, cmdReply_t reply)
{
	char red[SYS_LINE_MAX + 12];
	if (redact) {
		const char *eq = strchr(line, '=');
		int         kl = eq ? (int)(eq - line) + 1 : 1;
		snprintf(red, sizeof(red), "%.*s[redacted]", kl, line);
		line = red;
	}
	if (debug) {
		dbgf("%s", line);
	} else {
		char out[SYS_LINE_MAX + 14];
		snprintf(out, sizeof(out), "%s\n", line);
		reply(out);
	}
}

/* FDIR [txt|var|nvram|*]: "name size ro|rw" per file, then the count.
 * txt / var: SD:/scripts/data; nvram: SD:/sys; * = all three, with the
 * extension shown (otherwise it is dropped). */
static const char *sysLogList(const char *type, cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR char names[SYS_LIST_MAX][SD_NAME_MAX];
	static long sizes[SYS_LIST_MAX];
	static bool ro[SYS_LIST_MAX];
	static const struct { const char *type, *dir, *ext; } kinds[] = {
		{ "txt", SYS_DATA_DIR, ".txt" }, { "var", SYS_DATA_DIR, ".var" }, { "nvram", SYS_NV_DIR, ".nvram" },
	};
	char        path[sizeof(SYS_DATA_DIR) + SD_NAME_MAX + 2];
	char        out[SD_NAME_MAX + 24];
	struct stat st;
	bool        all = strcmp(type, "*") == 0;
	int         n   = 0;
	bool        found = all;

	for (int k = 0; k < 3; k++) {
		found |= strcasecmp(type, kinds[k].type) == 0;
	}
	if (!found) {
		return "usage: FDIR [txt|var|nvram|*]";
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	for (int k = 0; k < 3 && n >= 0; k++) {
		if (!all && strcasecmp(type, kinds[k].type) != 0) {
			continue;
		}
		int m = sdListFiles(kinds[k].dir, kinds[k].ext, names + n, SYS_LIST_MAX - n);
		if (m < 0) {
			n = -1;
			break;
		}
		for (int i = n; i < n + m; i++) {
			snprintf(path, sizeof(path), "%s/%s", kinds[k].dir, names[i]);
			sizes[i] = (stat(path, &st) == 0) ? (long)st.st_size : -1;
			ro[i]    = (sizes[i] >= 0) && !(st.st_mode & S_IWUSR);
			if (!all) {
				names[i][strlen(names[i]) - strlen(kinds[k].ext)] = '\0';	/* drop the extension */
			}
		}
		n += m;
	}
	sdUnlock();
	if (n < 0) {
		return "no SD card";
	}

	for (int i = 0; i < n; i++) {
		snprintf(out, sizeof(out), "%-31s %8ld %s\n", names[i], sizes[i], ro[i] ? "ro" : "rw");
		reply(out);
	}
	snprintf(out, sizeof(out), "%d file%s%s\n", n, n == 1 ? "" : "s",
	         n == SYS_LIST_MAX ? " (list full: there may be more)" : "");
	reply(out);
	return NULL;
}

/* MEM: heap per memory type, then each task's stack: lowest free so far. */
static const char *sysCmdMem(int argc, char *argv[], cmdReply_t reply)
{
	char text[96];
	const struct { const char *name; uint32_t caps; } m[] = {
		{ "Internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT },
		{ "PSRAM", MALLOC_CAP_SPIRAM },
	};
	for (int i = 0; i < 2; i++) {
		snprintf(text, sizeof(text), "%s: free %u KB, lowest %u KB, largest block %u KB\n", m[i].name,
		         (unsigned)(heap_caps_get_free_size(m[i].caps) / 1024),
		         (unsigned)(heap_caps_get_minimum_free_size(m[i].caps) / 1024),
		         (unsigned)(heap_caps_get_largest_free_block(m[i].caps) / 1024));
		reply(text);
	}
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
	UBaseType_t   n  = uxTaskGetNumberOfTasks();
	TaskStatus_t *ts = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
	if (!ts) {
		return "out of memory";
	}
	n = uxTaskGetSystemState(ts, n, NULL);
	reply("Task             prio  stack free (lowest)\n");
	for (UBaseType_t i = 0; i < n; i++) {
		snprintf(text, sizeof(text), "%-16s %4u  %6u B\n", ts[i].pcTaskName, (unsigned)ts[i].uxCurrentPriority,
		         (unsigned)ts[i].usStackHighWaterMark);
		reply(text);
	}
	free(ts);
#else
	reply("(task list needs CONFIG_FREERTOS_USE_TRACE_FACILITY)\n");
#endif
	return NULL;
}

static const char *sysCmdFdir(int argc, char *argv[], cmdReply_t reply)
{
	return sysLogList(argc ? argv[0] : "txt", reply);
}

/* file -> SD:/scripts/data/file.txt; file.txt, file.var the same folder;
 * file.nvram -> SD:/sys/file.nvram (*nvram set). file: letters, digits,
 * '_', max 31. */
static const char *sysDataPath(const char *file, char *path, size_t n, bool *nvram)
{
	char        name[SYS_FILE_MAX + 1];
	const char *dot = strchr(file, '.');
	const char *ext = dot ? dot : ".txt";
	const char *dir = SYS_DATA_DIR;
	bool        nv  = false;

	if (strcasecmp(ext, ".nvram") == 0) {
		dir = SYS_NV_DIR;
		ext = ".nvram";
		nv  = true;
	} else if (strcasecmp(ext, ".var") == 0) {
		ext = ".var";
	} else if (strcasecmp(ext, ".txt") == 0) {
		ext = ".txt";
	} else {
		return "file: name, name.var or name.nvram";
	}
	const char *err = sysFileName(file, dot ? (size_t)(dot - file) : strlen(file), name);
	if (!err) {
		snprintf(path, n, "%s/%s%s", dir, name, ext);
	}
	if (nvram) {
		*nvram = nv;
	}
	return err;
}

static const char *sysCmdFstat(int argc, char *argv[], cmdReply_t reply)
{
	char        path[SYS_PATH_MAX];
	struct stat st;
	const char *err = sysDataPath(argv[0], path, sizeof(path), NULL);
	if (err) {
		return err;
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode);
	sdUnlock();
	if (!ok) {
		return "no such file";
	}
	char text[32];
	snprintf(text, sizeof(text), "%ld:%s\n", (long)st.st_size, (st.st_mode & S_IWUSR) ? "rw" : "ro");
	reply(text);
	return NULL;
}

static const char *sysCmdFdelete(int argc, char *argv[], cmdReply_t reply)
{
	char        path[SYS_PATH_MAX];
	struct stat st;
	const char *err = sysDataPath(argv[0], path, sizeof(path), NULL);
	if (err) {
		return err;
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		err = "no SD card";
	} else if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		err = "no such file";
	} else if (!(st.st_mode & S_IWUSR)) {
		err = "read-only";
	} else if (unlink(path) != 0) {
		err = "delete failed";
	}
	sdUnlock();
	return err;
}

static const char *sysCmdReboot(int argc, char *argv[], cmdReply_t reply)
{
	static esp_timer_handle_t timer;
	if (!timer) {
		esp_timer_create_args_t a = { .callback = sysRestart, .name = "reboot" };
		if (esp_timer_create(&a, &timer) != ESP_OK) {
			return "reboot failed";
		}
	}
	esp_timer_start_once(timer, SYS_REBOOT_MS * 1000);	/* after the OK has gone out */
	return NULL;
}

static void sysRestart(void *arg)
{
	esp_restart();
}
