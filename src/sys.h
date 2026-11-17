#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cmd.h"

/*
 * System commands registered by sysInit():
 *   INFO       hardware and firmware information; also App (running image
 *              size of its OTA slot), Boot (count), Clock (or "not set"),
 *              Script (name running|paused, or none)
 *   WAIT ms    pause 0..60000 ms, 1 ms resolution (esp_timer); blocks
 *              the client's other commands
 *   REBOOT     reply OK, restart ~300 ms later
 *   PRINT text        reply text to the client (text rules: text.h textUnquote,
 *                     escapes \n \\ \# \$, \1..\9 dropped)
 *   DEBUG text        same text rules, to the log port(s) as dbgf()
 *   FAPP file text    same text rules, text + new line appended to file (full
 *                     name, as FCAT; folders and file created; read-only /
 *                     no card = error)
 *   FCAT file [start [count]] [>DEBUG]  show file (full name: name.txt|var|nvram,
 *                     /sys/name.txt|var), from line start, count lines, to the client or the log; CR dropped,
 *                     non-ASCII shown as '?', lines over 200 chars split
 *   FDIR [type]       list the files, default * (/scripts/data, then /sys/data as /sys/name;
 *                     nvram: /sys): name size ro|rw, then "n files"; no limit
 *   FSTAT file        size:ro|rw (full name, as FCAT)
 *   FDELETE file      delete a data file (full name; not a read-only one)
 */

/* Count this boot, register the commands above. Call after cmdInit() and nvInit(). */
esp_err_t sysInit(void);

/* Restart in SYS_REBOOT_MS (300 ms), so a reply can go out first. false =
 * no timer. */
bool sysRebootLater(void);

/* Current timestamp (n >= 16): "YYYYMMDD_HHMMSS" once a browser has set the
 * clock (POST /time), e.g. "20261113_142530"; else boot count (hex) and
 * uptime hhmmss, e.g. "b002A_001523". */
void sysTimestamp(char *out, size_t n);

/* Free file name in dir from the timestamp: "<stamp><ext>", or "<stamp>_1<ext>",
 * "_2" ... if taken. Caller holds sdLock() with the card mounted and dir existing.
 * Returns NULL, or an error text. */
const char *sysTimestampFile(const char *dir, const char *ext, char *out, size_t n);

/* FDIR / LIST: one folder's files matching ext (no limit), one line each:
 * "<prefix><name> size ro|rw" (ext dropped unless keepExt); adds the count
 * to *count. NULL, "no SD card" or "out of memory". */
const char *sysListDir(const char *dir, const char *ext, const char *prefix, bool keepExt,
                       cmdReply_t reply, int *count);

/* The closing "n file(s)" line. */
void sysListCount(int n, cmdReply_t reply);
