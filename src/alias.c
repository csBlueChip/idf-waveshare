#include "alias.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cmd.h"
#include "sd.h"

#define TAG		"alias"
#define ALIAS_FILE	SD_MOUNT_POINT "/sys/alias.cfg"
#define ALIAS_TMP	SD_MOUNT_POINT "/sys/alias.tmp"
#define ALIAS_LINE_MAX	160	/* a line of the file */
#define ALIAS_ARGS	9	/* $1..$9 */

typedef struct {
	char name[ALIAS_NAME_MAX + 1];
	char text[ALIAS_TEXT_MAX + 1];
} alias_t;

static EXT_RAM_BSS_ATTR alias_t s_al[ALIAS_MAX];
static int               s_count;
static SemaphoreHandle_t s_lock;

/* ================= Prototypes ================= */

static const char *aliasCmd(const char *args, cmdReply_t reply);
static void        aliasLoad(cmdReply_t reply);
static const char *aliasCheck(const char *name, const char *text);
static int         aliasFind(const char *name);
static const char *aliasWrite(const char *name, const char *text);
static const char *aliasWord(const char **p, char *out, size_t n, bool rest);
static void        aliasWarn(cmdReply_t reply, const char *text);

/* ================= Public API ================= */

esp_err_t aliasInit(void)
{
	s_lock = xSemaphoreCreateMutex();
	if (!s_lock) {
		return ESP_ERR_NO_MEM;
	}
	esp_err_t err = cmdRegisterRaw("ALIAS", aliasCmd,
	                               "ALIAS [ADD name text | DEL name | LOAD]  UI shortcuts (/sys/alias.cfg)");
	if (err == ESP_OK) {
		aliasLoad(NULL);
	}
	return err;
}

const char *aliasExpand(const char *line, char *out, size_t n)
{
	const char *p = line;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	size_t wl = strcspn(p, " \t");
	if (wl == 0 || wl > ALIAS_NAME_MAX) {
		return line;
	}
	char name[ALIAS_NAME_MAX + 1];
	memcpy(name, p, wl);
	name[wl] = '\0';

	char text[ALIAS_TEXT_MAX + 1];
	xSemaphoreTake(s_lock, portMAX_DELAY);
	int i = aliasFind(name);
	if (i >= 0) {
		strcpy(text, s_al[i].text);
	}
	xSemaphoreGive(s_lock);
	if (i < 0) {
		return line;
	}

	/* the arguments: words after the name, up to a comment */
	char        rest[CMD_LINE_MAX + 1];
	size_t      rl = 0;
	const char *a  = p + wl;
	while (*a == ' ' || *a == '\t') {
		a++;
	}
	for (; *a && rl < sizeof(rest) - 1; a++) {
		if (*a == '#') {
			break;
		}
		rest[rl++] = *a;
		if (*a == '\\' && a[1] && rl < sizeof(rest) - 1) {
			rest[rl++] = *++a;			/* "\#" is not a comment */
		}
	}
	while (rl && (rest[rl - 1] == ' ' || rest[rl - 1] == '\t')) {
		rl--;
	}
	rest[rl] = '\0';

	char  words[CMD_LINE_MAX + 1];
	char *argv[ALIAS_ARGS];
	int   argc = 0;
	char *save = NULL;
	strcpy(words, rest);
	for (char *w = strtok_r(words, " \t", &save); w && argc < ALIAS_ARGS; w = strtok_r(NULL, " \t", &save)) {
		argv[argc++] = w;
	}

	size_t o    = 0;
	bool   used = false, full = false;
#define PUT(s)	do { size_t l_ = strlen(s); if (o + l_ >= n) { full = true; } else { memcpy(out + o, s, l_); o += l_; } } while (0)
	for (const char *t = text; *t && !full; t++) {
		if (t[0] == '$' && t[1] >= '1' && t[1] <= '9') {
			int k = t[1] - '1';
			if (k < argc) {
				PUT(argv[k]);
			}
			used = true;
			t++;
		} else if (t[0] == '$' && t[1] == '*') {
			PUT(rest);
			used = true;
			t++;
		} else {
			char c[2] = { *t, '\0' };
			PUT(c);
		}
	}
	if (!used && rest[0] && !full) {
		PUT(" ");
		PUT(rest);
	}
#undef PUT
	if (full) {
		return NULL;
	}
	out[o] = '\0';
	return out;
}

/* ================= Internals ================= */

static int aliasFind(const char *name)
{
	for (int i = 0; i < s_count; i++) {
		if (strcasecmp(s_al[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

/* NULL = a good alias; else why not */
static const char *aliasCheck(const char *name, const char *text)
{
	size_t l = strlen(name);
	if (l == 0 || l > ALIAS_NAME_MAX) {
		return "name must be 1..15 chars";
	}
	for (size_t i = 0; i < l; i++) {
		if (!isalnum((unsigned char)name[i]) && name[i] != '_') {
			return "name: letters, digits, '_' only";
		}
	}
	if (cmdExists(name)) {
		return "built-in command";
	}
	l = strlen(text);
	if (l == 0 || l > ALIAS_TEXT_MAX) {
		return "text must be 1..63 chars";
	}
	return NULL;
}

static void aliasWarn(cmdReply_t reply, const char *text)
{
	if (reply) {
		char out[ALIAS_LINE_MAX + 40];
		snprintf(out, sizeof(out), "warning: %s\n", text);
		reply(out);
	} else {
		ESP_LOGW(TAG, "alias.cfg: %s", text);
	}
}

/* (Re)read the file; skipped lines are warned about. */
static void aliasLoad(cmdReply_t reply)
{
	static EXT_RAM_BSS_ATTR alias_t al[ALIAS_MAX];
	char line[ALIAS_LINE_MAX], msg[ALIAS_LINE_MAX + 40];
	int  n = 0, no = 0;

	sdLock();
	FILE *f = (sdEnsureMounted() == ESP_OK) ? fopen(ALIAS_FILE, "r") : NULL;
	while (f && fgets(line, sizeof(line), f)) {
		no++;
		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		char *t = s + strlen(s);
		while (t > s && (t[-1] == '\n' || t[-1] == '\r' || t[-1] == ' ' || t[-1] == '\t')) *--t = '\0';
		if (*s == '\0' || *s == '#') {
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			snprintf(msg, sizeof(msg), "line %d: expected name=text", no);
			aliasWarn(reply, msg);
			continue;
		}
		*eq = '\0';
		char *v = eq + 1;
		for (char *x = eq - 1; x >= s && (*x == ' ' || *x == '\t'); x--) *x = '\0';
		while (*v == ' ' || *v == '\t') v++;
		const char *why = aliasCheck(s, v);
		bool dup = false;
		for (int i = 0; !why && i < n; i++) {
			dup = dup || strcasecmp(al[i].name, s) == 0;
		}
		if (!why && dup) {
			why = "duplicate";
		}
		if (!why && n >= ALIAS_MAX) {
			why = "too many (max 32)";
		}
		if (why) {
			snprintf(msg, sizeof(msg), "line %d: %.40s ignored (%s)", no, s, why);
			aliasWarn(reply, msg);
			continue;
		}
		strcpy(al[n].name, s);
		strcpy(al[n].text, v);
		n++;
	}
	if (f) {
		fclose(f);
	}
	sdUnlock();

	xSemaphoreTake(s_lock, portMAX_DELAY);
	memcpy(s_al, al, sizeof(al[0]) * n);
	s_count = n;
	xSemaphoreGive(s_lock);
	ESP_LOGI(TAG, "%d alias(es)", n);
}

/* Rewrite the file: the line of name replaced by name=text (text NULL: the
 * line removed; not there: added at the end). Other lines are kept. */
static const char *aliasWrite(const char *name, const char *text)
{
	char        line[ALIAS_LINE_MAX];
	bool        done = false;
	const char *err  = NULL;

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	FILE *in  = fopen(ALIAS_FILE, "r");
	FILE *out = fopen(ALIAS_TMP, "w");
	if (!out) {
		if (in) {
			fclose(in);
		}
		sdUnlock();
		return "cannot write /sys/alias.cfg";
	}
	while (in && fgets(line, sizeof(line), in)) {
		const char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		size_t nl = strcspn(s, " \t=");
		bool   it = *s != '#' && nl == strlen(name) && strncasecmp(s, name, nl) == 0 && strchr(s, '=');
		if (it) {
			if (text && !done) {
				fprintf(out, "%s=%s\n", name, text);
			}
			done = true;				/* later copies of it go */
			continue;
		}
		fputs(line, out);
		if (line[strlen(line) - 1] != '\n') {
			fputc('\n', out);
		}
	}
	if (text && !done) {
		fprintf(out, "%s=%s\n", name, text);
	}
	if (in) {
		fclose(in);
	}
	if (fclose(out) != 0) {
		err = "write failed";
	} else {
		remove(ALIAS_FILE);
		if (rename(ALIAS_TMP, ALIAS_FILE) != 0) {
			err = "rename failed";
		}
	}
	if (err) {
		remove(ALIAS_TMP);
	}
	sdUnlock();
	return err;
}

/* Next word at *p (or "quoted text"); rest: the remainder of the line
 * (quotes stripped if it is one quoted string). NULL = OK. */
static const char *aliasWord(const char **p, char *out, size_t n, bool rest)
{
	const char *s = *p;
	while (*s == ' ' || *s == '\t') s++;
	size_t l;
	if (*s == '"') {
		const char *q = strchr(s + 1, '"');
		if (!q) {
			return "missing closing quote";
		}
		l = (size_t)(q - s - 1);
		if (l >= n) {
			return "too long";
		}
		memcpy(out, s + 1, l);
		out[l] = '\0';
		s = q + 1;
		while (*s == ' ' || *s == '\t') s++;
		if (rest && *s) {
			return "text after the closing quote";
		}
	} else {
		l = rest ? strlen(s) : strcspn(s, " \t");
		if (l >= n) {
			return "too long";
		}
		memcpy(out, s, l);
		out[l] = '\0';
		s += l;
	}
	*p = s;
	return NULL;
}

/* ALIAS [ADD name text | DEL name | LOAD] */
static const char *aliasCmd(const char *args, cmdReply_t reply)
{
	static char err[64];
	char        sub[8], name[ALIAS_NAME_MAX + 2], text[ALIAS_TEXT_MAX + 2];
	const char *p = args, *e;

	if (aliasWord(&p, sub, sizeof(sub), false) || sub[0] == '\0') {
		if (args[strspn(args, " \t")] != '\0') {
			return "usage: ALIAS [ADD name text | DEL name | LOAD]";
		}
		char line[ALIAS_NAME_MAX + ALIAS_TEXT_MAX + 4];
		xSemaphoreTake(s_lock, portMAX_DELAY);
		int n = s_count;
		for (int i = 0; i < n; i++) {
			snprintf(line, sizeof(line), "%s=%s\n", s_al[i].name, s_al[i].text);
			reply(line);
		}
		xSemaphoreGive(s_lock);
		if (n == 0) {
			reply("none\n");
		}
		return NULL;
	}
	if (strcasecmp(sub, "LOAD") == 0 && p[strspn(p, " \t")] == '\0') {
		aliasLoad(reply);
		return NULL;
	}
	bool add = strcasecmp(sub, "ADD") == 0, del = strcasecmp(sub, "DEL") == 0;
	if (!add && !del) {
		return "usage: ALIAS [ADD name text | DEL name | LOAD]";
	}
	if ((e = aliasWord(&p, name, sizeof(name), del)) != NULL) {
		snprintf(err, sizeof(err), "name: %s", e);
		return err;
	}
	if (del) {
		xSemaphoreTake(s_lock, portMAX_DELAY);
		int i = aliasFind(name);
		xSemaphoreGive(s_lock);
		if (i < 0) {
			return "not an alias";
		}
		if ((e = aliasWrite(name, NULL)) != NULL) {
			return e;
		}
		xSemaphoreTake(s_lock, portMAX_DELAY);
		i = aliasFind(name);
		if (i >= 0) {
			memmove(&s_al[i], &s_al[i + 1], sizeof(s_al[0]) * (s_count - i - 1));
			s_count--;
		}
		xSemaphoreGive(s_lock);
		return NULL;
	}
	if ((e = aliasWord(&p, text, sizeof(text), true)) != NULL) {
		snprintf(err, sizeof(err), "text: %s", e);
		return err;
	}
	if ((e = aliasCheck(name, text)) != NULL) {
		return e;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	bool full = aliasFind(name) < 0 && s_count >= ALIAS_MAX;
	xSemaphoreGive(s_lock);
	if (full) {
		return "too many (max 32)";
	}
	if ((e = aliasWrite(name, text)) != NULL) {
		return e;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	int i = aliasFind(name);
	if (i < 0) {
		i = s_count++;
	}
	strcpy(s_al[i].name, name);
	strcpy(s_al[i].text, text);
	xSemaphoreGive(s_lock);
	return NULL;
}
