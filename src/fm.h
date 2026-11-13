#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * SD card manager (web UI only): browse the card in a web page, download,
 * upload, delete, rename / move files and make folders.
 *
 *   FILES WEB        (web UI with a page connected) enter the mode: the page
 *                    is sent "<<GOTO /fm>>" and opens http://<wifi.ip>/fm:
 *                    the built-in page (assets/fm.html), or SD:/sys/fm.html
 *                    if present. Refused if a script is running or there is
 *                    no card. Already on: the page is just re-opened.
 *   FILES FMEXPORT   write the built-in page to SD:/sys/fm_builtin.html
 *                    (replaced; /sys created) - a start for your own fm.html.
 *   FILES SYNEXPORT  write the built-in viewer colours to
 *                    SD:/sys/syntax_builtin.cfg - a start for syntax.cfg -
 *                    with comments listing every highlighted word and all
 *                    registered commands (a word list for PC editors).
 *   FILES WEB STOP   leave it (FILES WEB EXIT is the same). These and FILES
 *                    WEB are the only commands accepted while it lasts
 *                    (any interface); every other command gets
 *                    "ERR SD manager active". Also left by the page's Exit
 *                    button, a BOOT short press, or REBOOT. No time-out.
 *
 * Endpoints (server task; all but /fm refuse with 409 when the mode is off):
 *   GET  /fm                      SD:/sys/fm.html, else the built-in page
 *                                 (mode off: redirect to /)
 *   GET  /fm/list?p=/dir          {"p":"/dir","d":[[name,mtime,attr]...],
 *                                  "f":[[name,size,mtime,attr]...]}
 *                                 attr bits: 1 read-only, 2 hidden, 4 system
 *                                 (FAT attributes, read through FatFs)
 *   GET  /fm/get?p=/dir/file      the file, as a download
 *   GET  /fm/view?p=/dir/file     the file to display, typed by extension:
 *                                 png ico jpg jpeg gif bmp webp svg, htm html,
 *                                 text: txt csv s ion var cfg c h sh css
 *                                 nvram (as text; "!key=value" lines sent
 *                                 as "!key=[redacted]", like FCAT)
 *                                 (else 400 "not a viewable type")
 *   GET  /fm/syntax[?builtin=1]   viewer colours: SD:/sys/syntax.cfg if
 *                                 present, else the built-in (assets/
 *                                 syntax.cfg); header X-Syntax-Source:
 *                                 card | builtin. The page checks it.
 *   POST /fm/del?p=/path          delete a file (not read-only) or an empty folder
 *   POST /fm/ren?p=/old&to=/new   rename / move (target must not exist)
 *   POST /fm/mkdir?p=/dir         new folder
 *   POST /fm/exit                 leave the mode
 *   POST /fm/put?p=/dir/name[&over=1]  upload: body = the file, written to
 *                                 name.upl and renamed when complete;
 *                                 409 "exists" unless over=1 (not read-only);
 *                                 failure or mode off: name.upl deleted
 * Replies: 200 "OK" (or the data), else 400 / 409 with the reason as text.
 * Paths start with '/', are relative to the card, URL-encoded; "." / ".."
 * segments, '\', ':', control characters and segments starting with a space
 * or ending in '.' or ' ' are refused (see fmPathCheck).
 */

/* Register the FILES WEB gate. Call after cmdInit(). */
esp_err_t fmInit(void);

/* Mode on. */
bool fmActive(void);

/* FILES WEB: enter the mode (NULL) or the reason it was refused. */
const char *fmStart(void);

/* FILES WEB STOP / Exit / BOOT: leave the mode (no-op if off). Any task. */
void fmStop(void);

/* FILES FMEXPORT: built-in page -> SD:/sys/fm_builtin.html. NULL or the
 * error; *bytes = the page size. */
const char *fmExport(size_t *bytes);

/* FILES SYNEXPORT: built-in viewer colours -> SD:/sys/syntax_builtin.cfg. */
const char *fmSynExport(size_t *bytes);

/* Register the /fm handlers (web.c, before its catch-all). */
void fmRegister(httpd_handle_t hd);

/* Number of URI handlers fmRegister() adds. */
#define FM_URI_COUNT	10

/* Card-relative path p (decoded, e.g. "/a/b.txt" or "/") checked and joined
 * to root into out. false = refused or too long. Pure, host-testable. */
bool fmPathCheck(const char *p, const char *root, char *out, size_t outMax);

/* URL-decode in place (%XX, '+' = space). false = malformed escape. */
bool fmUrlDecode(char *s);
