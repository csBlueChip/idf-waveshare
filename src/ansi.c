#include "ansi.h"
#include <stdio.h>
#include <string.h>

/* SGR for an ink / paper value (0..7, 90..97, 8 = default) */
static void sgrInk(int c, char *o)
{
	snprintf(o, ANSI_SEQ_MAX, "\x1b[%dm", c == ANSI_NONE ? 39 : c < 8 ? 30 + c : c);
}

static void sgrPaper(int c, char *o)
{
	snprintf(o, ANSI_SEQ_MAX, "\x1b[%dm", c == ANSI_NONE ? 49 : c < 8 ? 40 + c : c + 10);
}

/* one colour character: 0..8 or k r g y b m c w d -> 0..8; -1 = none */
static int colourOf(char ch)
{
	static const char letters[] = "krgybmcwd";
	const char *l = (ch != '\0') ? strchr(letters, ch) : NULL;
	if (ch >= '0' && ch <= '8') {
		return ch - '0';
	}
	return l ? (int)(l - letters) : -1;
}

/* [9|l] colour at s: value (0..8, 90..97) and *used chars; -1 = bad */
static int colourAt(const char *s, int *used)
{
	if (s[0] == '9' || s[0] == 'l') {		/* light (bright) */
		int c = colourOf(s[1]);
		*used = 2;
		return (c >= 0 && c <= 7) ? 90 + c : -1;
	}
	*used = 1;
	return colourOf(s[0]);
}

int ansiEscape(const char *p, char *out)
{
	bool paper = (p[0] == 'p');
	if (p[0] != 'i' && !paper) {
		return 0;
	}
	if (!paper && p[1] == 'p') {			/* \ip: both back */
		sgrInk(ANSI_NONE, out);
		sgrPaper(ANSI_NONE, out + strlen(out));
		return 2;
	}
	int used, c = colourAt(p + 1, &used);
	if (c < 0) {
		return -1;
	}
	(paper ? sgrPaper : sgrInk)(c, out);
	return 1 + used;
}

bool ansiParseColour(const char *w, int *c)
{
	int used, v = colourAt(w, &used);
	if (v < 0 || w[used] != '\0') {
		return false;
	}
	*c = v;
	return true;
}

typedef struct {
	char  *out;
	size_t n, len;
} sink_t;

static void put(sink_t *k, const char *s, size_t l)
{
	for (size_t i = 0; i < l; i++, k->len++) {
		if (k->n && k->len + 1 < k->n) {
			k->out[k->len] = s[i];
		}
	}
}

static void puts0(sink_t *k, const char *s)
{
	put(k, s, strlen(s));
}

size_t ansiFrame(const char *in, int ink, int paper, char *out, size_t n)
{
	sink_t k = { out, n, 0 };
	char   ci[ANSI_SEQ_MAX], cp[ANSI_SEQ_MAX];
	bool   set = (ink != ANSI_NONE || paper != ANSI_NONE);

	sgrInk(ink, ci);
	sgrPaper(paper, cp);
	for (const char *p = in; *p;) {
		size_t l   = strcspn(p, "\n");
		bool   col = set || memchr(p, ANSI_ESC, l) != NULL;
		if (col) {
			if (ink != ANSI_NONE) {
				puts0(&k, ci);
			}
			if (paper != ANSI_NONE) {
				puts0(&k, cp);
			}
		}
		for (size_t i = 0; i < l; i++) {
			if (p[i] == ANSI_ESC && l - i >= 5 && strncmp(p + i, "\x1b[39m", 5) == 0) {
				puts0(&k, ci);			/* \i8: the script's INK */
				i += 4;
			} else if (p[i] == ANSI_ESC && l - i >= 5 && strncmp(p + i, "\x1b[49m", 5) == 0) {
				puts0(&k, cp);			/* \p8: the script's PAPER */
				i += 4;
			} else {
				put(&k, p + i, 1);
			}
		}
		if (col) {
			puts0(&k, ci);				/* escapes end with the line */
			puts0(&k, cp);
			if (paper != ANSI_NONE) {
				puts0(&k, "\x1b[K");		/* rest of the line in the paper */
			}
			puts0(&k, "\x1b[0m");
		}
		p += l;
		if (*p == '\n') {
			put(&k, "\n", 1);
			p++;
		}
	}
	if (n) {
		out[k.len < n ? k.len : n - 1] = '\0';
	}
	return k.len;
}

size_t ansiStrip(char *s)
{
	char *o = s;
	for (const char *p = s; *p;) {
		if (p[0] == ANSI_ESC && p[1] == '[') {
			const char *q = p + 2;
			while (*q && !((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z'))) {
				q++;
			}
			p = *q ? q + 1 : q;
		} else {
			*o++ = *p++;
		}
	}
	*o = '\0';
	return (size_t)(o - s);
}

bool ansiHas(const char *s)
{
	return strchr(s, ANSI_ESC) != NULL;
}
