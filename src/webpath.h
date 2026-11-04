#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Map a request URI to a file path under a root folder, safely.
 *   - query (?...) and fragment (#...) are dropped
 *   - %XX escapes are decoded (malformed escapes are refused)
 *   - after decoding, refused: '\', ':', control chars (incl. NUL), and any
 *     segment ending in '.' or ' ' or starting with ' ' (FAT ignores trailing
 *     dots/spaces, so ".. " would act as ".."; also covers "." and "..")
 *   - "/" (or a path ending in '/') maps to index.html in that folder
 * Pure function (no I/O), host-testable.
 */

/* uri e.g. "/img/a%20b.png?x=1", root e.g. "/sdcard/www".
 * Writes "/sdcard/www/img/a b.png" to out. false = refused or too long. */
bool webPathMap(const char *uri, const char *root, char *out, size_t outMax);

/* Content type from the file name's extension (binary if unknown). */
const char *webPathType(const char *path);
