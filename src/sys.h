#pragma once

#include <stddef.h>
#include "esp_err.h"

/*
 * System commands registered by sysInit():
 *   INFO       hardware and firmware information
 *   WAIT ms    pause 0..60000 ms, 1 ms resolution (esp_timer); blocks
 *              the client's other commands
 *   REBOOT     reply OK, restart ~300 ms later
 *   PRINT text        reply text to the client (text rules: text.h textUnquote,
 *                     escapes \n \\ \# \$, \1..\9 dropped)
 *   DEBUG text        same text rules, to the log port(s) as dbgf()
 *   APPEND file text  same text rules, one line added to
 *                     SD:/scripts/data/file.txt (file: letters, digits, '_',
 *                     max 31; folders and file created; no card = error)
 *   LOGCAT            list the .txt files in SD:/scripts/data: "name size", then
 *                     "n files"
 *   LOGCAT file [>DEBUG]  show file.txt line by line to the client (or to
 *                     the log port(s) with >DEBUG); CR dropped, non-ASCII
 *                     shown as '?', lines over 200 chars split
 *
 * Boot count: NV sys.bootcnt (firmware-owned) is incremented by sysInit().
 * Timestamp: "bNNNN_hhmmss" = boot count (4 hex digits) + uptime, e.g.
 * b002A_001523 = boot 42, 0 h 15 min 23 s after boot. Used for automatic
 * file names (SCR SHOT, IMU REC *).
 */

/* Count this boot, register the commands above. Call after cmdInit() and nvInit(). */
esp_err_t sysInit(void);

/* Current timestamp, e.g. "b002A_001523" (n >= 16). */
void sysTimestamp(char *out, size_t n);

/* Free file name in dir from the timestamp: "<stamp><ext>", or "<stamp>-1<ext>",
 * "-2" ... if taken. Caller holds sdLock() with the card mounted and dir existing.
 * Returns NULL, or an error text. */
const char *sysTimestampFile(const char *dir, const char *ext, char *out, size_t n);
