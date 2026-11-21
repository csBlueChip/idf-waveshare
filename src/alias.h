#pragma once

/*
 * UI aliases (SD:/sys/alias.cfg): the first word of a console line
 * typed on any UI is looked up and replaced. Scripts never see aliases.
 *
 * alias.cfg: one "name=replacement" per line, '#' comment lines, blank lines.
 *   name         1..ALIAS_NAME_MAX letters, digits, '_' (case-insensitive);
 *                not a built-in command
 *   replacement  1..ALIAS_TEXT_MAX chars; $1..$9 = that argument, $* = all
 *                arguments; without any $n / $* the arguments are appended
 *   e.g.  fm=FILES WEB
 *         nvg=NV GET $1
 *
 * Commands registered by aliasInit():
 *   ALIAS                       list (name=replacement per line)
 *   ALIAS ADD name text...      add / replace (quotes optional on both:
 *                               ALIAS ADD nvg "NV GET $1"); written to the file
 *   ALIAS DEL name              remove; written to the file
 *   ALIAS LOAD                  read the file again; bad lines: warnings
 */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define ALIAS_MAX	32
#define ALIAS_NAME_MAX	15
#define ALIAS_TEXT_MAX	63

/* Register ALIAS and load SD:/sys/alias.cfg (skipped lines: log warnings).
 * Call after every other command is registered. */
esp_err_t aliasInit(void);

/* Expand a console line. Returns line itself (no alias), out (expanded, NUL
 * terminated, at most n - 1 chars) or NULL (the expansion does not fit). */
const char *aliasExpand(const char *line, char *out, size_t n);
