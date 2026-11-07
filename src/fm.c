#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "ff.h"

#include "cmd.h"
#include "script.h"
#include "sd.h"
#include "ui.h"

static const char *TAG = "fm";

#define FM_SYS_DIR	SD_MOUNT_POINT "/sys"
#define FM_PAGE		FM_SYS_DIR "/fm.html"		/* replaces the built-in page */
#define FM_EXPORT	FM_SYS_DIR "/fm_builtin.html"	/* FILES FMEXPORT */
#define FM_SYNTAX	FM_SYS_DIR "/syntax.cfg"	/* replaces the built-in colours */
#define FM_SYN_EXPORT	FM_SYS_DIR "/syntax_builtin.cfg"	/* FILES SYNEXPORT */
#define FM_PATH_MAX	300		/* full path incl. mount point */
#define FM_QUERY_MAX	512		/* = CONFIG_HTTPD_MAX_URI_LEN */
#define FM_CHUNK	1436		/* one TCP segment */
#define FM_UP_CHUNK	4096		/* upload: bytes per receive / write */
#define FM_UP_TMP	".upl"		/* upload goes to <name>.upl, renamed when complete */
#define FM_UP_RETRIES	3		/* receive time-outs in a row before giving up */
#define FM_JSON_FLUSH	1024		/* list: send when this much is buffered */

/* built-in page, embedded from assets/fm.html (see CMakeLists.txt) */
extern const char fm_html_start[] asm("_binary_fm_html_start");
/* built-in viewer colours, embedded from assets/syntax.cfg */
extern const char syntax_cfg_start[] asm("_binary_syntax_cfg_start");

static volatile bool s_active;

static const char *fmGate(int argc, char *argv[]);
static esp_err_t   fmGetPage(httpd_req_t *req);
static esp_err_t   fmList(httpd_req_t *req);
static esp_err_t   fmGetFile(httpd_req_t *req);
static esp_err_t   fmView(httpd_req_t *req);
static esp_err_t   fmSyntax(httpd_req_t *req);
static esp_err_t   fmDel(httpd_req_t *req);
static esp_err_t   fmRen(httpd_req_t *req);
static esp_err_t   fmMkdir(httpd_req_t *req);
static esp_err_t   fmExit(httpd_req_t *req);
static esp_err_t   fmPut(httpd_req_t *req);

/* ================= Public API ================= */

esp_err_t fmInit(void)
{
	cmdSetGate(fmGate);
	return ESP_OK;
}

bool fmActive(void)
{
	return s_active;
}

const char *fmStart(void)
{
	if (uiActive() != UI_WEB || !uiHasClient()) {
		return "FILES WEB: web UI only";
	}
	if (s_active) {
		return NULL;				/* already on: the page is just re-opened */
	}
	if (scriptActive()) {
		return "a script is running";
	}
	sdLock();
	esp_err_t card = sdEnsureMounted();
	sdUnlock();
	if (card != ESP_OK) {
		return "no SD card";
	}
	s_active = true;
	ESP_LOGI(TAG, "SD manager on");
	return NULL;
}

void fmStop(void)
{
	if (!s_active) {
		return;
	}
	s_active = false;
	ESP_LOGI(TAG, "SD manager off");
	uiFmChanged();
}

/* Write the NUL-terminated text to file (/sys created). NULL or the error. */
static const char *fmWriteBuiltin(const char *text, const char *file, const char *name, size_t *bytes)
{
	static char err[64];
	const char *ret = NULL;
	size_t      n   = strlen(text);
	FILE       *f   = NULL;

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		ret = "no SD card";
	} else if (mkdir(FM_SYS_DIR, 0775) != 0 && errno != EEXIST) {
		ret = "cannot create /sys";
	} else if (!(f = fopen(file, "wb"))) {
		snprintf(err, sizeof(err), "cannot write %s", name);
		ret = err;
	} else {
		bool ok = fwrite(text, 1, n, f) == n;
		if (fclose(f) != 0 || !ok) {
			snprintf(err, sizeof(err), "cannot write %s", name);
			ret = err;
		}
	}
	sdUnlock();
	*bytes = n;
	return ret;
}

static int fmCmpStr(const void *a, const void *b)
{
	return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

/* The built-in colours, then a comment block with every registered command
 * (alphabetical, wrapped) - a word list for PC editors. */
const char *fmSynExport(size_t *bytes)
{
	int          nc    = cmdCount();
	size_t       base  = strlen(syntax_cfg_start);
	size_t       max   = base + 256 + (size_t)nc * 24;
	char        *text  = heap_caps_malloc(max, MALLOC_CAP_SPIRAM);
	const char **names = heap_caps_malloc((nc ? nc : 1) * sizeof(char *), MALLOC_CAP_SPIRAM);
	if (!text || !names) {
		free(text);
		free(names);
		return "out of memory";
	}
	for (int i = 0; i < nc; i++) {
		names[i] = cmdName(i);
	}
	qsort(names, nc, sizeof(char *), fmCmpStr);
	size_t n = snprintf(text, max, "%s\n# Commands registered on this device (%d) - coloured as s.command when\n"
	                    "# they start a statement:\n#  ", syntax_cfg_start, nc);
	size_t col = 3;
	for (int i = 0; i < nc && n + 24 < max; i++) {
		size_t l = strlen(names[i]);
		if (col + l + 1 > 76) {
			n  += snprintf(text + n, max - n, "\n#  ");
			col = 3;
		}
		n   += snprintf(text + n, max - n, " %s", names[i]);
		col += l + 1;
	}
	snprintf(text + n, max - n, "\n");
	const char *err = fmWriteBuiltin(text, FM_SYN_EXPORT, "sys/syntax_builtin.cfg", bytes);
	free(text);
	free(names);
	return err;
}

const char *fmExport(size_t *bytes)
{
	return fmWriteBuiltin(fm_html_start, FM_EXPORT, "sys/fm_builtin.html", bytes);
}

void fmRegister(httpd_handle_t hd)
{
	const httpd_uri_t uris[FM_URI_COUNT] = {
		{ .uri = "/fm",       .method = HTTP_GET,  .handler = fmGetPage },
		{ .uri = "/fm/list",  .method = HTTP_GET,  .handler = fmList },
		{ .uri = "/fm/get",   .method = HTTP_GET,  .handler = fmGetFile },
		{ .uri = "/fm/view",  .method = HTTP_GET,  .handler = fmView },
		{ .uri = "/fm/syntax", .method = HTTP_GET, .handler = fmSyntax },
		{ .uri = "/fm/del",   .method = HTTP_POST, .handler = fmDel },
		{ .uri = "/fm/ren",   .method = HTTP_POST, .handler = fmRen },
		{ .uri = "/fm/mkdir", .method = HTTP_POST, .handler = fmMkdir },
		{ .uri = "/fm/exit",  .method = HTTP_POST, .handler = fmExit },
		{ .uri = "/fm/put",   .method = HTTP_POST, .handler = fmPut },
	};
	for (int i = 0; i < FM_URI_COUNT; i++) {
		if (httpd_register_uri_handler(hd, &uris[i]) != ESP_OK) {
			ESP_LOGE(TAG, "cannot register %s", uris[i].uri);
		}
	}
}

bool fmUrlDecode(char *s)
{
	char *o = s;
	for (; *s; s++) {
		if (*s == '+') {
			*o++ = ' ';
		} else if (*s == '%') {
			int v = 0;
			for (int i = 1; i <= 2; i++) {
				char c = s[i];
				int  d = (c >= '0' && c <= '9') ? c - '0'
				       : (c >= 'a' && c <= 'f') ? c - 'a' + 10
				       : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
				if (d < 0) {
					return false;
				}
				v = v * 16 + d;
			}
			if (v == 0) {
				return false;		/* %00 would cut the path */
			}
			*o++ = (char)v;
			s += 2;
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
	return true;
}

bool fmPathCheck(const char *p, const char *root, char *out, size_t outMax)
{
	if (p[0] != '/') {
		return false;
	}
	if (p[1] != '\0') {				/* "/" = the card itself */
		const char *seg = p + 1;
		for (;;) {
			size_t len = strcspn(seg, "/");
			if (len == 0 || seg[0] == ' ' || seg[len - 1] == '.' || seg[len - 1] == ' ') {
				return false;		/* empty, ".", "..", "x.", "x ", " x" */
			}
			for (size_t i = 0; i < len; i++) {
				unsigned char c = (unsigned char)seg[i];
				if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') {
					return false;
				}
			}
			if (seg[len] == '\0') {
				break;
			}
			seg += len + 1;
		}
	}
	int n = snprintf(out, outMax, "%s%s", root, p[1] ? p : "");
	return n > 0 && (size_t)n < outMax;
}

/* ================= Internals ================= */

/* While the mode is on only FILES WEB STOP gets through. */
static const char *fmGate(int argc, char *argv[])
{
	if (!s_active) {
		return NULL;
	}
	if (argc >= 2 && argc <= 3 && strcasecmp(argv[0], "FILES") == 0 && strcasecmp(argv[1], "WEB") == 0 &&
	    (argc == 2 || strcasecmp(argv[2], "STOP") == 0 || strcasecmp(argv[2], "EXIT") == 0)) {
		return NULL;				/* FILES WEB (re-open) / FILES WEB STOP|EXIT */
	}
	return "SD manager active";
}

/* Reply status ("400 Bad Request" ...) with a text body. */
static esp_err_t fmFail(httpd_req_t *req, const char *status, const char *why)
{
	httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_sendstr(req, why);
}

static esp_err_t fmOk(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_sendstr(req, "OK");
}

/* Query parameter key -> checked full path. NULL = OK, else the reason. */
static const char *fmArg(httpd_req_t *req, const char *key, char *path, size_t n)
{
	static EXT_RAM_BSS_ATTR char q[FM_QUERY_MAX + 1];	/* server task only */
	char v[FM_PATH_MAX];

	if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
	    httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
		return "missing path";
	}
	if (!fmUrlDecode(v) || !fmPathCheck(v, SD_MOUNT_POINT, path, n)) {
		return "bad path";
	}
	return NULL;
}

/* Mode off: refuse (409). true = refused and replied. */
static bool fmOff(httpd_req_t *req)
{
	if (s_active) {
		return false;
	}
	fmFail(req, "409 Conflict", "SD manager not active");
	return true;
}

static esp_err_t fmGetPage(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char buf[FM_CHUNK];
	if (!s_active) {
		httpd_resp_set_status(req, "302 Found");
		httpd_resp_set_hdr(req, "Location", "/");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		return httpd_resp_send(req, NULL, 0);
	}
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	sdLock();
	FILE *f = sdEnsureMounted() == ESP_OK ? fopen(FM_PAGE, "rb") : NULL;
	if (!f) {
		sdUnlock();
		return httpd_resp_send(req, fm_html_start, HTTPD_RESP_USE_STRLEN);	/* built-in */
	}
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
			break;
		}
	}
	fclose(f);
	sdUnlock();
	return httpd_resp_send_chunk(req, NULL, 0);
}

/* JSON string: quotes, backslash and control characters escaped. */
static size_t fmJsonStr(char *o, size_t max, const char *s)
{
	size_t n = 0;
	if (n < max) o[n++] = '"';
	for (; *s && n + 7 < max; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			o[n++] = '\\';
			o[n++] = c;
		} else if (c < 0x20) {
			n += snprintf(o + n, max - n, "\\u%04x", c);
		} else {
			o[n++] = c;
		}
	}
	if (n < max) o[n++] = '"';
	return n;
}

/* FAT date / time fields -> seconds since 1970 (taken as UTC, as the VFS does
 * with no time zone set). */
static long long fmFatTime(WORD d, WORD t)
{
	int y = 1980 + (d >> 9), m = (d >> 5) & 15, day = d & 31;
	if (m < 1 || m > 12 || day < 1) {
		return 0;
	}
	/* days from civil (H. Hinnant) */
	y -= m <= 2;
	int      era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	long long days = (long long)era * 146097 + doe - 719468;
	return days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2;
}

/* GET /fm/list?p=/dir: read through FatFs directly (not the VFS) to get the
 * attribute flags: attr bit 1 read-only, 2 hidden, 4 system. */
static esp_err_t fmList(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char    js[FM_JSON_FLUSH + 2048];	/* + one entry (escaped name) */
	static EXT_RAM_BSS_ATTR FF_DIR  fd;
	static EXT_RAM_BSS_ATTR FILINFO fi;
	char        dir[FM_PATH_MAX], fpath[FM_PATH_MAX + 4], drv[4];
	const char *why;

	if (fmOff(req)) {
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", dir, sizeof(dir)))) {
		return fmFail(req, "400 Bad Request", why);
	}
	const char *rel = dir + strlen(SD_MOUNT_POINT);
	sdLock();
	bool ok = sdEnsureMounted() == ESP_OK && sdFatDrive(drv);
	if (ok) {
		snprintf(fpath, sizeof(fpath), "%s%s", drv, *rel ? rel : "/");
		ok = f_opendir(&fd, fpath) == FR_OK;
	}
	if (!ok) {
		sdUnlock();
		return fmFail(req, "409 Conflict", "no such folder");
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");

	/* two passes: folders, then files */
	size_t n = snprintf(js, sizeof(js), "{\"p\":");
	n += fmJsonStr(js + n, sizeof(js) - n, *rel ? rel : "/");
	esp_err_t err = ESP_OK;
	for (int pass = 0; pass < 2 && err == ESP_OK; pass++) {
		n += snprintf(js + n, sizeof(js) - n, pass ? "],\"f\":[" : ",\"d\":[");
		bool first = true;
		f_rewinddir(&fd);
		while (err == ESP_OK && f_readdir(&fd, &fi) == FR_OK && fi.fname[0]) {
			bool isDir = (fi.fattrib & AM_DIR) != 0;
			if (isDir != (pass == 0)) {
				continue;
			}
			int attr = ((fi.fattrib & AM_RDO) ? 1 : 0) | ((fi.fattrib & AM_HID) ? 2 : 0) |
			           ((fi.fattrib & AM_SYS) ? 4 : 0);
			if (!first) {
				js[n++] = ',';
			}
			first = false;
			js[n++] = '[';
			n += fmJsonStr(js + n, sizeof(js) - n, fi.fname);
			if (isDir) {
				n += snprintf(js + n, sizeof(js) - n, ",%lld,%d]", fmFatTime(fi.fdate, fi.ftime), attr);
			} else {
				n += snprintf(js + n, sizeof(js) - n, ",%lu,%lld,%d]", (unsigned long)fi.fsize,
				              fmFatTime(fi.fdate, fi.ftime), attr);
			}
			if (n >= FM_JSON_FLUSH) {
				err = httpd_resp_send_chunk(req, js, n);
				n   = 0;
			}
		}
	}
	f_closedir(&fd);
	sdUnlock();
	if (err == ESP_OK) {
		n += snprintf(js + n, sizeof(js) - n, "]}");
		err = httpd_resp_send_chunk(req, js, n);
	}
	if (err == ESP_OK) {
		err = httpd_resp_send_chunk(req, NULL, 0);
	}
	return err;
}

/* Content type for /fm/view from the file name's extension; NULL = not
 * viewable. */
static const char *fmViewType(const char *path)
{
	static const struct { const char *ext, *type; } types[] = {
		{ "png", "image/png" }, { "ico", "image/x-icon" }, { "jpg", "image/jpeg" },
		{ "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "bmp", "image/bmp" },
		{ "webp", "image/webp" }, { "svg", "image/svg+xml" },
		{ "htm", "text/html" }, { "html", "text/html" },
		{ "txt", "text/plain" }, { "csv", "text/plain" }, { "s", "text/plain" },
		{ "ion", "text/plain" }, { "var", "text/plain" }, { "cfg", "text/plain" },
		{ "c", "text/plain" }, { "h", "text/plain" }, { "sh", "text/plain" },
		{ "css", "text/plain" }, { "nvram", "text/plain" },
	};
	const char *dot = strrchr(path, '.');
	if (!dot || strchr(dot, '/')) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (strcasecmp(dot + 1, types[i].ext) == 0) {
			return types[i].type;
		}
	}
	return NULL;
}

/* .nvram for viewing: each line starting with '!' sent as "!key=[redacted]"
 * (no '=': "![redacted]"), like FCAT. Lines read in pieces, so any length. */
static esp_err_t fmSendRedacted(httpd_req_t *req, FILE *f)
{
	static EXT_RAM_BSS_ATTR char line[FM_CHUNK];
	esp_err_t err = ESP_OK;
	bool      start = true, secret = false, cut = false;	/* cut: '=' passed, value dropped */

	while (err == ESP_OK && fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		bool   nl  = len && line[len - 1] == '\n';
		if (start) {
			secret = line[0] == '!';
			cut    = false;
		}
		if (!secret) {
			err = httpd_resp_send_chunk(req, line, len);
		} else if (!cut) {
			char  *eq = strchr(line, '=');
			size_t kl = eq ? (size_t)(eq - line) + 1 : (start ? 1 : 0);
			err = httpd_resp_send_chunk(req, line, kl);
			if (err == ESP_OK && (eq || start)) {
				err = httpd_resp_send_chunk(req, "[redacted]", 10);
				cut = true;
			}
			if (err == ESP_OK && nl && cut) {
				err = httpd_resp_send_chunk(req, "\n", 1);
			}
		} else if (nl) {
			err = httpd_resp_send_chunk(req, "\n", 1);	/* rest of a long secret line */
		}
		start = nl;
	}
	return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

/* GET /fm/view?p=/dir/file: the file to display (type from the extension). */
static esp_err_t fmView(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char buf[FM_CHUNK];
	char        path[FM_PATH_MAX];
	struct stat st;
	const char *why, *type;

	if (fmOff(req)) {
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", path, sizeof(path)))) {
		return fmFail(req, "400 Bad Request", why);
	}
	if (!(type = fmViewType(path))) {
		return fmFail(req, "400 Bad Request", "not a viewable type");
	}
	sdLock();
	FILE *f = NULL;
	if (sdEnsureMounted() == ESP_OK && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		f = fopen(path, "rb");
	}
	if (!f) {
		sdUnlock();
		return fmFail(req, "409 Conflict", "no such file");
	}
	httpd_resp_set_type(req, type);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
	const char *dot = strrchr(path, '.');
	if (strcasecmp(dot, ".nvram") == 0) {
		esp_err_t err = fmSendRedacted(req, f);	/* "!key=value" -> "!key=[redacted]", as FCAT */
		fclose(f);
		sdUnlock();
		return err;
	}
	esp_err_t err = ESP_OK;
	size_t    n;
	while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
		err = httpd_resp_send_chunk(req, buf, n);
	}
	fclose(f);
	sdUnlock();
	return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

/* GET /fm/syntax[?builtin=1]: viewer colours - SD:/sys/syntax.cfg if
 * present (header X-Syntax-Source: card), else the built-in (builtin). */
static esp_err_t fmSyntax(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char buf[FM_CHUNK];
	char q[32], v[4] = "";

	if (fmOff(req)) {
		return ESP_OK;
	}
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
		httpd_query_key_value(q, "builtin", v, sizeof(v));
	}
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	sdLock();
	FILE *f = (strcmp(v, "1") != 0 && sdEnsureMounted() == ESP_OK) ? fopen(FM_SYNTAX, "rb") : NULL;
	if (!f) {
		sdUnlock();
		httpd_resp_set_hdr(req, "X-Syntax-Source", "builtin");
		return httpd_resp_send(req, syntax_cfg_start, HTTPD_RESP_USE_STRLEN);
	}
	httpd_resp_set_hdr(req, "X-Syntax-Source", "card");
	esp_err_t err = ESP_OK;
	size_t    n;
	while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
		err = httpd_resp_send_chunk(req, buf, n);
	}
	fclose(f);
	sdUnlock();
	return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t fmGetFile(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char buf[FM_CHUNK];
	char        path[FM_PATH_MAX], disp[FM_PATH_MAX + 32];
	struct stat st;
	const char *why;

	if (fmOff(req)) {
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", path, sizeof(path)))) {
		return fmFail(req, "400 Bad Request", why);
	}
	sdLock();
	FILE *f = NULL;
	if (sdEnsureMounted() == ESP_OK && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		f = fopen(path, "rb");
	}
	if (!f) {
		sdUnlock();
		return fmFail(req, "409 Conflict", "no such file");
	}
	/* file name for the browser: '"' and '\' replaced */
	const char *name = strrchr(path, '/') + 1;
	size_t      k    = snprintf(disp, sizeof(disp), "attachment; filename=\"");
	for (const char *c = name; *c && k + 2 < sizeof(disp); c++) {
		disp[k++] = (*c == '"' || *c == '\\') ? '_' : *c;
	}
	disp[k++] = '"';
	disp[k]   = '\0';
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Content-Disposition", disp);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	esp_err_t err = ESP_OK;
	size_t    n;
	while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
		err = httpd_resp_send_chunk(req, buf, n);
	}
	fclose(f);
	sdUnlock();
	return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t fmDel(httpd_req_t *req)
{
	char        path[FM_PATH_MAX];
	struct stat st;
	const char *why;

	if (fmOff(req)) {
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", path, sizeof(path)))) {
		return fmFail(req, "400 Bad Request", why);
	}
	if (strcmp(path, SD_MOUNT_POINT) == 0) {
		return fmFail(req, "400 Bad Request", "not the card itself");
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		why = "no SD card";
	} else if (stat(path, &st) != 0) {
		why = "no such file or folder";
	} else if (S_ISDIR(st.st_mode)) {
		why = rmdir(path) == 0 ? NULL : (errno == ENOTEMPTY || errno == EEXIST || errno == EACCES)
		                                ? "folder not empty" : "delete failed";
	} else if (!(st.st_mode & S_IWUSR)) {
		why = "read-only";
	} else if (unlink(path) != 0) {
		why = "delete failed";
	}
	sdUnlock();
	return why ? fmFail(req, "409 Conflict", why) : fmOk(req);
}

static esp_err_t fmRen(httpd_req_t *req)
{
	char        from[FM_PATH_MAX], to[FM_PATH_MAX];
	struct stat st;
	const char *why;

	if (fmOff(req)) {
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", from, sizeof(from))) || (why = fmArg(req, "to", to, sizeof(to)))) {
		return fmFail(req, "400 Bad Request", why);
	}
	if (strcmp(from, SD_MOUNT_POINT) == 0 || strcmp(to, SD_MOUNT_POINT) == 0) {
		return fmFail(req, "400 Bad Request", "not the card itself");
	}
	size_t fl = strlen(from);
	if (strncmp(to, from, fl) == 0 && to[fl] == '/') {
		return fmFail(req, "400 Bad Request", "cannot move a folder into itself");
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		why = "no SD card";
	} else if (stat(from, &st) != 0) {
		why = "no such file or folder";
	} else if (!S_ISDIR(st.st_mode) && !(st.st_mode & S_IWUSR)) {
		why = "read-only";
	} else if (strcasecmp(from, to) != 0 && stat(to, &st) == 0) {
		why = "the new name exists";		/* FAT names ignore case: a case change is fine */
	} else if (rename(from, to) != 0) {
		why = "rename failed (folder missing?)";
	}
	sdUnlock();
	return why ? fmFail(req, "409 Conflict", why) : fmOk(req);
}

static esp_err_t fmMkdir(httpd_req_t *req)
{
	char        path[FM_PATH_MAX];
	struct stat st;
	const char *why;

	if (fmOff(req)) {
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", path, sizeof(path)))) {
		return fmFail(req, "400 Bad Request", why);
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		why = "no SD card";
	} else if (stat(path, &st) == 0) {
		why = "already exists";
	} else if (mkdir(path, 0775) != 0) {
		why = "cannot make the folder";
	}
	sdUnlock();
	return why ? fmFail(req, "409 Conflict", why) : fmOk(req);
}

/* Upload failed part-way: reply, and close the connection (the rest of the
 * body is still on its way and would be read as the next request). */
static esp_err_t fmPutFail(httpd_req_t *req, const char *status, const char *why)
{
	esp_err_t err = fmFail(req, status, why);
	httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
	return err;
}

/* POST /fm/put?p=/dir/name[&over=1], body = the file. Written to name.upl,
 * renamed to name when complete (over=1: an existing file is replaced, not
 * a read-only one). Mode turned off or a failure: name.upl deleted. */
static esp_err_t fmPut(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char buf[FM_UP_CHUNK];
	static EXT_RAM_BSS_ATTR char q[FM_QUERY_MAX + 1];
	char        path[FM_PATH_MAX], tmp[FM_PATH_MAX + sizeof(FM_UP_TMP)], over[4] = "";
	struct stat st;
	const char *why;

	if (fmOff(req)) {
		httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
		return ESP_OK;
	}
	if ((why = fmArg(req, "p", path, sizeof(path)))) {
		return fmPutFail(req, "400 Bad Request", why);
	}
	if (strcmp(path, SD_MOUNT_POINT) == 0) {
		return fmPutFail(req, "400 Bad Request", "not the card itself");
	}
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
		httpd_query_key_value(q, "over", over, sizeof(over));
	}
	snprintf(tmp, sizeof(tmp), "%s" FM_UP_TMP, path);

	sdLock();
	FILE *f = NULL;
	if (sdEnsureMounted() != ESP_OK) {
		why = "no SD card";
	} else if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		why = "a folder has that name";
	} else if (stat(path, &st) == 0 && strcmp(over, "1") != 0) {
		why = "exists";				/* the page asks, then sends over=1 */
	} else if (stat(path, &st) == 0 && !(st.st_mode & S_IWUSR)) {
		why = "read-only";
	} else if (!(f = fopen(tmp, "wb"))) {
		why = "cannot create the file (folder missing?)";
	}
	sdUnlock();
	if (why) {
		return fmPutFail(req, "409 Conflict", why);
	}

	size_t left  = req->content_len;
	int    tries = 0;
	while (left > 0 && !why) {
		int n = httpd_req_recv(req, buf, left < sizeof(buf) ? left : sizeof(buf));
		if (n == HTTPD_SOCK_ERR_TIMEOUT && ++tries < FM_UP_RETRIES) {
			continue;
		}
		if (n <= 0) {
			why = "upload broken off";
			break;
		}
		tries = 0;
		if (!s_active) {
			why = "SD manager not active";		/* left while uploading */
			break;
		}
		sdLock();
		if (fwrite(buf, 1, n, f) != (size_t)n) {
			why = "write failed (card full?)";
		}
		sdUnlock();
		left -= n;
	}

	sdLock();
	if (fclose(f) != 0 && !why) {
		why = "write failed (card full?)";
	}
	if (!why) {
		if (stat(path, &st) == 0 && unlink(path) != 0) {
			why = "cannot replace the file";
		} else if (rename(tmp, path) != 0) {
			why = "cannot rename the upload";
		}
	}
	if (why) {
		unlink(tmp);
	}
	sdUnlock();
	if (why) {
		ESP_LOGW(TAG, "upload %s: %s", path + strlen(SD_MOUNT_POINT), why);
		return fmPutFail(req, "409 Conflict", why);
	}
	ESP_LOGI(TAG, "uploaded %s (%u bytes)", path + strlen(SD_MOUNT_POINT), (unsigned)req->content_len);
	return fmOk(req);
}

static esp_err_t fmExit(httpd_req_t *req)
{
	fmStop();
	return fmOk(req);
}
