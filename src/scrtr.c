#include "scrtr.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "script.h"

/* ================= Transitions ================= */

/* FADE: per-channel RGB565 blend from old to new. */
static void scrtrFadeFrame(const scrtrCtx_t *c, int y0, int rows, int p)
{
	int       bw = c->x1 - c->x0;
	uint16_t *o  = c->out;

	for (int y = y0; y < y0 + rows; y++) {
		const uint16_t *f = c->from + y * c->w + c->x0;
		const uint16_t *t = c->to + y * c->w + c->x0;
		for (int i = 0; i < bw; i++) {
			int a = f[i], b = t[i];
			int r = (a >> 11)         + ((((b >> 11) & 0x1F) - (a >> 11)) * p >> 10);
			int g = ((a >> 5) & 0x3F) + ((((b >> 5) & 0x3F) - ((a >> 5) & 0x3F)) * p >> 10);
			int u = (a & 0x1F)        + (((b & 0x1F) - (a & 0x1F)) * p >> 10);
			*o++ = (uint16_t)((r << 11) | (g << 5) | u);
		}
	}
}

/* PUSH: old image moves by d along (mx,my); the new one follows right behind it. */
static void scrtrPushFrame(const scrtrCtx_t *c, int y0, int rows, int p)
{
	int       span = c->mx ? c->w : c->h;
	int       d    = span * p >> 10;
	int       ox   = c->mx * d, oy = c->my * d;		/* old image offset */
	int       nx   = ox - c->mx * span;			/* new image offset */
	int       ny   = oy - c->my * span;
	int       bw   = c->x1 - c->x0;
	uint16_t *o    = c->out;

	for (int y = y0; y < y0 + rows; y++) {
		for (int x = c->x0; x < c->x0 + bw; x++) {
			int sx = x - ox, sy = y - oy;
			if (sx >= 0 && sx < c->w && sy >= 0 && sy < c->h) {
				*o++ = c->from[sy * c->w + sx];
			} else {
				*o++ = c->to[(y - ny) * c->w + (x - nx)];
			}
		}
	}
}

/* WIPE: a hard edge sweeps in from the entry side; neither image moves. */
static void scrtrWipeFrame(const scrtrCtx_t *c, int y0, int rows, int p)
{
	int       span = c->mx ? c->w : c->h;
	int       d    = span * p >> 10;				/* revealed depth */
	int       bw   = c->x1 - c->x0;
	uint16_t *o    = c->out;

	for (int y = y0; y < y0 + rows; y++) {
		const uint16_t *f = c->from + y * c->w;
		const uint16_t *t = c->to + y * c->w;
		for (int x = c->x0; x < c->x0 + bw; x++) {
			bool isNew = (c->mx < 0) ? (x >= c->w - d) :	/* entering from the native right */
			             (c->mx > 0) ? (x < d) :
			             (c->my < 0) ? (y >= c->h - d) :
			                           (y < d);
			*o++ = isNew ? t[x] : f[x];
		}
	}
}

/* DISSOLVE: each pixel flips to the new image at its own random moment. */
static void scrtrDissolveFrame(const scrtrCtx_t *c, int y0, int rows, int p)
{
	int       bw = c->x1 - c->x0;
	uint16_t *o  = c->out;

	for (int y = y0; y < y0 + rows; y++) {
		const uint16_t *f = c->from + y * c->w;
		const uint16_t *t = c->to + y * c->w;
		for (int x = c->x0; x < c->x0 + bw; x++) {
			uint32_t h = (uint32_t)(y * c->w + x) * 0x9E3779B1u ^ c->seed;	/* hash -> 0..1023 */
			h ^= h >> 16;
			h *= 0x85EBCA6Bu;
			h ^= h >> 13;
			*o++ = ((int)(h & 1023) < p) ? t[x] : f[x];
		}
	}
}

/* ================= Table: add new transitions here (see scrtr.h) ================= */

typedef enum {
	SCRTR_ARGS_MS,		/* ms */
	SCRTR_ARGS_MS_DIR,	/* ms T|B|L|R */
} scrtrArgs_t;

typedef struct {
	const char  *name;
	scrtrArgs_t  args;
	bool         full;	/* true: whole screen; false: dirty rectangle */
	void       (*frame)(const scrtrCtx_t *c, int y0, int rows, int p);
} scrtrDef_t;

static const scrtrDef_t s_table[] = {
	{ "PUSH", SCRTR_ARGS_MS_DIR, true,  scrtrPushFrame },
	{ "FADE", SCRTR_ARGS_MS,     false, scrtrFadeFrame },
	{ "WIPE", SCRTR_ARGS_MS_DIR, true,  scrtrWipeFrame },
	{ "DISSOLVE", SCRTR_ARGS_MS, false, scrtrDissolveFrame },
};

#define SCRTR_COUNT	(int)(sizeof(s_table) / sizeof(s_table[0]))
#define SCRTR_MS_MIN	10
#define SCRTR_MS_MAX	10000

/* ================= State ================= */

typedef struct {
	const scrtrDef_t *def;	/* NULL = CUT */
	int               ms;
	char              dir;	/* 'T' 'B' 'L' 'R' or 0 */
} scrtrSet_t;

static bool       s_overArmed;	/* firmware's own one-off (scrtrOverride) */
static scrtrSet_t s_over;
static bool       s_onceArmed;
static scrtrSet_t s_once;
static scrtrSet_t s_pers;	/* def NULL = CUT */

/* ================= Helpers ================= */

static void scrtrFormat(const scrtrSet_t *s, char *out, size_t n)
{
	if (!s->def) {
		snprintf(out, n, "CUT");
	} else if (s->def->args == SCRTR_ARGS_MS_DIR) {
		snprintf(out, n, "%s %d %c", s->def->name, s->ms, s->dir);
	} else {
		snprintf(out, n, "%s %d", s->def->name, s->ms);
	}
}

static const char *scrtrUsage(void)
{
	static char buf[200];
	if (!buf[0]) {
		int n = snprintf(buf, sizeof(buf), "usage: SCR TR|TRP");
		for (int i = 0; i < SCRTR_COUNT && n < (int)sizeof(buf); i++) {
			n += snprintf(buf + n, sizeof(buf) - n, " %s %s |", s_table[i].name,
			              s_table[i].args == SCRTR_ARGS_MS_DIR ? "ms T|B|L|R" : "ms");
		}
		if (n < (int)sizeof(buf)) {
			snprintf(buf + n, sizeof(buf) - n, " CUT | NONE");
		}
	}
	return buf;
}

/* Parse "name args..." into s. Returns false on any error. */
static bool scrtrParse(int argc, char *argv[], scrtrSet_t *s)
{
	const scrtrDef_t *def = NULL;
	for (int i = 0; i < SCRTR_COUNT; i++) {
		if (strcasecmp(argv[0], s_table[i].name) == 0) {
			def = &s_table[i];
			break;
		}
	}
	if (!def) {
		return false;
	}

	int need = (def->args == SCRTR_ARGS_MS_DIR) ? 3 : 2;
	if (argc != need) {
		return false;
	}

	char *end;
	long  ms = strtol(argv[1], &end, 10);
	if (*end || end == argv[1] || ms < SCRTR_MS_MIN || ms > SCRTR_MS_MAX) {
		return false;
	}

	char dir = 0;
	if (def->args == SCRTR_ARGS_MS_DIR) {
		if (strlen(argv[2]) != 1 || !strchr("TBLRtblr", argv[2][0])) {
			return false;
		}
		dir = argv[2][0] & ~0x20;			/* upper case */
	}

	s->def = def;
	s->ms  = (int)ms;
	s->dir = dir;
	return true;
}

/* Logical direction -> native unit vector the old image moves in. */
static void scrtrDirVector(const scrtrEnv_t *env, char dir, int *mx, int *my)
{
	int lx = 0, ly = 0;
	switch (dir) {
	case 'R': lx = -1; break;		/* new enters right: old moves left */
	case 'L': lx =  1; break;
	case 'B': ly = -1; break;		/* new enters bottom: old moves up */
	case 'T': ly =  1; break;
	default:  break;
	}
	int step = lx * env->sx + ly * env->sy;		/* native index step */
	*mx = (step ==  1) ? 1 : (step == -1) ? -1 : 0;
	*my = (step == env->w) ? 1 : (step == -env->w) ? -1 : 0;
}

/* ================= Public ================= */

esp_err_t scrtrRun(const scrtrEnv_t *env)
{
	scrtrSet_t set = s_pers;
	if (s_overArmed) {				/* firmware one-off: user settings untouched */
		set         = s_over;
		s_overArmed = false;
	} else if (s_onceArmed) {
		set         = s_once;
		s_onceArmed = false;
	}
	if (!set.def) {
		return ESP_ERR_NOT_FOUND;			/* CUT */
	}

	scrtrCtx_t c = {
		.from = env->from,
		.to   = env->to,
		.out  = env->buf,
		.w    = env->w,
		.h    = env->h,
		.seed = esp_random(),
	};
	if (set.def->full) {
		c.x0 = 0;
		c.y0 = 0;
		c.x1 = env->w;
		c.y1 = env->h;
	} else {
		c.x0 = env->x0;
		c.y0 = env->y0;
		c.x1 = env->x1;
		c.y1 = env->y1;
	}
	scrtrDirVector(env, set.dir, &c.mx, &c.my);

	int     bw    = c.x1 - c.x0;
	int     band  = env->bufPx / bw;			/* rows per DMA band */
	int64_t start = esp_timer_get_time();
	int     p;

	do {
		int64_t ms = (esp_timer_get_time() - start) / 1000;
		p = (ms >= set.ms || scriptAbortRequested()) ? 1024 : (int)(ms * 1024 / set.ms);	/* STOP NOW: last frame */

		for (int y = c.y0; y < c.y1; y += band) {
			int rows = (c.y1 - y < band) ? c.y1 - y : band;
			set.def->frame(&c, y, rows, p);
			esp_err_t err = env->blit(c.x0, y, bw, rows);
			if (err != ESP_OK) {
				return err;
			}
		}
	} while (p < 1024);

	return ESP_OK;
}

bool scrtrOverride(const char *name, int ms, char dir)
{
	char  msText[12], dirText[2] = { dir, '\0' };
	char *argv[3] = { (char *)name, msText, dirText };
	snprintf(msText, sizeof(msText), "%d", ms);

	if (!name) {
		s_overArmed = false;				/* cancel */
		return true;
	}
	scrtrSet_t s;
	if (!scrtrParse(dir ? 3 : 2, argv, &s)) {
		return false;
	}
	s_over      = s;
	s_overArmed = true;
	return true;
}

const char *scrtrCommand(bool persistent, int argc, char *argv[], cmdReply_t reply)
{
	char x[32], y[32], text[72];

	if (argc == 0) {					/* report */
		scrtrFormat(&s_pers, y, sizeof(y));
		if (persistent) {
			snprintf(text, sizeof(text), "%s\n", y);
		} else if (!s_onceArmed) {
			snprintf(text, sizeof(text), "> %s\n", y);
		} else {
			scrtrFormat(&s_once, x, sizeof(x));
			if (s_pers.def) {
				snprintf(text, sizeof(text), "%s > %s\n", x, y);
			} else {
				snprintf(text, sizeof(text), "%s\n", x);
			}
		}
		reply(text);
		return NULL;
	}

	if (argc == 2 && strcasecmp(argv[0], "NONE") == 0 && strcasecmp(argv[1], "NONE") == 0) {
		s_onceArmed = false;
		s_pers.def  = NULL;
		return NULL;
	}

	if (argc == 1 && (strcasecmp(argv[0], "NONE") == 0 || strcasecmp(argv[0], "CUT") == 0)) {
		bool cut = (strcasecmp(argv[0], "CUT") == 0);
		if (persistent) {
			s_pers.def = NULL;			/* NONE = CUT */
		} else if (cut) {
			s_once.def  = NULL;
			s_onceArmed = true;
		} else {
			s_onceArmed = false;
		}
		return NULL;
	}

	scrtrSet_t s;
	if (!scrtrParse(argc, argv, &s)) {
		return scrtrUsage();
	}
	if (persistent) {
		s_pers = s;
	} else {
		s_once      = s;
		s_onceArmed = true;
	}
	return NULL;
}
