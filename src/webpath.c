#include "webpath.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define WEBPATH_MAX	256	/* decoded path, without root */

static int webPathHex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool webPathMap(const char *uri, const char *root, char *out, size_t outMax)
{
	char   path[WEBPATH_MAX];
	size_t n = 0;

	if (uri[0] != '/') {
		return false;
	}

	/* decode up to '?' or '#' */
	for (const char *p = uri; *p && *p != '?' && *p != '#'; p++) {
		char c = *p;
		if (c == '%') {
			int hi = webPathHex(p[1]), lo = (hi >= 0) ? webPathHex(p[2]) : -1;
			if (hi < 0 || lo < 0) {
				return false;			/* malformed escape */
			}
			c  = (char)(hi * 16 + lo);
			p += 2;
			if (c == '\0') {
				return false;			/* %00 */
			}
		}
		if (c == '\\' || c == ':' || (unsigned char)c < 0x20) {
			return false;
		}
		if (n + 1 >= sizeof(path)) {
			return false;
		}
		path[n++] = c;
	}
	path[n] = '\0';

	/* Segments: FAT ignores trailing dots/spaces (".. " would act as ".."),
	 * so refuse any segment that ends in '.' or ' ' or starts with ' '.
	 * This also refuses ".", ".." and "...". */
	for (char *s = path; *s; ) {
		s++;						/* skip '/' */
		char  *e   = strchr(s, '/');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		if (len > 0 && (s[len - 1] == '.' || s[len - 1] == ' ' || s[0] == ' ')) {
			return false;
		}
		if (!e) {
			break;
		}
		s = e;
	}

	const char *index = (path[n - 1] == '/') ? "index.html" : "";
	int len = snprintf(out, outMax, "%s%s%s", root, path, index);
	return len > 0 && (size_t)len < outMax;
}

const char *webPathType(const char *path)
{
	static const struct {
		const char *ext;
		const char *type;
	} types[] = {
		{ ".html", "text/html" },
		{ ".htm",  "text/html" },
		{ ".css",  "text/css" },
		{ ".js",   "text/javascript" },
		{ ".json", "application/json" },
		{ ".txt",  "text/plain" },
		{ ".png",  "image/png" },
		{ ".jpg",  "image/jpeg" },
		{ ".jpeg", "image/jpeg" },
		{ ".gif",  "image/gif" },
		{ ".svg",  "image/svg+xml" },
		{ ".ico",  "image/x-icon" },
	};
	const char *dot   = strrchr(path, '.');
	const char *slash = strrchr(path, '/');
	if (dot && (!slash || dot > slash)) {
		for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
			if (strcasecmp(dot, types[i].ext) == 0) {
				return types[i].type;
			}
		}
	}
	return "application/octet-stream";
}
