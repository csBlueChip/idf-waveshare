#include "nv.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs.h"

#include "cmd.h"
#include "sd.h"

static const char *TAG = "nv";

#define NV_NAMESPACE	"wavedemo"
#define NV_VAL_MAX	64		/* incl. NUL */
#define NV_DIR		SD_MOUNT_POINT "/sys"
#define NV_EXT		".nvram"
#define NV_FILE_LINE	160
#define NV_FILE_PAIRS	48

/* Checker: validate 'in' against the key's limits, write the normalised value to out. */
typedef const char *(*nvCheck_t)(const char *in, int min, int max, char *out);

typedef struct {
	const char *key;
	const char *def;
	nvCheck_t   check;
	int         min, max;
	uint8_t     flags;	/* NV_RO, NV_SECRET */
} nvKey_t;

#define NV_RO		1	/* firmware-owned: no NV SET / RESET key; IMPORT skips it; kept by NV RESET */
#define NV_SECRET	2	/* EXPORT writes "!key=value" (FCAT shows [redacted]) */

static const char *nvChkText(const char *in, int min, int max, char *out);
static const char *nvChkInt(const char *in, int min, int max, char *out);
static const char *nvChkIp(const char *in, int min, int max, char *out);
static const char *nvChkMask(const char *in, int min, int max, char *out);
static const char *nvChkPwr(const char *in, int min, int max, char *out);
static const char *nvChkUi(const char *in, int min, int max, char *out);
static const char *nvChkRatio(const char *in, int min, int max, char *out);
static const char *nvChkRgb(const char *in, int min, int max, char *out);
static const char *nvChkDbl(const char *in, int min, int max, char *out);
static const char *nvChkTenth(const char *in, int min, int max, char *out);
static const char *nvChkBaud(const char *in, int min, int max, char *out);

static const nvKey_t s_keys[] = {
	/* key               default          check       min  max    flags */
	{ "rad.ssid",         "wavedemo",      nvChkText,  1,   32,    0 },
	{ "rad.pwd",          "waveP@55word",  nvChkText,  8,   63,    NV_SECRET },
	{ "rad.ble",          "ESP32-S3-Demo", nvChkText,  1,   29,    0 },
	{ "rad.ip",           "192.168.4.1",   nvChkIp,    0,   0,     0 },
	{ "rad.mask",         "/24",           nvChkMask,  8,   30,    0 },
	{ "rad.pwr",          "8.5",           nvChkPwr,   2,   20,    0 },
	{ "rad.nogw",         "0",             nvChkInt,   0,   1,     0 },
	{ "rad.maxcli",       "1",             nvChkInt,   1,   4,     0 },
	{ "ui.boot",          "BLE",           nvChkUi,    0,   0,     0 },
	{ "ui.baud",          "115200",        nvChkBaud,  0,   0,     0 },
	{ "ui.echo",          "1",             nvChkInt,   0,   1,     0 },
	{ "scr.bl",           "100",           nvChkInt,   0,   100,   0 },
	{ "scr.pflip",        "0",             nvChkInt,   0,   1,     0 },
	{ "scr.lflip",        "0",             nvChkInt,   0,   1,     0 },
	{ "btn.deb",          "30",            nvChkInt,   0,   200,   0 },
	{ "btn.long",         "600",           nvChkInt,   50,  5000,  0 },
	{ "btn.dbl",          "300",           nvChkInt,   50,  5000,  0 },
	{ "btn.rpt",          "0",             nvChkInt,   0,   5000,  0 },
	{ "btn.touch",        "0.015",         nvChkRatio, 0,   0,     0 },
	{ "web.hist",         "50",            nvChkInt,   10,  500,   0 },
	{ "disk.read",        "0 255 0",       nvChkRgb,   0,   255,   0 },
	{ "disk.write",       "255 0 0",       nvChkRgb,   0,   255,   0 },
	{ "imu.arange",       "4",             nvChkDbl,   2,   16,    0 },
	{ "imu.arate",        "224",           nvChkDbl,   28,  896,   0 },
	{ "imu.grange",       "512",           nvChkDbl,   16,  2048,  0 },
	{ "imu.grate",        "224",           nvChkDbl,   28,  896,   0 },
	{ "imu.wom",          "100",           nvChkInt,   1,   255,   0 },
	{ "imu.tap.thr",      "1.3",           nvChkTenth, 1,   79,    0 },
	{ "imu.tap.gap",      "500",           nvChkInt,   100, 2000,  0 },
	{ "imu.mot.thr",      "100",           nvChkInt,   32,  2000,  0 },
	{ "imu.still.thr",    "64",            nvChkInt,   32,  2000,  0 },
	{ "imu.step.thr",     "200",           nvChkInt,   50,  2000,  0 },
	{ "imu.step.min",     "10",            nvChkInt,   1,   255,   0 },
	{ "imu.orient.hold",  "500",           nvChkInt,   0,   5000,  0 },
	{ "imu.orient.flat",  "30",            nvChkInt,   5,   80,    0 },
	{ "run.files",        "4",             nvChkInt,   1,   16,    0 },
	{ "sys.autorun",      "0",             nvChkInt,   0,   1,     0 },
	{ "sys.aron",         "0 255 0",       nvChkRgb,   0,   255,   0 },
	{ "sys.aroff",        "255 0 0",       nvChkRgb,   0,   255,   0 },
	{ "sys.bootcnt",      "0",             nvChkInt,   0,   65535, NV_RO },
};

#define NV_COUNT	(int)(sizeof(s_keys) / sizeof(s_keys[0]))

static EXT_RAM_BSS_ATTR char s_val[NV_COUNT][NV_VAL_MAX];	/* current values */
static EXT_RAM_BSS_ATTR char s_stage[NV_COUNT][NV_VAL_MAX];	/* nvSetMany() working copy */
static char         s_err[80];
static nvs_handle_t s_nvs;

static int         nvFind(const char *key);
static const char *nvCross(char (*vals)[NV_VAL_MAX]);
static bool        nvParseIp(const char *s, uint32_t *ip);
static const char *nvFilePath(const char *name, char *path, size_t n);
static const char *nvExport(const char *name);
static const char *nvImport(const char *name, cmdReply_t reply);
static const char *nvCmd(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t nvInit(void)
{
	esp_err_t err = nvs_open(NV_NAMESPACE, NVS_READWRITE, &s_nvs);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
		return err;
	}

	for (int i = 0; i < NV_COUNT; i++) {
		char   raw[NV_VAL_MAX];
		size_t len = sizeof(raw);
		strcpy(s_val[i], s_keys[i].def);
		if (nvs_get_str(s_nvs, s_keys[i].key, raw, &len) == ESP_OK) {
			if (s_keys[i].check(raw, s_keys[i].min, s_keys[i].max, s_val[i]) != NULL) {
				ESP_LOGW(TAG, "%s: stored value invalid, using default", s_keys[i].key);
				strcpy(s_val[i], s_keys[i].def);
			}
		}
	}
	if (nvCross(s_val) != NULL) {				/* ip/mask pair unusable */
		ESP_LOGW(TAG, "rad.ip/rad.mask invalid together, using defaults");
		strcpy(s_val[nvFind("rad.ip")], "192.168.4.1");
		strcpy(s_val[nvFind("rad.mask")], "/24");
	}

	return cmdRegister("NV", 1, 5, nvCmd,
	                   "NV SET key value|r g b | GET [key] | RESET [key] | EXPORT name | IMPORT name");
}

const char *nvGet(const char *key)
{
	int i = nvFind(key);
	return (i < 0) ? NULL : s_val[i];
}

int nvGetInt(const char *key)
{
	const char *v = nvGet(key);
	return v ? atoi(v) : 0;
}

float nvGetFloat(const char *key)
{
	const char *v = nvGet(key);
	return v ? strtof(v, NULL) : 0.0f;
}

const char *nvSetMany(int n, const char *keys[], const char *vals[], int *badIndex)
{
	if (badIndex) {
		*badIndex = -1;
	}
	memcpy(s_stage, s_val, sizeof(s_stage));

	for (int k = 0; k < n; k++) {
		int i = nvFind(keys[k]);
		if (badIndex) {
			*badIndex = k;
		}
		if (i < 0) {
			snprintf(s_err, sizeof(s_err), "unknown key %s", keys[k]);
			return s_err;
		}
		const char *e = s_keys[i].check(vals[k], s_keys[i].min, s_keys[i].max, s_stage[i]);
		if (e) {
			snprintf(s_err, sizeof(s_err), "%s %s", s_keys[i].key, e);
			return s_err;
		}
	}
	if (badIndex) {
		*badIndex = -1;
	}
	const char *e = nvCross(s_stage);
	if (e) {
		return e;
	}

	bool changed = false;
	for (int i = 0; i < NV_COUNT; i++) {
		if (strcmp(s_stage[i], s_val[i]) != 0) {
			if (nvs_set_str(s_nvs, s_keys[i].key, s_stage[i]) != ESP_OK) {
				return "NV write failed";
			}
			strcpy(s_val[i], s_stage[i]);
			changed = true;
		}
	}
	if (changed && nvs_commit(s_nvs) != ESP_OK) {
		return "NV write failed";
	}
	return NULL;
}

void nvUnescape(char *s)
{
	char *d = s;
	for (; *s; s++) {
		if (s[0] == '\\' && s[1] == '#') {
			s++;
		}
		*d++ = *s;
	}
	*d = '\0';
}

/* ================= Checkers ================= */

static const char *nvChkText(const char *in, int min, int max, char *out)
{
	static char msg[64];
	int len = strlen(in);
	bool ok = (len >= min && len <= max);
	for (int i = 0; ok && i < len; i++) {
		ok = (in[i] > ' ' && in[i] <= '~' &&		/* printable ASCII, no space */
		      in[i] != ':' && in[i] != '|');		/* word separators in scripts */
	}
	if (!ok) {
		snprintf(msg, sizeof(msg), "must be %d..%d chars, no spaces, ':' or '|'", min, max);
		return msg;
	}
	strcpy(out, in);
	return NULL;
}

static const char *nvChkInt(const char *in, int min, int max, char *out)
{
	static char msg[32];
	int v;
	if (!cmdParseInt(in, min, max, &v)) {
		snprintf(msg, sizeof(msg), "must be %d..%d", min, max);
		return msg;
	}
	snprintf(out, NV_VAL_MAX, "%d", v);
	return NULL;
}

static const char *nvChkIp(const char *in, int min, int max, char *out)
{
	uint32_t ip;
	if (!nvParseIp(in, &ip)) {
		return "must be a.b.c.d";
	}
	uint8_t a = ip >> 24;
	if (a == 0 || a == 127 || a >= 224) {
		return "not a usable address";
	}
	snprintf(out, NV_VAL_MAX, "%u.%u.%u.%u",
	         (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 0xFF, (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
	return NULL;
}

static const char *nvChkMask(const char *in, int min, int max, char *out)
{
	int v;
	if (!cmdParseInt(in[0] == '/' ? in + 1 : in, min, max, &v)) {
		return "must be /8../30";
	}
	snprintf(out, NV_VAL_MAX, "/%d", v);
	return NULL;
}

static const char *nvChkPwr(const char *in, int min, int max, char *out)
{
	char *end;
	float v = strtof(in, &end);
	if (end == in || *end || !(v >= min && v <= max)) {
		return "must be 2..20 dBm";
	}
	int q = (int)lroundf(v * 4.0f);				/* 0.25 dBm steps */
	if (q % 4 == 0) {
		snprintf(out, NV_VAL_MAX, "%d", q / 4);
	} else if (q % 2 == 0) {
		snprintf(out, NV_VAL_MAX, "%d.5", q / 4);
	} else {
		snprintf(out, NV_VAL_MAX, "%d.%d", q / 4, (q % 4) * 25);
	}
	return NULL;
}

static const char *nvChkUi(const char *in, int min, int max, char *out)
{
	if (strcasecmp(in, "BLE") == 0) {
		strcpy(out, "BLE");
	} else if (strcasecmp(in, "TELNET") == 0) {
		strcpy(out, "TELNET");
	} else if (strcasecmp(in, "WEB") == 0) {
		strcpy(out, "WEB");
	} else if (strcasecmp(in, "UART") == 0) {
		strcpy(out, "UART");
	} else if (strcasecmp(in, "USB") == 0) {
		strcpy(out, "USB");
	} else if (strcasecmp(in, "MSC") == 0) {
		strcpy(out, "MSC");
	} else {
		return "must be BLE|TELNET|WEB|UART|USB|MSC";
	}
	return NULL;
}

static const char *nvChkRatio(const char *in, int min, int max, char *out)
{
	char *end;
	float v = strtof(in, &end);
	if (end == in || *end || !(v >= 0.001f && v <= 0.5f)) {
		return "must be 0.001..0.5";
	}
	snprintf(out, NV_VAL_MAX, "%g", v);
	return NULL;
}

/* "r g b", each 0..255, separated by spaces/tabs. */
static const char *nvChkRgb(const char *in, int min, int max, char *out)
{
	int  v[3];
	char tok[3][8];
	char extra;
	if (sscanf(in, " %7s %7s %7s %c", tok[0], tok[1], tok[2], &extra) != 3 ||
	    !cmdParseInt(tok[0], min, max, &v[0]) || !cmdParseInt(tok[1], min, max, &v[1]) ||
	    !cmdParseInt(tok[2], min, max, &v[2])) {
		return "must be r g b (0..255)";
	}
	snprintf(out, NV_VAL_MAX, "%d %d %d", v[0], v[1], v[2]);
	return NULL;
}

/* min * 2^k within min..max (e.g. 2 4 8 16, or 28 56 ... 896). */
static const char *nvChkDbl(const char *in, int min, int max, char *out)
{
	static char msg[64];
	int v;
	if (cmdParseInt(in, min, max, &v) && v % min == 0 && ((v / min) & (v / min - 1)) == 0) {
		snprintf(out, NV_VAL_MAX, "%d", v);
		return NULL;
	}
	int n = snprintf(msg, sizeof(msg), "must be");
	for (int x = min; x <= max && n < (int)sizeof(msg); x *= 2) {
		n += snprintf(msg + n, sizeof(msg) - n, "%s%d", x == min ? " " : "|", x);
	}
	return msg;
}

/* Standard UART speeds. */
static const char *nvChkBaud(const char *in, int min, int max, char *out)
{
	static const int rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
	int v;
	if (cmdParseInt(in, 9600, 921600, &v)) {
		for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
			if (v == rates[i]) {
				snprintf(out, NV_VAL_MAX, "%d", v);
				return NULL;
			}
		}
	}
	return "must be 9600|19200|38400|57600|115200|230400|460800|921600";
}

/* Decimal with at most one decimal place; min/max in tenths. */
static const char *nvChkTenth(const char *in, int min, int max, char *out)
{
	static char msg[64];
	char *end;
	float v = strtof(in, &end);
	int   t = (int)lroundf(v * 10.0f);
	if (end == in || *end || in[0] == '-' || in[0] == '+' || fabsf(v * 10.0f - t) > 0.001f ||
	    t < min || t > max) {
		snprintf(msg, sizeof(msg), "must be %d.%d..%d.%d (one decimal)", min / 10, min % 10, max / 10, max % 10);
		return msg;
	}
	snprintf(out, NV_VAL_MAX, "%d.%d", t / 10, t % 10);
	return NULL;
}

/* ================= Internals ================= */

static int nvFind(const char *key)
{
	for (int i = 0; i < NV_COUNT; i++) {
		if (strcasecmp(key, s_keys[i].key) == 0) {
			return i;
		}
	}
	return -1;
}

/* Checks across keys: rad.ip must be a host address in its rad.mask network. */
static const char *nvCross(char (*vals)[NV_VAL_MAX])
{
	uint32_t ip;
	int      bits = atoi(vals[nvFind("rad.mask")] + 1);
	uint32_t host = (1u << (32 - bits)) - 1;

	if (!nvParseIp(vals[nvFind("rad.ip")], &ip) || (ip & host) == 0 || (ip & host) == host) {
		return "rad.ip is not a host address for rad.mask";
	}
	return NULL;
}

/* Strict a.b.c.d, each 0..255. */
static bool nvParseIp(const char *s, uint32_t *ip)
{
	uint32_t v = 0;
	for (int part = 0; part < 4; part++) {
		if (*s < '0' || *s > '9') {
			return false;
		}
		int n = 0, digits = 0;
		while (*s >= '0' && *s <= '9') {
			n = n * 10 + (*s++ - '0');
			if (++digits > 3 || n > 255) {
				return false;
			}
		}
		v = (v << 8) | n;
		if (part < 3 && *s++ != '.') {
			return false;
		}
	}
	if (*s) {
		return false;
	}
	*ip = v;
	return true;
}

/* SD:/sys/<name>[.nvram]. Returns NULL or an error. */
static const char *nvFilePath(const char *name, char *path, size_t n)
{
	size_t len = strlen(name);
	if (len == 0 || len > 40 || strpbrk(name, "/\\:")) {
		return "bad file name";
	}
	bool hasExt = len >= strlen(NV_EXT) && strcasecmp(name + len - strlen(NV_EXT), NV_EXT) == 0;
	snprintf(path, n, "%s/%s%s", NV_DIR, name, hasExt ? "" : NV_EXT);
	return NULL;
}

static const char *nvExport(const char *name)
{
	char path[96];
	const char *e = nvFilePath(name, path, sizeof(path));
	if (e) {
		return e;
	}

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	if (mkdir(NV_DIR, 0777) != 0 && errno != EEXIST) {
		sdUnlock();
		return "cannot create /sys";
	}
	FILE *f = fopen(path, "w");
	if (!f) {
		sdUnlock();
		return "cannot write file";
	}
	bool ok = true;
	for (int i = 0; i < NV_COUNT && ok; i++) {
		ok = fprintf(f, "%s%s=%s\n", (s_keys[i].flags & NV_SECRET) ? "!" : "", s_keys[i].key, s_val[i]) > 0;
	}
	ok = (fclose(f) == 0) && ok;
	sdUnlock();
	return ok ? NULL : "cannot write file";
}

static const char *nvImport(const char *name, cmdReply_t reply)
{
	char         skipped[NV_FILE_LINE] = "";		/* read-only keys found */
	static EXT_RAM_BSS_ATTR char lines[NV_FILE_PAIRS][NV_FILE_LINE];
	static int   lineNo[NV_FILE_PAIRS];
	const char  *keys[NV_FILE_PAIRS], *vals[NV_FILE_PAIRS];
	int          n = 0, no = 0;
	char         path[96], buf[NV_FILE_LINE];

	const char *e = nvFilePath(name, path, sizeof(path));
	if (e) {
		return e;
	}

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		sdUnlock();
		return "file not found";
	}

	e = NULL;
	while (!e && fgets(buf, sizeof(buf), f)) {
		no++;
		char *s = buf;
		while (*s == ' ' || *s == '\t') s++;
		char *t = s + strlen(s);
		while (t > s && (t[-1] == '\n' || t[-1] == '\r' || t[-1] == ' ' || t[-1] == '\t')) *--t = '\0';
		if (*s == '\0' || *s == '#') {
			continue;				/* blank / comment */
		}
		if (*s == '!') {
			s++;					/* "!key=value": FCAT redacts it */
		}
		char *eq = strchr(s, '=');
		if (!eq || n >= NV_FILE_PAIRS) {
			snprintf(s_err, sizeof(s_err), "line %d: %s", no, eq ? "too many lines" : "expected key=value");
			e = s_err;
			break;
		}
		strcpy(lines[n], s);
		char *k = lines[n], *v = lines[n] + (eq - s);
		*v++ = '\0';
		for (char *x = v - 2; x >= k && (*x == ' ' || *x == '\t'); x--) *x = '\0';	/* trim key */
		while (*v == ' ' || *v == '\t') v++;					/* trim value */
		int ki = nvFind(k);
		if (ki >= 0 && (s_keys[ki].flags & NV_RO)) {
			if (!strstr(skipped, s_keys[ki].key) &&
			    strlen(skipped) + strlen(s_keys[ki].key) + 2 < sizeof(skipped)) {
				strcat(skipped, " ");
				strcat(skipped, s_keys[ki].key);
			}
			continue;				/* firmware-owned: ignored */
		}
		keys[n]   = k;
		vals[n]   = v;
		lineNo[n] = no;
		n++;
	}
	fclose(f);
	sdUnlock();
	if (e) {
		return e;
	}
	if (n == 0) {
		return "no settings in file";
	}

	int bad;
	e = nvSetMany(n, keys, vals, &bad);
	if (e && bad >= 0) {
		static char msg[sizeof(s_err) + 16];
		snprintf(msg, sizeof(msg), "line %d: %s", lineNo[bad], e);
		return msg;
	}
	if (!e && skipped[0]) {
		char text[NV_FILE_LINE + 40];
		snprintf(text, sizeof(text), "warning:%s ignored (read-only)\n", skipped);
		reply(text);
	}
	return e;
}

/* ================= Command ================= */

static const char *nvCmd(int argc, char *argv[], cmdReply_t reply)
{
	const char *sub = argv[0];
	char        text[NV_VAL_MAX + 24];

	if (strcasecmp(sub, "SET") == 0 && (argc == 3 || argc == 5)) {
		char joined[NV_VAL_MAX];
		if (argc == 5) {				/* r g b */
			snprintf(joined, sizeof(joined), "%.15s %.15s %.15s", argv[2], argv[3], argv[4]);
		} else {
			nvUnescape(argv[2]);
			snprintf(joined, sizeof(joined), "%s", argv[2]);
		}
		int i = nvFind(argv[1]);
		if (i >= 0 && (s_keys[i].flags & NV_RO)) {
			return "read-only key";
		}
		const char *k[] = { argv[1] }, *v[] = { joined };
		return nvSetMany(1, k, v, NULL);
	}
	if (strcasecmp(sub, "GET") == 0 && argc <= 2) {
		for (int i = 0; i < NV_COUNT; i++) {
			if (argc == 1 || strcasecmp(argv[1], s_keys[i].key) == 0) {
				snprintf(text, sizeof(text), "%s=%s\n", s_keys[i].key, s_val[i]);
				reply(text);
				if (argc == 2) {
					return NULL;
				}
			}
		}
		if (argc == 2) {
			snprintf(s_err, sizeof(s_err), "unknown key %s", argv[1]);
			return s_err;
		}
		return NULL;
	}
	if (strcasecmp(sub, "RESET") == 0 && argc <= 2) {
		if (argc == 2) {
			int i = nvFind(argv[1]);
			if (i < 0) {
				snprintf(s_err, sizeof(s_err), "unknown key %s", argv[1]);
				return s_err;
			}
			if ((s_keys[i].flags & NV_RO)) {
				return "read-only key";
			}
			const char *k[] = { s_keys[i].key }, *v[] = { s_keys[i].def };
			return nvSetMany(1, k, v, NULL);
		}
		if (nvs_erase_all(s_nvs) != ESP_OK) {
			return "NV write failed";
		}
		for (int i = 0; i < NV_COUNT; i++) {
			if ((s_keys[i].flags & NV_RO)) {				/* firmware-owned: keep */
				if (nvs_set_str(s_nvs, s_keys[i].key, s_val[i]) != ESP_OK) {
					return "NV write failed";
				}
			} else {
				strcpy(s_val[i], s_keys[i].def);
			}
		}
		return (nvs_commit(s_nvs) == ESP_OK) ? NULL : "NV write failed";
	}
	if (strcasecmp(sub, "EXPORT") == 0 && argc == 2) {
		return nvExport(argv[1]);
	}
	if (strcasecmp(sub, "IMPORT") == 0 && argc == 2) {
		return nvImport(argv[1], reply);
	}
	return "usage: NV SET key value|r g b | GET [key] | RESET [key] | EXPORT name | IMPORT name";
}
