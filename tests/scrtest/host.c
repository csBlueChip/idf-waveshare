/* PC test host for the script interpreter core (src/scr_*.c).
 *
 *   build: make (or run.sh)     use: scrtest SD name [args] [options] (README.txt)
 *
 * SD is the card's root folder (e.g. ../../sd): name.s comes from SD/scripts
 * (sys/name from SD/sys), data files are real files in SD/scripts/data.
 * -test: the old layout for the test suite - SD is the folder holding the
 * scripts, data files live in memory (nothing is written to disk).
 *
 * Options:
 *   -test              test-suite layout (see above)
 *   -i                 interactive: INPUT reads the keyboard (real time-outs),
 *                      PRINT is plain, WAIT and time-outs take real time
 *   -load / -bpt file:n / -fn name / -go STEP,IN,OUT,CONT,STOP   debugger
 *   -file name=path    (-test) load a disk file as data file name
 *   -input a,b,c       INPUT answers in order (then INPUT times out)
 *   -seed n            random generator start (default 1)
 *   -dump name         print data file name after the run
 *
 * Fake commands (everything else: prints "CMD: line", replies OK):
 *   FAIL reason        ERR reason
 *   REPLY a b ...      replies one line per word, then OK
 *   EVT text           queues event text (e.g. EVT BTN 1 SHORT)
 *   XINPUT text        queues a console line for INPUT
 *   XGO mode           queues a debugger answer (CONT STEP IN OUT STOP)
 *   XSTOP / XPAUSE     request STOP / PAUSE (seen between statements)
 *   XCLIENT 0|1        client connected (INPUT with none: -1)
 *   FLOG f text        appends "text\n" to data file f.txt
 *   FSTAT f / FDELETE f / FCAT f   on the data files (f.var allowed)
 *   XRO f              make f.txt read-only
 * Time is virtual (not with -i): WAIT / time-outs advance it; timers tick on it.
 * On a pause it prints STACK and VARS, then takes the next XGO (default CONT).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/select.h>
#include "scr.h"
#include "scr_host.h"

static const char *s_dir;
static bool        s_card = true;		/* SD layout, data files on disk (not -test) */
static bool        s_live;			/* -i: keyboard INPUT, real time */
static char        s_data[512];		/* SD/scripts/data */
static int64_t     s_t0;			/* -i: real ms at start */

/* -i: real milliseconds since the start (+1000, like the virtual clock) */
static int64_t realMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - s_t0 + 1000;
}

/* card layout: path of data file fname */
static void dataPath(const char *fname, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", s_data, fname);
}
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

/* in-memory data files */
#define NF 16
static struct { char name[48]; char *d; long n; bool ro; } s_f[NF];

static int fileFind(const char *fname, bool create)
{
	for (int i = 0; i < NF; i++) {
		if (s_f[i].name[0] && !strcasecmp(s_f[i].name, fname)) {
			return i;
		}
	}
	if (!create) {
		return -1;
	}
	for (int i = 0; i < NF; i++) {
		if (!s_f[i].name[0]) {
			snprintf(s_f[i].name, sizeof(s_f[i].name), "%s", fname);
			s_f[i].d = NULL;
			s_f[i].n = 0;
			s_f[i].ro = false;
			return i;
		}
	}
	return -1;
}

long scrHostFileSize(const char *fname, bool *ro, const char **err)
{
	if (s_card) {
		char        path[600];
		struct stat st;
		dataPath(fname, path, sizeof(path));
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			return -1;
		}
		*ro = access(path, W_OK) != 0;
		return (long)st.st_size;
	}
	int i = fileFind(fname, false);
	if (i < 0) {
		return -1;
	}
	*ro = s_f[i].ro;
	return s_f[i].n;
}

int scrHostFileRead(const char *fname, long off, char *buf, int n, const char **err)
{
	if (s_card) {
		char path[600];
		dataPath(fname, path, sizeof(path));
		FILE *f = fopen(path, "rb");
		if (!f) {
			*err = "no such file";
			return -1;
		}
		int got = fseek(f, off, SEEK_SET) == 0 ? (int)fread(buf, 1, n, f) : 0;
		fclose(f);
		return got;
	}
	int i = fileFind(fname, false);
	if (i < 0) {
		*err = "no such file";
		return -1;
	}
	long left = s_f[i].n - off;
	int  c    = left < 0 ? 0 : left < n ? (int)left : n;
	memcpy(buf, s_f[i].d + off, c);
	return c;
}

const char *scrHostFileWrite(const char *fname, long off, const char *data, int n, bool trunc)
{
	if (s_card) {
		char path[600];
		dataPath(fname, path, sizeof(path));
		if (access(path, F_OK) == 0 && access(path, W_OK) != 0) {
			return "read-only";
		}
		mkdir(s_data, 0775);
		FILE *f = fopen(path, trunc ? "wb" : "r+b");
		if (!f && !trunc) {
			f = fopen(path, "w+b");			/* created if missing */
		}
		if (!f) {
			return "write failed";
		}
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		for (; size < off; size++) {
			fputc(' ', f);				/* gap filled with spaces, as in memory */
		}
		bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(data, 1, n, f) == (size_t)n;
		ok = fclose(f) == 0 && ok;
		return ok ? NULL : "write failed";
	}
	int i = fileFind(fname, true);
	if (i < 0) {
		return "too many files";
	}
	if (s_f[i].ro) {
		return "read-only";
	}
	if (trunc) {
		s_f[i].n = 0;
	}
	long end = off + n;
	if (end > s_f[i].n) {
		s_f[i].d = realloc(s_f[i].d, end);
		memset(s_f[i].d + s_f[i].n, ' ', off > s_f[i].n ? off - s_f[i].n : 0);
		s_f[i].n = end;
	}
	memcpy(s_f[i].d + off, data, n);
	return NULL;
}

char *scrHostRead(const char *name, size_t *len, const char **err)
{
	char path[512];
	if (s_card) {				/* as on the card: sys/x -> SD/sys/x.s, x -> SD/scripts/x.s */
		snprintf(path, sizeof(path), strchr(name, '/') ? "%s/%s.s" : "%s/scripts/%s.s", s_dir, name);
	} else {
		snprintf(path, sizeof(path), "%s/%s.s", s_dir, name);
	}
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

/* FLOG / FSTAT / FDELETE / FCAT / XRO on the real data files (card layout) */
static void cardCmd(const char *cmd, const char *fn, const char *text,
                    void (*cb)(void *ctx, const char *text), void *ctx)
{
	char        path[600], buf[700];
	struct stat st;
	dataPath(fn, path, sizeof(path));
	bool exists = stat(path, &st) == 0 && S_ISREG(st.st_mode);
	if (!strcasecmp(cmd, "FLOG")) {
		char t[600];
		snprintf(t, sizeof(t), "%s\n", text);
		const char *e = scrHostFileWrite(fn, exists ? (long)st.st_size : 0, t, strlen(t), false);
		if (e) {
			snprintf(buf, sizeof(buf), "ERR %s\n", e);
			cb(ctx, buf);
			return;
		}
	} else if (!exists) {
		cb(ctx, "ERR no such file\n");
		return;
	} else if (!strcasecmp(cmd, "FSTAT")) {
		snprintf(buf, sizeof(buf), "%ld:%s\n", (long)st.st_size, access(path, W_OK) ? "ro" : "rw");
		cb(ctx, buf);
	} else if (!strcasecmp(cmd, "FDELETE")) {
		if (access(path, W_OK) != 0) {
			cb(ctx, "ERR read-only\n");
			return;
		}
		unlink(path);
	} else if (!strcasecmp(cmd, "XRO")) {
		chmod(path, 0444);
	} else {
		FILE *f = fopen(path, "rb");
		printf("FCAT %s:\n", fn);
		int c;
		while (f && (c = fgetc(f)) != EOF) {
			putchar(c);
		}
		if (f) {
			fclose(f);
		}
	}
	cb(ctx, "OK\n");
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
	} else if (!strncasecmp(line, "FLOG ", 5) || !strncasecmp(line, "FSTAT ", 6) || !strncasecmp(line, "FDELETE ", 8) ||
	           !strncasecmp(line, "FCAT ", 5) || !strncasecmp(line, "XRO ", 4)) {
		char cmd[16], nm[40], fn[48];
		int  used = 0;
		sscanf(line, "%15s %39s %n", cmd, nm, &used);
		snprintf(fn, sizeof(fn), strchr(nm, '.') && strcasecmp(cmd, "FLOG") ? "%s" : "%s.txt", nm);
		if (s_card) {
			cardCmd(cmd, fn, used ? line + used : "", cb, ctx);
			return;
		}
		int i = fileFind(fn, !strcasecmp(cmd, "FLOG"));
		if (i < 0) {
			cb(ctx, "ERR no such file\n");
			return;
		}
		if (!strcasecmp(cmd, "FLOG")) {
			char t[600];
			snprintf(t, sizeof(t), "%s\n", used ? line + used : "");
			const char *e = scrHostFileWrite(fn, s_f[i].n, t, strlen(t), false);
			if (e) {
				snprintf(buf, sizeof(buf), "ERR %s\n", e);
				cb(ctx, buf);
				return;
			}
		} else if (!strcasecmp(cmd, "FSTAT")) {
			snprintf(buf, sizeof(buf), "%ld:%s\n", s_f[i].n, s_f[i].ro ? "ro" : "rw");
			cb(ctx, buf);
		} else if (!strcasecmp(cmd, "FDELETE")) {
			if (s_f[i].ro) {
				cb(ctx, "ERR read-only\n");
				return;
			}
			free(s_f[i].d);
			s_f[i].name[0] = '\0';
		} else if (!strcasecmp(cmd, "XRO")) {
			s_f[i].ro = true;
		} else {
			printf("FCAT %s:\n%.*s", fn, (int)s_f[i].n, s_f[i].d ? s_f[i].d : "");
		}
	} else {
		printf("CMD: %s\n", line);
	}
	cb(ctx, "OK\n");
}

bool scrHostIsCommand(const char *name)
{
	static const char *c[] = { "ECHO", "CLS", "LED", "UI", "NV", "IMU", "BTN", "FAIL", "REPLY", "EVT",
	                           "ACC", "GYR", "FBOX", "LINE", "FLOG", "FCAT", "FDIR", "FSTAT", "FDELETE", NULL };
	for (int i = 0; c[i]; i++) {
		if (!strcasecmp(c[i], name)) {
			return true;
		}
	}
	return false;
}

void scrHostPrint(const char *text)
{
	size_t n = strlen(text);
	if (s_live) {
		printf("%s", text);			/* as sent: an INPUT prompt has no line end */
	} else {
		printf("> %s%s", text, n && text[n - 1] == '\n' ? "" : "\n");	/* one line each */
	}
	fflush(stdout);
}
void scrHostDebug(const char *text)		{ printf("DEBUG: %s\n", text); }
void scrHostNotify(const char *text)		{ printf("%s", text); }

bool scrHostNv(const char *key, char *out, size_t n)
{
	if (!strcasecmp(key, "scr.bl")) {
		snprintf(out, n, "50");
		return true;
	}
	if (!strcasecmp(key, "wifi.ssid")) {
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

int64_t scrHostMs(void)				{ return s_live ? (s_ms = realMs()) : s_ms; }
int     scrHostFilesDefault(void)		{ return 4; }	/* NV run.files */

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

/* -i: wait ms of real time (or until an event of mask, if mask) */
static void liveWait(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
	s_ms = realMs();
	timersTo(s_ms);
}

bool scrHostSleep(int ms)
{
	if (s_live) {
		int64_t end = realMs() + ms;
		while (realMs() < end) {
			int64_t left = end - realMs();
			liveWait(left > 20 ? 20 : (int)left);
		}
		return true;
	}
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
	if (s_live) {					/* real time: poll */
		int64_t end = ms < 0 ? -1 : realMs() + ms;
		for (;;) {
			liveWait(20);
			if (take(mask, out, n)) {
				return 1;
			}
			if (end >= 0 && realMs() >= end) {
				return 0;
			}
			if (end < 0 && !s_tim[1].ms && !s_tim[2].ms && !s_tim[3].ms && !s_tim[4].ms) {
				printf("[host: WAIT EVENT forever with nothing to come - stopping]\n");
				return -1;
			}
		}
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
	if (s_live) {					/* the keyboard, with a real time-out */
		fflush(stdout);
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(0, &rd);
		struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
		int r = select(1, &rd, NULL, NULL, ms < 0 ? NULL : &tv);
		s_ms = realMs();
		if (r == 0) {
			return 0;				/* time-out */
		}
		if (r < 0 || !fgets(out, (int)n, stdin)) {
			s_client = false;			/* Ctrl+D: as no client */
			post("UI DISCONNECT");
			return -1;
		}
		out[strcspn(out, "\r\n")] = '\0';
		return 1;
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

/* -file name=path: the disk file becomes data file name (whole file in memory). */
static bool loadFile(const char *spec)
{
	const char *eq = strchr(spec, '=');
	char        name[48];
	if (!eq || eq == spec || (size_t)(eq - spec) >= sizeof(name)) {
		fprintf(stderr, "-file name=path: bad '%s'\n", spec);
		return false;
	}
	snprintf(name, sizeof(name), "%.*s", (int)(eq - spec), spec);
	FILE *f = fopen(eq + 1, "rb");
	if (!f) {
		fprintf(stderr, "-file: cannot read %s\n", eq + 1);
		return false;
	}
	static char buf[64 * 1024];
	size_t      n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (fileFind(name, true) < 0 || scrHostFileWrite(name, 0, buf, (int)n, true)) {
		fprintf(stderr, "-file: cannot store %s\n", name);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "use: scrtest SD name [args] [-i] [-test] [-load] [-bpt file:n] [-fn name]\n"
		                "       [-go STEP,IN,...] [-file name=path] [-input a,b,...] [-seed n] [-dump name]\n"
		                "  SD = the card's root folder (scripts/, sys/, scripts/data/), e.g. ../../sd\n");
		return 2;
	}
	s_dir = argv[1];
	for (int i = 3; i < argc; i++) {			/* layout first: -file needs it */
		if (!strcmp(argv[i], "-test")) {
			s_card = false;
		} else if (!strcmp(argv[i], "-i")) {
			s_live = true;
		}
	}
	snprintf(s_data, sizeof(s_data), "%s/scripts/data", s_dir);
	if (s_card) {
		char        sc[600];
		struct stat st;
		snprintf(sc, sizeof(sc), "%s/scripts", s_dir);
		if (stat(sc, &st) != 0 || !S_ISDIR(st.st_mode)) {
			fprintf(stderr, "%s: no scripts folder - give the card's root (e.g. ../../sd), or -test\n", s_dir);
			return 2;
		}
	}
	if (s_live) {
		s_t0 = 0;
		s_t0 = realMs() - 1000;
		s_ms = 1000;
	}
	char        args[256] = "";
	bool        load = false;
	const char *dump[8];
	int         nDump = 0;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "-test") || !strcmp(argv[i], "-i")) {
			/* done above */
		} else if (!strcmp(argv[i], "-load")) {
			load = true;
		} else if (!strcmp(argv[i], "-bpt") && i + 1 < argc) {
			char *a[1] = { argv[++i] };
			const char *e = scrCmdBpt(1, a, out);
			if (e) {
				printf("BPT: %s\n", e);
			}
		} else if (!strcmp(argv[i], "-fn") && i + 1 < argc) {
			i++;
		} else if (!strcmp(argv[i], "-file") && i + 1 < argc) {
			if (s_card) {
				fprintf(stderr, "-file is for -test (with SD the data files are real files)\n");
				return 2;
			}
			if (!loadFile(argv[++i])) {
				return 2;
			}
		} else if (!strcmp(argv[i], "-input") && i + 1 < argc) {
			char tmp[1024];
			snprintf(tmp, sizeof(tmp), "%s", argv[++i]);
			for (char *w = strtok(tmp, ","); w && s_nin < Q; w = strtok(NULL, ",")) {
				snprintf(s_in[s_nin++], sizeof(s_in[0]), "%s", w);
			}
		} else if (!strcmp(argv[i], "-seed") && i + 1 < argc) {
			s_rng = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-dump") && i + 1 < argc) {
			i++;
			if (nDump < 8) {
				dump[nDump++] = argv[i];
			}
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
	for (int i = 0; i < nDump; i++) {
		if (s_card) {
			char path[600];
			dataPath(dump[i], path, sizeof(path));
			FILE *f = fopen(path, "rb");
			if (!f) {
				printf("[%s: no such file]\n", dump[i]);
				continue;
			}
			printf("[%s]\n", dump[i]);
			int c, last = '\n';
			while ((c = fgetc(f)) != EOF) {
				putchar(last = c);
			}
			fclose(f);
			printf("%s[end]\n", last == '\n' ? "" : "\n");
			continue;
		}
		int k = fileFind(dump[i], false);
		if (k < 0) {
			printf("[%s: no such file]\n", dump[i]);
		} else {
			printf("[%s]\n%.*s%s[end]\n", dump[i], (int)s_f[k].n, s_f[k].d ? s_f[k].d : "",
			       s_f[k].n && s_f[k].d[s_f[k].n - 1] != '\n' ? "\n" : "");
		}
	}
	return 0;
}
