/* Script values, variables, expressions and $variable substitution. */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "scr_int.h"

static int         s_skip;		/* > 0: parse only (lazy AND / OR) */
static const char *s_stop;		/* extra words that end an expression */
static uint32_t    s_prngState = 0x12345678;

/* ================= basics ================= */

void scrFail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(scr.msg, sizeof(scr.msg), fmt, ap);
	va_end(ap);
	longjmp(scr.jb, 1);
}

char *scrArena(size_t n)
{
	n = (n + 3) & ~(size_t)3;
	if (scr.aUsed + n > sizeof(scr.arena)) {
		scrFail("statement too complex (out of scratch memory)");
	}
	char *p = scr.arena + scr.aUsed;
	scr.aUsed += n;
	return p;
}

char *scrArenaStr(const char *s, int len)
{
	char *p = scrArena(len + 1);
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

bool scrIsName(char c, bool first)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (!first && c >= '0' && c <= '9');
}

const char *scrSkip(const char *p)
{
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return p;
}

bool scrWordIs(const char *p, const char *w)
{
	size_t l = strlen(w);
	return strncasecmp(p, w, l) == 0 && (p[l] == '\0' || p[l] == ' ' || p[l] == '\t');
}

int scrStrcasecmp(const char *a, const char *b)
{
	return strcasecmp(a, b);
}

/* ================= numbers / text ================= */

/* [sign] digits [.digits] [e[sign]digits], or [sign] 0x.. / 0b.. */
bool scrParseNum(const char *s, int len, double *out)
{
	int  i = 0;
	bool neg = false;
	if (len <= 0) {
		return false;
	}
	if (s[0] == '+' || s[0] == '-') {
		neg = (s[0] == '-');
		i++;
	}
	if (i + 2 < len + 1 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B')) {
		int    base = (s[i + 1] == 'x' || s[i + 1] == 'X') ? 16 : 2;
		double v    = 0;
		int    j    = i + 2;
		if (j >= len) {
			return false;
		}
		for (; j < len; j++) {
			int d;
			char c = s[j];
			if (c >= '0' && c <= '9') {
				d = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				d = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				d = c - 'A' + 10;
			} else {
				return false;
			}
			if (d >= base) {
				return false;
			}
			v = v * base + d;
		}
		*out = neg ? -v : v;
		return true;
	}
	int digits = 0;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		i++;
		digits++;
	}
	if (!digits) {
		return false;
	}
	if (i < len && s[i] == '.') {
		i++;
		int fd = 0;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			i++;
			fd++;
		}
		if (!fd) {
			return false;
		}
	}
	if (i < len && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (i < len && (s[i] == '+' || s[i] == '-')) {
			i++;
		}
		int ed = 0;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			i++;
			ed++;
		}
		if (!ed) {
			return false;
		}
	}
	if (i != len) {
		return false;
	}
	char buf[64];
	if (len >= (int)sizeof(buf)) {
		return false;
	}
	memcpy(buf, s, len);
	buf[len] = '\0';
	*out = strtod(buf, NULL);
	return true;
}

void scrFmtNumBuf(double v, char *out, size_t n)
{
	if (isnan(v)) {
		snprintf(out, n, "nan");
		return;
	}
	if (isinf(v)) {
		snprintf(out, n, v < 0 ? "-inf" : "inf");
		return;
	}
	if (fabs(v) >= 1e15) {
		snprintf(out, n, "%.15g", v);
		return;
	}
	snprintf(out, n, "%.3f", v);
	char *dot = strchr(out, '.');
	if (dot) {
		char *e = out + strlen(out) - 1;
		while (e > dot && *e == '0') {
			*e-- = '\0';
		}
		if (e == dot) {
			*e = '\0';
		}
	}
	if (strcmp(out, "-0") == 0) {
		snprintf(out, n, "0");
	}
}

const char *scrFmtNum(double v)
{
	char buf[64];
	scrFmtNumBuf(v, buf, sizeof(buf));
	return scrArenaStr(buf, strlen(buf));
}

scrVal_t scrNum(double n)
{
	scrVal_t v = { .num = true, .n = n, .s = "", .len = 0 };
	return v;
}

scrVal_t scrText(const char *s, int len)
{
	scrVal_t v = { .num = false, .n = 0, .s = s, .len = len };
	return v;
}

bool scrToNum(scrVal_t v, double *out)
{
	if (v.num) {
		*out = v.n;
		return true;
	}
	return scrParseNum(v.s, v.len, out);
}

const char *scrToText(scrVal_t v, int *len)
{
	if (v.num) {
		const char *t = scrFmtNum(v.n);
		if (len) {
			*len = strlen(t);
		}
		return t;
	}
	if (len) {
		*len = v.len;
	}
	return v.s;
}

bool scrTruth(scrVal_t v)
{
	double d;
	if (v.num) {
		return v.n != 0;
	}
	if (v.len == 0) {
		return false;
	}
	if (scrParseNum(v.s, v.len, &d)) {
		return d != 0;
	}
	return true;
}

static double needNum(scrVal_t v)
{
	double d;
	if (!scrToNum(v, &d)) {
		int         l;
		const char *t = scrToText(v, &l);
		scrFail("number expected, got \"%.*s\"", l > 20 ? 20 : l, t);
	}
	return d;
}

static int64_t needWhole(scrVal_t v)
{
	double d = needNum(v);
	if (d != floor(d)) {
		scrFail("bitwise operators need whole numbers");
	}
	return (int64_t)d;
}

/* ================= words ================= */

static bool isSep(char c)
{
	return c == ' ' || c == '\t' || c == ':' || c == '|' || c == '\n' || c == '\r';
}

int scrWords(const char *s, int len)
{
	int n = 0;
	for (int i = 0; i < len;) {
		while (i < len && isSep(s[i])) {
			i++;
		}
		if (i < len) {
			n++;
		}
		while (i < len && !isSep(s[i])) {
			i++;
		}
	}
	return n;
}

bool scrWord(const char *s, int len, int n, int *start, int *wlen)
{
	int k = 0;
	for (int i = 0; i < len;) {
		while (i < len && isSep(s[i])) {
			i++;
		}
		if (i >= len) {
			break;
		}
		int b = i;
		while (i < len && !isSep(s[i])) {
			i++;
		}
		if (++k == n) {
			*start = b;
			*wlen  = i - b;
			return true;
		}
	}
	return false;
}

/* ================= variables ================= */

scrVar_t *scrFindVar(bool global, const char *name, bool create)
{
	scrVar_t *tab;
	int      *n, max;
	if (global) {
		tab = scr.globals;
		n   = &scr.nGlobals;
		max = SCR_GLOBALS;
	} else {
		scrFrame_t *f = &scr.frames[scr.depth - 1];
		tab = f->locals;
		n   = &f->nLocals;
		max = SCR_LOCALS;
	}
	for (int i = 0; i < *n; i++) {
		if (strcasecmp(tab[i].name, name) == 0) {
			return &tab[i];
		}
	}
	if (!create) {
		return NULL;
	}
	if (*n >= max) {
		scrFail("too many %s variables (max %d)", global ? "global" : "local", max);
	}
	scrVar_t *v = &tab[(*n)++];
	memset(v, 0, sizeof(*v));
	snprintf(v->name, sizeof(v->name), "%s", name);
	return v;
}

void scrSetVar(scrVar_t *v, scrVal_t val)
{
	if (val.num) {
		v->num = true;
		v->n   = val.n;
		v->len = 0;
		return;
	}
	int len = val.len > SCR_VAL_MAX ? SCR_VAL_MAX : val.len;
	char *s = scrHostRealloc(v->s, len + 1);
	if (!s) {
		scrFail("out of memory");
	}
	memmove(s, val.s, len);
	s[len] = '\0';
	v->s   = s;
	v->len = len;
	v->num = false;
}

scrVal_t scrVarVal(const scrVar_t *v)
{
	if (v->num) {
		return scrNum(v->n);
	}
	return scrText(scrArenaStr(v->s ? v->s : "", v->len), v->len);
}

void scrFreeVar(scrVar_t *v)
{
	scrHostFree(v->s);
	v->s   = NULL;
	v->len = 0;
}

bool scrConstText(const char *name, int len)
{
	for (int i = 0; i < scr.nConst; i++) {
		if ((int)strlen(scr.cname[i]) == len && strncasecmp(scr.cname[i], name, len) == 0) {
			return scr.ctext[i];
		}
	}
	return false;
}

bool scrConstFind(const char *name, int len, const char **val)
{
	for (int i = 0; i < scr.nConst; i++) {
		if ((int)strlen(scr.cname[i]) == len && strncasecmp(scr.cname[i], name, len) == 0) {
			*val = scr.cval[i];
			return true;
		}
	}
	return false;
}

/* ================= random ================= */

static uint32_t rnd32(void)
{
	return scr.prng ? scrPrng(&s_prngState) : scrHostTrng();
}

void scrRngSeed(uint32_t seed)
{
	s_prngState = seed ? seed : 0x12345678;
}

double scrRnd16(void)
{
	return rnd32() & 0xFFFF;
}

/* ================= built-ins / variable references ================= */

static scrVal_t builtin(const char *name)
{
	if (s_skip) {
		return scrNum(0);
	}
	if (!strcasecmp(name, "ok")) {
		return scrNum(scr.ok);
	}
	if (!strcasecmp(name, "err")) {
		return scrText(scrArenaStr(scr.err, strlen(scr.err)), strlen(scr.err));
	}
	if (!strcasecmp(name, "ret")) {
		return scrVarVal(&scr.ret);
	}
	if (!strcasecmp(name, "event")) {
		return scrVarVal(&scr.event);
	}
	if (!strcasecmp(name, "runtime")) {
		return scrNum((double)(scrHostMs() - scr.t0));
	}
	if (!strcasecmp(name, "dropped")) {
		return scrNum(scrHostEventsDropped());
	}
	if (!strcasecmp(name, "rnd")) {
		return scrNum(scrRnd16());
	}
	char        buf[96];
	const char *err = NULL;
	if (scrHostBuiltin(name, buf, sizeof(buf), &err)) {
		double d;
		int    l = strlen(buf);
		if (scrParseNum(buf, l, &d)) {
			return scrNum(d);
		}
		return scrText(scrArenaStr(buf, l), l);
	}
	if (err) {
		scrFail("$.%s: %s", name, err);
	}
	scrFail("unknown built-in $.%s", name);
}

/* name chars at p (NV: '.' allowed after the first). */
static int nameLen(const char *p, bool nv)
{
	int l = 0;
	while (scrIsName(p[l], l == 0) || (nv && l > 0 && p[l] == '.' && scrIsName(p[l + 1], true))) {
		l++;
	}
	return l;
}

/* ${x/base,min,dec} */
static scrVal_t fmtBase(scrVal_t v, int base, int min, int dec)
{
	double d    = needNum(v);
	char   out[160];
	int    n    = 0;
	bool   neg  = d < 0;
	if (base == 10) {
		char fmt[16];
		snprintf(fmt, sizeof(fmt), "%%.%df", dec);
		char num[96];
		snprintf(num, sizeof(num), fmt, fabs(d));
		char *dot = strchr(num, '.');
		if (dot) {
			char *e = num + strlen(num) - 1;
			while (e > dot && *e == '0') {
				*e-- = '\0';
			}
			if (e == dot) {
				*e = '\0';
			}
		}
		int il = dot ? (int)(dot - num) : (int)strlen(num);
		if (neg && strcmp(num, "0") != 0) {
			out[n++] = '-';
		}
		for (int i = il; i < min && n < 100; i++) {
			out[n++] = '0';
		}
		n += snprintf(out + n, sizeof(out) - n, "%s", num);
	} else {
		uint64_t u = (uint64_t)fabs(trunc(d));
		char     digs[72];
		int      dn = 0;
		do {
			int x = u % base;
			digs[dn++] = x < 10 ? '0' + x : 'A' + x - 10;
			u /= base;
		} while (u && dn < 70);
		if (neg && trunc(d) != 0) {
			out[n++] = '-';
		}
		for (int i = dn; i < min && n < 100; i++) {
			out[n++] = '0';
		}
		while (dn) {
			out[n++] = digs[--dn];
		}
		out[n] = '\0';
	}
	return scrText(scrArenaStr(out, n), n);
}

/* $x  $$x  $$a.b  $.x  ${x}  $${x}  $.{x}  ${x/base,min,dec}, then [n] */
scrVal_t scrReadVarRef(const char **pp)
{
	const char *p = *pp + 1;
	int  kind = 0;				/* 0 local, 1 global, 2 built-in */
	if (*p == '$') {
		kind = 1;
		p++;
	} else if (*p == '.') {
		kind = 2;
		p++;
	}
	bool brace = (*p == '{');
	if (brace) {
		p++;
	}
	int l = nameLen(p, kind == 1);
	if (l == 0) {
		scrFail("bad variable name");
	}
	if (l > SCR_NAME_MAX && !(kind == 1 && memchr(p, '.', l))) {
		scrFail("name too long: %.20s", p);
	}
	char name[48];
	snprintf(name, sizeof(name), "%.*s", l, p);
	p += l;
	int base = 10, min = 0, dec = 3;
	bool fmt = false;
	if (brace && *p == '/') {
		fmt = true;
		p++;
		char *e;
		long  b = strtol(p, &e, 10);
		if (e != p) {
			base = (int)b;
			p    = e;
		}
		if (*p == ',') {
			p++;
			long m = strtol(p, &e, 10);
			if (e != p) {
				min = (int)m;
				p   = e;
			}
			if (*p == ',') {
				p++;
				long d = strtol(p, &e, 10);
				if (e != p) {
					dec = (int)d;
					p   = e;
				}
			}
		}
		if (base < 2 || base > 36 || min < 0 || min > 64 || dec < 0 || dec > 9) {
			scrFail("bad format ${%s/base,min,dec}", name);
		}
	}
	if (brace) {
		if (*p != '}') {
			scrFail("missing } after ${%s", name);
		}
		p++;
	}

	scrVal_t v;
	if (kind == 2) {
		v = builtin(name);
	} else if (kind == 1 && strchr(name, '.')) {
		char buf[SCR_VAL_MAX + 1];
		if (s_skip) {
			v = scrNum(0);
		} else if (!scrHostNv(name, buf, sizeof(buf))) {
			scrFail("no NV setting %s", name);
		} else {
			int    bl = strlen(buf);
			double d;
			v = scrParseNum(buf, bl, &d) ? scrNum(d) : scrText(scrArenaStr(buf, bl), bl);
		}
	} else {
		scrVar_t *var = scrFindVar(kind == 1, name, false);
		if (!var) {
			if (s_skip) {
				v = scrNum(0);
			} else {
				scrFail("%s%s is not set", kind ? "$$" : "$", name);
			}
		} else {
			v = scrVarVal(var);
		}
	}

	if (*p == '[') {
		p++;
		const char *save = s_stop;
		s_stop = NULL;
		scrVal_t ix = scrEval(&p, NULL);
		s_stop = save;
		p = scrSkip(p);
		if (*p != ']') {
			scrFail("missing ]");
		}
		p++;
		if (!s_skip) {
			double d = needNum(ix);
			int    tl;
			const char *t = scrToText(v, &tl);
			int    nw = scrWords(t, tl);
			if (d != floor(d) || d < 0 || d > nw) {
				scrFail("word %s of %s%s: out of range (0..%d)", scrFmtNum(d), kind ? "$$" : "$", name, nw);
			}
			if (d == 0) {
				v = scrNum(nw);
			} else {
				int ws, wl;
				scrWord(t, tl, (int)d, &ws, &wl);
				v = scrText(t + ws, wl);
			}
		}
	}
	if (fmt && !s_skip) {
		v = fmtBase(v, base, min, dec);
	}
	*pp = p;
	return v;
}

/* ================= expressions ================= */

static scrVal_t eOr(const char **pp);

static bool atWord(const char *p, const char *w)
{
	size_t l = strlen(w);
	return strncasecmp(p, w, l) == 0 && !scrIsName(p[l], false);
}

static bool atStopWord(const char *p)
{
	if (atWord(p, "AND") || atWord(p, "OR")) {
		return true;
	}
	if (s_stop) {
		for (const char *w = s_stop; *w;) {
			int l = 0;
			while (w[l] && w[l] != ' ') {
				l++;
			}
			if (strncasecmp(p, w, l) == 0 && !scrIsName(p[l], false)) {
				return true;
			}
			w += l;
			while (*w == ' ') {
				w++;
			}
		}
	}
	return false;
}

/* a primary (value) can start here -> two values side by side = join */
static bool startsValue(const char *p)
{
	char c = *p;
	if (c == '"' || c == '\'' || c == '$' || c == '(' || (c >= '0' && c <= '9')) {
		return true;
	}
	if (c == '.' && p[1] >= '0' && p[1] <= '9') {
		return true;
	}
	if ((c == '!' || c == '~') && p[1] != '=') {
		return true;
	}
	if (scrIsName(c, true)) {
		return !atStopWord(p);
	}
	return false;
}

/* "..." / '...' at *pp: substitution, then escapes */
static scrVal_t strLit(const char **pp)
{
	const char *p = *pp;
	char        q = *p++;
	char       *raw = scrArena(SCR_LINE_MAX + 1);
	int         n   = 0;
	for (;; p++) {
		if (!*p) {
			scrFail("missing closing quote");
		}
		if (*p == q) {
			break;
		}
		if (n >= SCR_LINE_MAX) {
			scrFail("text too long");
		}
		if (p[0] == '\\' && p[1] == q) {
			raw[n++] = q;
			p++;
		} else if (p[0] == '\\' && p[1]) {
			raw[n++] = *p++;
			raw[n++] = *p;
		} else {
			raw[n++] = *p;
		}
	}
	raw[n] = '\0';
	*pp = p + 1;
	if (s_skip) {
		return scrText("", 0);
	}
	const char *sub = scrSubst(raw, SUB_TEXT, 0);
	char       *out = scrArenaStr(sub, strlen(sub));
	scrUnescape(out);
	return scrText(out, strlen(out));
}

static scrVal_t numLit(const char **pp)
{
	const char *p = *pp;
	int l = 0;
	while (isalnum((unsigned char)p[l]) || p[l] == '.' ||
	       ((p[l] == '+' || p[l] == '-') && l > 0 && (p[l - 1] == 'e' || p[l - 1] == 'E') &&
	        !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))) {
		l++;
	}
	double d;
	if (!scrParseNum(p, l, &d)) {
		scrFail("bad number %.*s", l > 20 ? 20 : l, p);
	}
	*pp = p + l;
	return scrNum(d);
}

static scrVal_t callFunc(const char *name, int nl, const char **pp)
{
	scrVal_t a[3];
	int      n = 0;
	const char *p = scrSkip(*pp + 1);	/* after '(' */
	const char *save = s_stop;
	s_stop = NULL;
	if (*p != ')') {
		for (;;) {
			if (n >= 3) {
				scrFail("too many arguments");
			}
			a[n++] = eOr(&p);
			p = scrSkip(p);
			if (*p == ',') {
				p = scrSkip(p + 1);
				continue;
			}
			break;
		}
	}
	s_stop = save;
	if (*p != ')') {
		scrFail("missing )");
	}
	*pp = p + 1;

	static const struct { const char *n; int args; } fns[] = {
		{ "INT", 1 }, { "ROUND", 1 }, { "ABS", 1 }, { "MIN", 2 }, { "MAX", 2 }, { "SQRT", 1 },
		{ "SIN", 1 }, { "COS", 1 }, { "ATAN2", 2 }, { "LEN", 1 }, { "UPPER", 1 }, { "LOWER", 1 },
		{ "MID", 3 }, { "FIND", 2 }, { "RND", 2 },
	};
	int f = -1;
	for (int i = 0; i < (int)(sizeof(fns) / sizeof(fns[0])); i++) {
		if ((int)strlen(fns[i].n) == nl && strncasecmp(fns[i].n, name, nl) == 0) {
			f = i;
		}
	}
	if (f < 0) {
		scrFail("unknown function %.*s", nl > 20 ? 20 : nl, name);
	}
	if (n != fns[f].args) {
		scrFail("%s needs %d argument%s", fns[f].n, fns[f].args, fns[f].args > 1 ? "s" : "");
	}
	if (s_skip) {
		return scrNum(0);
	}
	const double rad = M_PI / 180.0;
	switch (f) {
	case 0: return scrNum(trunc(needNum(a[0])));
	case 1: return scrNum(round(needNum(a[0])));
	case 2: return scrNum(fabs(needNum(a[0])));
	case 3: return scrNum(fmin(needNum(a[0]), needNum(a[1])));
	case 4: return scrNum(fmax(needNum(a[0]), needNum(a[1])));
	case 5: {
		double d = needNum(a[0]);
		if (d < 0) {
			scrFail("SQRT of a negative number");
		}
		return scrNum(sqrt(d));
	}
	case 6: return scrNum(sin(needNum(a[0]) * rad));
	case 7: return scrNum(cos(needNum(a[0]) * rad));
	case 8: return scrNum(atan2(needNum(a[0]), needNum(a[1])) / rad);
	case 9: {
		int l;
		scrToText(a[0], &l);
		return scrNum(l);
	}
	case 10:
	case 11: {
		int         l;
		const char *t = scrToText(a[0], &l);
		char       *o = scrArenaStr(t, l);
		for (int i = 0; i < l; i++) {
			o[i] = (f == 10) ? toupper((unsigned char)o[i]) : tolower((unsigned char)o[i]);
		}
		return scrText(o, l);
	}
	case 12: {
		int         l;
		const char *t = scrToText(a[0], &l);
		double      st = needNum(a[1]), ln = needNum(a[2]);
		long        s0 = (long)trunc(st) - 1, cnt = (long)trunc(ln);
		if (s0 < 0) {
			cnt += s0;
			s0 = 0;
		}
		if (s0 >= l || cnt <= 0) {
			return scrText("", 0);
		}
		if (s0 + cnt > l) {
			cnt = l - s0;
		}
		return scrText(t + s0, (int)cnt);
	}
	case 13: {
		int         l, pl;
		const char *t  = scrToText(a[0], &l);
		const char *pt = scrToText(a[1], &pl);
		if (pl == 0) {
			return scrNum(l ? 1 : 0);
		}
		for (int i = 0; i + pl <= l; i++) {
			if (memcmp(t + i, pt, pl) == 0) {
				return scrNum(i + 1);
			}
		}
		return scrNum(0);
	}
	default: {
		double lo = trunc(needNum(a[0])), hi = trunc(needNum(a[1]));
		if (hi < lo) {
			double t = lo;
			lo = hi;
			hi = t;
		}
		double span = hi - lo + 1;
		uint64_t r  = ((uint64_t)rnd32() << 32) | rnd32();
		return scrNum(lo + (double)(r % (uint64_t)span));
	}
	}
}

static scrVal_t ePrimary(const char **pp)
{
	const char *p = scrSkip(*pp);
	char        c = *p;
	scrVal_t    v;

	if (++scr.exprDepth > SCR_EXPR_DEPTH) {
		scrFail("expression nested too deep");
	}
	if (c == '(') {
		p++;
		const char *save = s_stop;
		s_stop = NULL;
		v = eOr(&p);
		s_stop = save;
		p = scrSkip(p);
		if (*p != ')') {
			scrFail("missing )");
		}
		p++;
	} else if (c == '"' || c == '\'') {
		v = strLit(&p);
	} else if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9')) {
		v = numLit(&p);
	} else if (c == '$') {
		v = scrReadVarRef(&p);
	} else if (scrIsName(c, true)) {
		int l = 0;
		while (scrIsName(p[l], l == 0)) {
			l++;
		}
		const char *after = scrSkip(p + l);
		const char *cv;
		if (*after == '(') {
			const char *nm = p;
			p = after;
			v = callFunc(nm, l, &p);
		} else if (scrConstFind(p, l, &cv)) {
			double d;
			int    cl = strlen(cv);
			v = (!scrConstText(p, l) && scrParseNum(cv, cl, &d)) ? scrNum(d) : scrText(cv, cl);
			p += l;
		} else {
			scrFail("unknown name %.*s (text needs quotes)", l > 20 ? 20 : l, p);
		}
	} else if (c == '\0') {
		scrFail("value expected");
	} else {
		scrFail("unexpected '%c'", c);
	}
	scr.exprDepth--;
	*pp = p;
	return v;
}

static scrVal_t eUnary(const char **pp)
{
	const char *p = scrSkip(*pp);
	if (*p == '!' && p[1] != '=') {
		p++;
		scrVal_t v = eUnary(&p);
		*pp = p;
		return s_skip ? v : scrNum(!scrTruth(v));
	}
	if (*p == '-') {
		p++;
		scrVal_t v = eUnary(&p);
		*pp = p;
		return s_skip ? v : scrNum(-needNum(v));
	}
	if (*p == '~' && p[1] != '=') {
		p++;
		scrVal_t v = eUnary(&p);
		*pp = p;
		return s_skip ? v : scrNum((double)~needWhole(v));
	}
	*pp = p;
	return ePrimary(pp);
}

static scrVal_t eMul(const char **pp)
{
	scrVal_t a = eUnary(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		char        op = *p;
		if (op != '*' && op != '/' && op != '%') {
			return a;
		}
		p++;
		scrVal_t b = eUnary(&p);
		*pp = p;
		if (s_skip) {
			continue;
		}
		double x = needNum(a), y = needNum(b);
		if ((op == '/' || op == '%') && y == 0) {
			scrFail("divide by zero");
		}
		a = scrNum(op == '*' ? x * y : op == '/' ? x / y : fmod(x, y));
	}
}

static scrVal_t eAdd(const char **pp)
{
	scrVal_t a = eMul(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		char        op = *p;
		if (op != '+' && op != '-') {
			return a;
		}
		p++;
		scrVal_t b = eMul(&p);
		*pp = p;
		if (!s_skip) {
			if (op == '+' && (!scrToNum(a, &(double){0}) || !scrToNum(b, &(double){0}))) {
				scrFail("'+' needs numbers (join text by putting values side by side)");
			}
			a = scrNum(op == '+' ? needNum(a) + needNum(b) : needNum(a) - needNum(b));
		}
	}
}

static scrVal_t eShift(const char **pp)
{
	scrVal_t a = eAdd(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		if (!((p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>'))) {
			return a;
		}
		bool left = (p[0] == '<');
		p += 2;
		scrVal_t b = eAdd(&p);
		*pp = p;
		if (!s_skip) {
			int64_t x = needWhole(a), y = needWhole(b);
			if (y < 0 || y > 63) {
				scrFail("shift must be 0..63");
			}
			a = scrNum((double)(left ? (int64_t)((uint64_t)x << y) : x >> y));
		}
	}
}

static scrVal_t eBand(const char **pp)
{
	scrVal_t a = eShift(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		if (*p != '&') {
			return a;
		}
		p++;
		scrVal_t b = eShift(&p);
		*pp = p;
		if (!s_skip) {
			a = scrNum((double)(needWhole(a) & needWhole(b)));
		}
	}
}

static scrVal_t eBxor(const char **pp)
{
	scrVal_t a = eBand(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		if (*p != '^') {
			return a;
		}
		p++;
		scrVal_t b = eBand(&p);
		*pp = p;
		if (!s_skip) {
			a = scrNum((double)(needWhole(a) ^ needWhole(b)));
		}
	}
}

static scrVal_t eBor(const char **pp)
{
	scrVal_t a = eBxor(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		if (*p != '|') {
			return a;
		}
		p++;
		scrVal_t b = eBxor(&p);
		*pp = p;
		if (!s_skip) {
			a = scrNum((double)(needWhole(a) | needWhole(b)));
		}
	}
}

/* values side by side -> joined with one space */
static scrVal_t eJoin(const char **pp, int *parts)
{
	scrVal_t a = eBor(pp);
	*parts = 1;
	for (;;) {
		const char *p = scrSkip(*pp);
		if (!startsValue(p)) {
			return a;
		}
		scrVal_t b = eBor(&p);
		*pp = p;
		(*parts)++;
		if (!s_skip) {
			int         al, bl;
			const char *at = scrToText(a, &al);
			const char *bt = scrToText(b, &bl);
			char       *o  = scrArena(al + bl + 2);
			memcpy(o, at, al);
			o[al] = ' ';
			memcpy(o + al + 1, bt, bl);
			o[al + bl + 1] = '\0';
			a = scrText(o, al + bl + 1);
		}
	}
}

static scrVal_t eCmp(const char **pp)
{
	int      parts;
	scrVal_t a = eJoin(pp, &parts);
	const char *p = scrSkip(*pp);
	int op = 0;					/* 1 <  2 <=  3 >  4 >=  5 ==  6 !=  7 ~= */
	if (p[0] == '<' && p[1] == '=') {
		op = 2;
	} else if (p[0] == '<') {
		op = 1;
	} else if (p[0] == '>' && p[1] == '=') {
		op = 4;
	} else if (p[0] == '>') {
		op = 3;
	} else if (p[0] == '=' && p[1] == '=') {
		op = 5;
	} else if (p[0] == '!' && p[1] == '=') {
		op = 6;
	} else if (p[0] == '~' && p[1] == '=') {
		op = 7;
	}
	if (!op) {
		return a;
	}
	if (parts > 1) {
		scrFail("joined values left of a comparison");
	}
	p += (op == 1 || op == 3) ? 1 : 2;
	int      bparts;
	scrVal_t b = eJoin(&p, &bparts);
	*pp = p;
	if (s_skip) {
		return scrNum(0);
	}
	double x, y;
	int    c;
	if (op != 7 && scrToNum(a, &x) && scrToNum(b, &y)) {
		c = (x < y) ? -1 : (x > y) ? 1 : 0;
	} else {
		int         al, bl;
		const char *at = scrToText(a, &al);
		const char *bt = scrToText(b, &bl);
		if (op == 7) {
			return scrNum(al == bl && strncasecmp(at, bt, al) == 0);
		}
		int m = al < bl ? al : bl;
		c = memcmp(at, bt, m);
		if (c == 0) {
			c = (al < bl) ? -1 : (al > bl) ? 1 : 0;
		}
	}
	switch (op) {
	case 1: return scrNum(c < 0);
	case 2: return scrNum(c <= 0);
	case 3: return scrNum(c > 0);
	case 4: return scrNum(c >= 0);
	case 5: return scrNum(c == 0);
	default: return scrNum(c != 0);
	}
}

static scrVal_t eAnd(const char **pp)
{
	scrVal_t a = eCmp(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		if (!atWord(p, "AND")) {
			return a;
		}
		p += 3;
		bool skip = !s_skip && !scrTruth(a);
		if (skip) {
			s_skip++;
		}
		scrVal_t b = eCmp(&p);
		if (skip) {
			s_skip--;
		}
		*pp = p;
		if (!s_skip) {
			a = skip ? scrNum(0) : scrNum(scrTruth(b));
		}
	}
}

static scrVal_t eOr(const char **pp)
{
	scrVal_t a = eAnd(pp);
	for (;;) {
		const char *p = scrSkip(*pp);
		if (!atWord(p, "OR")) {
			return a;
		}
		p += 2;
		bool skip = !s_skip && scrTruth(a);
		if (skip) {
			s_skip++;
		}
		scrVal_t b = eAnd(&p);
		if (skip) {
			s_skip--;
		}
		*pp = p;
		if (!s_skip) {
			a = skip ? scrNum(1) : scrNum(scrTruth(b));
		}
	}
}

scrVal_t scrEval(const char **pp, const char *stopWords)
{
	const char *save = s_stop;
	s_stop = stopWords;
	scrVal_t v = eOr(pp);
	s_stop = save;
	return v;
}

scrVal_t scrEvalAll(const char *s)
{
	const char *p = s;
	s_skip = 0;
	scr.exprDepth = 0;
	scrVal_t v = scrEval(&p, NULL);
	p = scrSkip(p);
	if (*p) {
		scrFail("unexpected \"%.20s\"", p);
	}
	return v;
}

/* ================= text: substitution, quotes, escapes ================= */

/* Copy value text t into o with the escaping of the mode. */
static int putVal(char *o, int n, int cap, const char *t, int tl, int mode, char quote)
{
	for (int i = 0; i < tl; i++) {
		char c = t[i];
		if (n + 2 >= cap) {
			scrFail("line too long after substitution");
		}
		if ((mode == SUB_TEXT && c == '\\') || (mode == SUB_CMD && c == '#') ||
		    (mode == SUB_RAW && (c == '\\' || c == '#' || (quote && c == quote)))) {
			o[n++] = '\\';
		}
		o[n++] = c;
	}
	return n;
}

const char *scrSubst(const char *s, int mode, char quote)
{
	int   cap = SCR_LINE_MAX * 4 + 1;
	char *o   = scrArena(cap);
	int   n   = 0;

	for (const char *p = s; *p;) {
		if (n + 2 >= cap) {
			scrFail("line too long after substitution");
		}
		if (p[0] == '\\' && p[1] == '$') {
			if ((mode == SUB_CMD || mode == SUB_PLAIN) && !quote) {
				o[n++] = '$';
			} else {
				o[n++] = '\\';
				o[n++] = '$';
			}
			p += 2;
			continue;
		}
		if (p[0] == '\\' && p[1]) {
			o[n++] = *p++;
			o[n++] = *p++;
			continue;
		}
		if (p[0] == '$') {
			const char *q = p + 1;
			if (*q == '$' || *q == '.') {
				q++;
			}
			if (*q == '{') {
				q++;
			}
			if (scrIsName(*q, true)) {
				scrVal_t v = scrReadVarRef(&p);
				int      tl;
				const char *t = scrToText(v, &tl);
				n = putVal(o, n, cap, t, tl, mode, quote);
				continue;
			}
		}
		o[n++] = *p++;
	}
	o[n] = '\0';
	return o;
}

const char *scrUnquote(const char *in)
{
	const char *p = scrSkip(in);
	int         len = strlen(in);
	char       *out = scrArena(len + 1);
	int         o   = 0;

	if (*p == '"' || *p == '\'') {
		char q = *p++;
		for (;; p++) {
			if (!*p) {
				scrFail("missing closing quote");
			}
			if (*p == q) {
				break;
			}
			if (p[0] == '\\' && p[1] == q) {
				out[o++] = q;
				p++;
			} else if (p[0] == '\\' && p[1]) {
				out[o++] = *p++;
				out[o++] = *p;
			} else {
				out[o++] = *p;
			}
		}
		if (*scrSkip(p + 1)) {
			scrFail("text after closing quote");
		}
	} else {
		const char *skip = (p[0] == '\\' && (p[1] == '"' || p[1] == '\'')) ? p : NULL;
		for (const char *c = in; *c; c++) {
			if (c != skip) {
				out[o++] = *c;
			}
		}
	}
	out[o] = '\0';
	return out;
}

void scrUnescape(char *s)
{
	char *o = s;
	for (const char *p = s; *p; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			*o++ = '\n';
			p++;
		} else if (p[0] == '\\' && (p[1] == '\\' || p[1] == '#' || p[1] == '$')) {
			*o++ = p[1];
			p++;
		} else if (p[0] == '\\' && p[1] >= '1' && p[1] <= '9') {
			p++;
		} else {
			*o++ = *p;
		}
	}
	*o = '\0';
}
