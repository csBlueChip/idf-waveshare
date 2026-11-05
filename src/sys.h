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
 *   FLOG file text    same text rules, one line added to
 *                     SD:/scripts/data/file.txt (file: letters, digits, '_',
 *                     max 31; folders and file created; no card = error)
 *   FCAT file [start,count] [>DEBUG]  show file.txt (or count lines from
 *                     line start) to the client or the log; CR dropped,
 *                     non-ASCII shown as '?', lines over 200 chars split
 *   FDIR              list the .txt files: name size ro|rw, then "n files"
 *   FSTAT file        size:ro|rw
 *   FDELETE file      delete a data file (not a read-only one)
 */

/* Count this boot, register the commands above. Call after cmdInit() and nvInit(). */
esp_err_t sysInit(void);

/* Current timestamp, e.g. "b002A_001523" (n >= 16). */
void sysTimestamp(char *out, size_t n);

/* Free file name in dir from the timestamp: "<stamp><ext>", or "<stamp>-1<ext>",
 * "-2" ... if taken. Caller holds sdLock() with the card mounted and dir existing.
 * Returns NULL, or an error text. */
const char *sysTimestampFile(const char *dir, const char *ext, char *out, size_t n);
