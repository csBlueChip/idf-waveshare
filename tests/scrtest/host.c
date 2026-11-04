/* PC test host for the script interpreter core (src/scr_*.c).
 *
 *   build: see run.sh     use: scrtest dir name [args]
 *
 * Fake commands (everything else: prints "CMD: line", replies OK):
 *   FAIL reason        ERR reason
 *   REPLY a b ...      replies one line per word, then OK
 *   EVT text           queues event text (e.g. EVT BTN 1 SHORT)
 *   XINPUT text        queues a console line for INPUT
 *   XGO mode           queues a debugger answer (CONT STEP IN OUT STOP)
 *   XSTOP / XPAUSE     request STOP / PAUSE (seen between statements)
 *   XCLIENT 0|1        client connected (INPUT with none: -1)
 * Time is virtual: WAIT / time-outs advance it; timers tick on it.
 * On a pause it prints STACK and VARS, then takes the next XGO (default CONT).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "scr.h"
#include "scr_host.h"

static const char *s_dir;
static int64_t     s_ms = 1000;
static int         s_req;
static bool        s_client = true;

#define Q 64
static char s_ev[Q][256];
static int  s_nev, s_dropped;
static char s_in[Q][256];
static int  s_nin;
static int  s_go[Q];
static int  s_ngo;

static struct { int ms; int64_t next; } s_tim[5];

char *scrHostRead(const char *name, size_t *len, const char **err)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.s", s_dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		*err = "file not found";
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = calloc(1, n + 1);
	if (fread(b, 1, n, f) != (size_t)n) {
		*err = "read failed";
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = n;
	return b;
}

void *scrHostAlloc(size_t n)			{ return calloc(1, n); }
void *scrHostRealloc(void *p, size_t n)		{ return realloc(p, n); }
void  scrHostFree(void *p)			{ free(p); }

static void post(const char *t)
{
	if (s_nev == Q) {
		memmove(s_ev[0], s_ev[1], sizeof(s_ev[0]) * (Q - 1));
		s_nev--;
		s_dropped++;
	}
	snprintf(s_ev[s_nev++], sizeof(s_ev[0]), "%s", t);
}

void scrHostCmd(const char *line, void (*cb)(void *ctx, const char *text), void *ctx)
{
	char buf[600];
	if (strncasecmp(line, "FAIL", 4) == 0) {
		snprintf(buf, sizeof(buf), "ERR %s\n", line[4] ? line + 5 : "failed");
		cb(ctx, buf);
		return;
	}
	if (strncasecmp(line, "REPLY ", 6) == 0) {
		char tmp[600];
		snprintf(tmp, sizeof(tmp), "%s", line + 6);
		for (char *w = strtok(tmp, " "); w; w = strtok(NULL, " ")) {
			snprintf(buf, sizeof(buf), "%s\n", w);
			cb(ctx, buf);
		}
		cb(ctx, "OK\n");
		return;
	}
	if (strncasecmp(line, "EVT ", 4) == 0) {
		post(line + 4);
	} else if (strncasecmp(line, "XINPUT ", 7) == 0) {
		snprintf(s_in[s_nin++], sizeof(s_in[0]), "%s", line + 7);
	} else if (strncasecmp(line, "XGO ", 4) == 0) {
		const char *m = line + 4;
		s_go[s_ngo++] = !strcasecmp(m, "STEP") ? SCR_GO_STEP : !strcasecmp(m, "IN") ? SCR_GO_IN
		              : !strcasecmp(m, "OUT") ? SCR_GO_OUT : !strcasecmp(m, "STOP") ? SCR_GO_STOP
		              : SCR_GO_CONT;
	} else if (!strcasecmp(line, "XSTOP")) {
		s_req |= SCR_CHK_STOP;
	} else if (!strcasecmp(line, "XPAUSE")) {
		s_req |= SCR_CHK_PAUSE;
	} else if (strncasecmp(line, "XCLIENT ", 8) == 0) {
		s_client = atoi(line + 8);
	} else {
		printf("CMD: %s\n", line);
	}
	cb(ctx, "OK\n");
}

bool scrHostIsCommand(const char *name)
{
	static const char *c[] = { "ECHO", "CLS", "LED", "UI", "NV", "IMU", "BTN", "FAIL", "REPLY", "EVT",
	                           "ACC", "GYR", "FBOX", "LINE", "APPEND", NULL };
	for (int i = 0; c[i]; i++) {
		if (!strcasecmp(c[i], name)) {
			return true;
		}
	}
	return false;
}

void scrHostPrint(const char *text)		{ printf("> %s", text); }
void scrHostDebug(const char *text)		{ printf("DEBUG: %s\n", text); }
void scrHostNotify(const char *text)		{ printf("%s", text); }

bool scrHostNv(const char *key, char *out, size_t n)
{
	if (!strcasecmp(key, "scr.bl")) {
		snprintf(out, n, "50");
		return true;
	}
	if (!strcasecmp(key, "rad.ssid")) {
		snprintf(out, n, "wavedemo");
		return true;
	}
	return false;
}

bool scrHostBuiltin(const char *name, char *out, size_t n, const char **err)
{
	static const struct { const char *n, *v; } b[] = {
		{ "ax", "0.012" }, { "ay", "-0.5" }, { "az", "1.001" }, { "gx", "1" }, { "gy", "2" }, { "gz", "3" },
		{ "temp", "24.5" }, { "bat", "4.1" }, { "steps", "0" }, { "orient", "1" }, { "time", "b0001_000010" },
		{ "w", "172" }, { "h", "320" }, { "rot", "0" }, { "fw", "1.0.0" }, { NULL, NULL },
	};
	if (!strcasecmp(name, "uptime")) {
		snprintf(out, n, "%lld", (long long)s_ms);
		return true;
	}
	for (int i = 0; b[i].n; i++) {
		if (!strcasecmp(b[i].n, name)) {
			snprintf(out, n, "%s", b[i].v);
			return true;
		}
	}
	(void)err;
	return false;
}

const char *scrHostImu(bool gyro, double v[3])
{
	v[0] = gyro ? 1.5 : 0.01;
	v[1] = gyro ? -2 : 0.02;
	v[2] = gyro ? 0.25 : 0.98;
	return NULL;
}

static uint32_t s_rng = 1;
uint32_t scrHostTrng(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return s_rng >> 1;
}

uint32_t scrPrng(uint32_t *st)
{
	uint32_t x = *st;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *st = x;
}

int64_t scrHostMs(void)				{ return s_ms; }

int scrHostCheck(void)
{
	int r = s_req;
	s_req = 0;
	return r;
}

/* timers: generate ticks up to time t */
static void timersTo(int64_t t)
{
	for (;;) {
		int     best = 0;
		int64_t when = 0;
		for (int i = 1; i <= 4; i++) {
			if (s_tim[i].ms && s_tim[i].next <= t && (!best || s_tim[i].next < when)) {
				best = i;
				when = s_tim[i].next;
			}
		}
		if (!best) {
			return;
		}
		s_tim[best].next += s_tim[best].ms;
		/* merge with a queued tick of the same timer */
		char pre[16];
		snprintf(pre, sizeof(pre), "TIMER %d", best);
		bool merged = false;
		for (int i = 0; i < s_nev; i++) {
			if (strncmp(s_ev[i], pre, strlen(pre)) == 0 && (s_ev[i][strlen(pre)] == '\0' || s_ev[i][strlen(pre)] == ' ')) {
				int cnt = s_ev[i][strlen(pre)] ? atoi(s_ev[i] + strlen(pre) + 1) : 1;
				snprintf(s_ev[i], sizeof(s_ev[0]), "%s %d", pre, cnt + 1);
				merged = true;
			}
		}
		if (!merged) {
			post(pre);
		}
	}
}

bool scrHostSleep(int ms)
{
	s_ms += ms;
	timersTo(s_ms);
	return true;
}

static int take(int mask, char *out, size_t n)
{
	for (int i = 0; i < s_nev; i++) {
		if (scrEventSource(s_ev[i]) & mask) {
			snprintf(out, n, "%s", s_ev[i]);
			memmove(s_ev[i], s_ev[i + 1], sizeof(s_ev[0]) * (s_nev - i - 1));
			s_nev--;
			return 1;
		}
	}
	return 0;
}

int scrHostEvent(int mask, char *out, size_t n, int ms)
{
	timersTo(s_ms);
	if (take(mask, out, n)) {
		return 1;
	}
	if (ms == 0) {
		return 0;
	}
	/* jump to the next timer tick within the wait */
	int64_t end = ms < 0 ? s_ms + 3600000 : s_ms + ms;
	for (;;) {
		int64_t next = end;
		for (int i = 1; i <= 4; i++) {
			if (s_tim[i].ms && s_tim[i].next < next) {
				next = s_tim[i].next;
			}
		}
		s_ms = next;
		timersTo(s_ms);
		if (take(mask, out, n)) {
			return 1;
		}
		if (s_ms >= end) {
			if (ms < 0) {
				printf("[host: WAIT EVENT forever with nothing to come - stopping]\n");
				return -1;
			}
			return 0;
		}
	}
}

int scrHostEventsDropped(void)			{ return s_dropped; }

void scrHostTimer(int n, int ms)
{
	if (n == 0) {
		memset(s_tim, 0, sizeof(s_tim));
		return;
	}
	s_tim[n].ms   = ms;
	s_tim[n].next = s_ms + ms;
	if (!ms) {
		char pre[16];
		snprintf(pre, sizeof(pre), "TIMER %d", n);
		for (int i = 0; i < s_nev; i++) {
			if (strncmp(s_ev[i], pre, strlen(pre)) == 0) {
				memmove(s_ev[i], s_ev[i + 1], sizeof(s_ev[0]) * (s_nev - i - 1));
				s_nev--;
				i--;
			}
		}
	}
}

int scrHostInput(char *out, size_t n, int ms)
{
	if (!s_client) {
		post("UI DISCONNECT");
		return -1;
	}
	if (s_nin) {
		snprintf(out, n, "%s", s_in[0]);
		memmove(s_in[0], s_in[1], sizeof(s_in[0]) * (s_nin - 1));
		s_nin--;
		return 1;
	}
	s_ms += ms < 0 ? 0 : ms;
	return 0;
}

static void out(const char *t)			{ printf("  %s", t); }
static void nullCb(void *ctx, const char *t)	{ (void)ctx; (void)t; }

int scrHostPause(void)
{
	printf("[paused] stack: ");
	scrCmdStack(out);
	printf("[paused] vars:\n");
	scrCmdVars(out);
	int go = SCR_GO_CONT;
	if (s_ngo) {
		go = s_go[0];
		memmove(s_go, s_go + 1, sizeof(int) * (s_ngo - 1));
		s_ngo--;
	}
	static const char *names[] = { "CONT", "STEP", "IN", "OUT", "STOP" };
	printf("[resume %s]\n", names[go]);
	return go;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "use: scrtest dir name [args] [-load] [-bpt file:n] [-fn name] [-go STEP,IN,...]\n");
		return 2;
	}
	s_dir = argv[1];
	char args[256] = "";
	bool load = false;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "-load")) {
			load = true;
		} else if (!strcmp(argv[i], "-bpt") && i + 1 < argc) {
			char *a[1] = { argv[++i] };
			const char *e = scrCmdBpt(1, a, out);
			if (e) {
				printf("BPT: %s\n", e);
			}
		} else if (!strcmp(argv[i], "-fn") && i + 1 < argc) {
			i++;
		} else if (!strcmp(argv[i], "-go") && i + 1 < argc) {
			char tmp[256], line[64];
			snprintf(tmp, sizeof(tmp), "%s", argv[++i]);
			for (char *w = strtok(tmp, ","); w; w = strtok(NULL, ",")) {
				snprintf(line, sizeof(line), "XGO %s", w);
				scrHostCmd(line, nullCb, NULL);
			}
		} else {
			size_t l = strlen(args);
			snprintf(args + l, sizeof(args) - l, "%s%s", l ? " " : "", argv[i]);
		}
	}
	const char *fn = NULL;
	for (int i = 3; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "-fn")) {
			fn = argv[i + 1];
		}
	}
	scrResult_t r = scrRun(argv[2], args, fn, load);
	static const char *rn[] = { "END", "STOPPED", "ERROR", "NOFN" };
	printf("[result %s]\n", rn[r]);
	return 0;
}
