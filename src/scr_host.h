#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Hooks the interpreter core needs from its host: script.c in the firmware,
 * tests/scrtest/host.c on the PC. All are called from the script task.
 */

/* name.s from the script folder (name without .s) into a new buffer
 * (scrHostFree), NUL added; *len = file size. NULL + *err if it can't. */
char *scrHostRead(const char *name, size_t *len, const char **err);

void *scrHostAlloc(size_t n);			/* zeroed; PSRAM in the firmware */
void *scrHostRealloc(void *p, size_t n);
void  scrHostFree(void *p);

/* Run a console command; every reply piece goes to cb (ends with "OK\n" or
 * "ERR reason\n"). */
void scrHostCmd(const char *line, void (*cb)(void *ctx, const char *text), void *ctx);

/* name is a console command (for .CONST name checks). */
bool scrHostIsCommand(const char *name);

void scrHostPrint(const char *text);		/* to the UI client (text has its "\n") */
void scrHostDebug(const char *text);		/* one log line (no "\n") */
void scrHostNotify(const char *text);		/* "<<SCRIPT ...>>\n" to the client */

/* NV value of key ("scr.bl"), false if no such key. */
bool scrHostNv(const char *key, char *out, size_t n);

/* Built-in $.name handled by the host (ax ay az gx gy gz temp bat steps orient
 * time uptime w h rot fw): its text, false if unknown / failed (*err set). */
bool scrHostBuiltin(const char *name, char *out, size_t n, const char **err);

/* One IMU snapshot (g or dps). NULL or an error text. */
const char *scrHostImu(bool gyro, double v[3]);

uint32_t scrHostTrng(void);
uint32_t scrPrng(uint32_t *state);		/* RNG PRNG: user generator */
int64_t  scrHostMs(void);			/* ms since boot */

/* Between statements: watchdog / yield; returns SCR_CHK_* requests. */
#define SCR_CHK_STOP	0x01
#define SCR_CHK_PAUSE	0x02
int scrHostCheck(void);

/* Pause the script for ms (1 ms resolution). false = stopped. */
bool scrHostSleep(int ms);

/* Take the oldest queued event whose source is in mask (text without << >>).
 * ms: 0 = don't wait, < 0 = forever. 1 = got one, 0 = time-out, -1 = stopped. */
int scrHostEvent(int mask, char *out, size_t n, int ms);
int scrHostEventsDropped(void);

/* TIMER n ms (0 = off); all timers stop with scrHostTimer(0, 0). */
void scrHostTimer(int n, int ms);

/* Next console line for INPUT. 1 = line, 0 = time-out, -1 = no client
 * (host queues UI DISCONNECT), -2 = stopped. */
int scrHostInput(char *out, size_t n, int ms);

/* Paused (<<SCRIPT ... PAUSED>> already sent): block until CONT / STEP /
 * STOP. Returns SCR_GO_*. */
int scrHostPause(void);

/* Data files with a position per run when the script has no .FILES (NV
 * run.files), 1..SCR_FILES_MAX. */
int scrHostFilesDefault(void);

/* Data files (FREAD FWRITE ... and VARS SAVE / LOAD): fname is "name.txt" or
 * "name.var" in the script data folder (SD:/scripts/data). Offsets from 0.
 * Size in bytes, -1 = no such file (*err NULL) or an error (*err set);
 * *ro = read-only. */
long scrHostFileSize(const char *fname, bool *ro, const char **err);
/* Read up to n bytes at off: count read, -1 = error (*err). */
int scrHostFileRead(const char *fname, long off, char *buf, int n, const char **err);
/* Write n bytes at off (file created; trunc: emptied first). NULL or an error. */
const char *scrHostFileWrite(const char *fname, long off, const char *data, int n, bool trunc);
