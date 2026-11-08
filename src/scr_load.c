/* Script loader: files, INSERT, statements, blocks, functions, labels, VARS,
 * .STACK / .CONST, and the files named by literal CALL file.fn / ON src file.fn.
 * Errors longjmp out through scrFail() with scr.locSrc / scr.locLine set. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "scr_int.h"

static const struct {
	const char *w;
	uint8_t     k;
} s_kw[] = {
	{ "LET", K_LET },	{ "GET", K_GET },	{ "VARS", K_VARS },	{ "DEL", K_DEL },
	{ "INS", K_INS },	{ "IF", K_IF },		{ "ELIF", K_ELIF },	{ "ELSE", K_ELSE },
	{ "END", K_END },	{ "LOOP", K_LOOP },	{ "BREAK", K_BREAK },	{ "NEXT", K_NEXT },
	{ "SWITCH", K_SWITCH },	{ "GOTO", K_GOTO },	{ "EXIT", K_EXIT },	{ "FN", K_FN },
	{ "RETURN", K_RETURN },	{ "CALL", K_CALL },	{ "ON", K_ON },		{ "ONERROR", K_ONERROR },
	{ "QUIET", K_QUIET },	{ "TRACE", K_TRACE },	{ "PRINT", K_PRINT },	{ "DEBUG", K_DEBUG },
	{ "RNG", K_RNG },	{ "TIMER", K_TIMER },	{ "WAIT", K_WAIT },	{ "INPUT", K_INPUT },
	{ "BPT!", K_BPT },	{ "ACC", K_ACC },	{ "GYR", K_GYR },	{ ".STACK", K_STACK },
	{ ".FILES", K_FILES },
	{ "FWRITE", K_FWRITE },	{ "FREAD", K_FREAD },	{ "FTELL", K_FTELL },	{ "FSEEK", K_FSEEK },
	{ "FCLOSE", K_FCLOSE },
	{ ".CONST", K_CONST },
	{ "RUN", K_CONSOLE },	{ "STOP", K_CONSOLE },	{ "CONT", K_CONSOLE },	{ "LOAD", K_CONSOLE },
	{ "STEP", K_CONSOLE },	{ "BPT", K_CONSOLE },	{ "LIST", K_CONSOLE },	{ "STACK", K_CONSOLE },
	{ "PAUSE", K_PAUSE },
};

/* Reserved words (no .CONST or function of that name) */
static const char *const s_reserved[] = {
	"LET", "GET", "VARS", "DEL", "INS", "IF", "ELIF", "ELSE", "END", "LOOP", "TO", "STEP",
	"WHILE", "TIMES", "FOREVER", "BREAK", "NEXT", "SWITCH", "GOTO", "EXIT", "FN", "RETURN",
	"CALL", "INSERT", "ON", "OFF", "ONERROR", "QUIET", "TRACE", "PRINT", "DEBUG", "FWRITE",
	"FREAD", "FTELL", "FSEEK", "FCLOSE", "SAVE",
	"RNG", "TIMER", "EVENT", "NOW", "INPUT", "RUN", "STOP", "PAUSE", "CONT", "LOAD", "BPT",
	"LIST", "STACK", "LOG", "EVENTS", "AND", "OR", "INT", "ROUND", "ABS", "MIN", "MAX",
	"SQRT", "SIN", "COS", "ATAN2", "LEN", "UPPER", "LOWER", "MID", "FIND", "RND", NULL,
};

static void loadFail(int src, int line, const char *fmt, const char *a) __attribute__((noreturn));
static void loadFail(int src, int line, const char *fmt, const char *a)
{
	scr.locSrc  = src;
	scr.locLine = line;
	scrFail(fmt, a ? a : "");
}

const char *scrSrcName(int src)
{
	return (src >= 0 && src < scr.nSrc) ? scr.src[src].name : "?";
}

bool scrIsReserved(const char *w, int len)
{
	for (int i = 0; s_reserved[i]; i++) {
		if ((int)strlen(s_reserved[i]) == len && strncasecmp(s_reserved[i], w, len) == 0) {
			return true;
		}
	}
	return false;
}

/* ---------- statement building ---------- */

static void addStmt(scrMod_t *m, int src, int line, bool lineStart, const char *text)
{
	if (m->n == m->cap) {
		int        cap = m->cap ? m->cap * 2 : 64;
		scrStmt_t *st  = scrHostRealloc(m->st, cap * sizeof(scrStmt_t));
		if (!st) {
			loadFail(src, line, "out of memory%s", NULL);
		}
		m->st  = st;
		m->cap = cap;
	}
	size_t len = strlen(text);
	char  *t   = scrHostAlloc(len + 1);
	if (!t) {
		loadFail(src, line, "out of memory%s", NULL);
	}
	memcpy(t, text, len + 1);
	scrStmt_t *s = &m->st[m->n++];
	memset(s, 0, sizeof(*s));
	s->src       = src;
	s->line      = line;
	s->lineStart = lineStart;
	s->text      = t;
	s->jump = s->end = s->blk = s->fn = -1;
}

static void loadFile(scrMod_t *m, const char *name, int depth, const char **chain);

/* One source line -> statements (split on ';' outside quotes, comment cut). */
static void emit(scrMod_t *m, int src, int lineNo, char *t, bool *first, int depth, const char **chain)
{
	while (*t == ' ' || *t == '\t') {
		t++;
	}
	char *e = t + strlen(t);
	while (e > t && (e[-1] == ' ' || e[-1] == '\t')) {
		*--e = '\0';
	}
	if (!*t) {
		return;
	}
	if (scrWordIs(t, "INSERT")) {
		char        fn[SCR_FNAME_MAX + 8];
		const char *a = scrSkip(t + 6);
		int         l = 0;
		while (a[l] && a[l] != ' ' && a[l] != '\t' && l < (int)sizeof(fn) - 1) {
			fn[l] = a[l];
			l++;
		}
		fn[l] = '\0';
		if (l > 2 && strcasecmp(fn + l - 2, ".s") == 0) {
			fn[l - 2] = '\0';
		}
		if (!fn[0] || *scrSkip(a + l)) {
			loadFail(src, lineNo, "usage: INSERT file.s%s", NULL);
		}
		scr.locSrc  = src;
		scr.locLine = lineNo;
		loadFile(m, fn, depth + 1, chain);
	} else {
		addStmt(m, src, lineNo, *first, t);
	}
	*first = false;
}

static void splitLine(scrMod_t *m, int src, int lineNo, const char *line, int depth, const char **chain)
{
	char buf[SCR_LINE_MAX + 2];
	int  n     = 0;
	char q     = 0;
	bool first = true;

	for (const char *p = line; *p; p++) {
		char c = *p;
		if (n >= SCR_LINE_MAX) {
			loadFail(src, lineNo, "line too long%s", NULL);
		}
		if (q) {
			if (c == '\\' && (p[1] == q || p[1] == '\\')) {
				buf[n++] = c;
				c = *++p;
			} else if (c == q) {
				q = 0;
			}
			buf[n++] = c;
		} else if (c == '\\' && p[1] == ';') {
			buf[n++] = ';';
			p++;
		} else if (c == '\\' && p[1]) {
			buf[n++] = c;
			buf[n++] = *++p;
		} else if (c == '"' || c == '\'') {
			q = c;
			buf[n++] = c;
		} else if (c == ';') {
			buf[n] = '\0';
			emit(m, src, lineNo, buf, &first, depth, chain);
			n = 0;
		} else if (c == '#') {
			break;					/* comment */
		} else {
			buf[n++] = c;
		}
	}
	buf[n] = '\0';
	emit(m, src, lineNo, buf, &first, depth, chain);
}

/* Read name.s, check it, add its statements (INSERTs expanded). */
static void loadFile(scrMod_t *m, const char *name, int depth, const char **chain)
{
	int ps = scr.locSrc, pl = scr.locLine;
	if (depth > SCR_INSERT_DEPTH) {
		loadFail(ps, pl, "INSERT nested too deep (%s)", name);
	}
	for (int i = 0; i < depth; i++) {
		if (strcasecmp(chain[i], name) == 0) {
			char msg[64];
			snprintf(msg, sizeof(msg), "%s.s (line %d)", name, pl);
			loadFail(ps, pl, "recursive INSERT %s", msg);
		}
	}
	if (scr.nSrc >= SCR_SRC_MAX) {
		loadFail(ps, pl, "too many files%s", NULL);
	}
	size_t      len;
	const char *err = "file not found";
	char       *text = scrHostRead(name, &len, &err);
	if (!text) {
		scr.locSrc  = ps;
		scr.locLine = pl;
		scrFail("%s.s: %s", name, err);
	}
	scr.loaded += len;
	int       si = scr.nSrc++;
	scrSrc_t *sf = &scr.src[si];
	memset(sf, 0, sizeof(*sf));
	snprintf(sf->name, sizeof(sf->name), "%s", name);
	sf->text = text;
	if (scr.loaded > SCR_FILE_MAX) {
		loadFail(si, 0, "file too big (max 32 KB with INSERTs)%s", NULL);
	}

	char *p = text;
	if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
		p += 3;						/* UTF-8 BOM */
	}
	int nl = 1;
	for (char *q = p; *q; q++) {
		nl += (*q == '\n');
	}
	sf->lines = scrHostAlloc(nl * sizeof(char *));
	if (!sf->lines) {
		loadFail(si, 0, "out of memory%s", NULL);
	}
	/* cut into lines (CR, LF, CR LF) */
	while (*p) {
		char *start = p;
		while (*p && *p != '\n' && *p != '\r') {
			p++;
		}
		char *next = p;
		if (*next == '\r' && next[1] == '\n') {
			next += 2;
		} else if (*next) {
			next++;
		}
		*p = '\0';
		sf->lines[sf->nLines++] = start;
		p = next;
	}
	chain[depth] = sf->name;
	for (int i = 0; i < sf->nLines; i++) {
		const char *l = sf->lines[i];
		for (const char *c = l; *c; c++) {
			if ((uint8_t)*c >= 0x80 || ((uint8_t)*c < ' ' && *c != '\t')) {
				loadFail(si, i + 1, "not an ASCII character%s", NULL);
			}
		}
		if (strlen(l) > SCR_LINE_MAX) {
			loadFail(si, i + 1, "line too long%s", NULL);
		}
		splitLine(m, si, i + 1, l, depth, chain);
	}
}

/* ---------- structure pass ---------- */

static int kindOf(const char *t)
{
	if (t[0] == ':') {
		return K_LABEL;
	}
	if (t[0] == '$') {
		size_t l = strlen(t);
		if (l > 2 && (strcmp(t + l - 2, "++") == 0 || strcmp(t + l - 2, "--") == 0)) {
			return K_INCR;
		}
	}
	if (scrWordIs(t, "VARS")) {
		const char *a = scrSkip(t + 4);
		if (scrWordIs(a, "SAVE") || scrWordIs(a, "LOAD")) {
			return K_VARSIO;			/* VARS SAVE / LOAD: a statement, not a declaration */
		}
	}
	for (size_t i = 0; i < sizeof(s_kw) / sizeof(s_kw[0]); i++) {
		if (scrWordIs(t, s_kw[i].w)) {
			return s_kw[i].k;
		}
	}
	return K_CMD;
}

static void setArg(scrStmt_t *s)
{
	const char *t = s->text;
	if (s->kind == K_CMD || s->kind == K_INCR || s->kind == K_CONSOLE || s->kind == K_PAUSE) {
		s->arg = s->text;
		return;
	}
	if (s->kind == K_LABEL) {
		s->arg = (char *)scrSkip(t + 1);
		return;
	}
	while (*t && *t != ' ' && *t != '\t') {
		t++;
	}
	s->arg = (char *)scrSkip(t);
}

/* first word ends with ')' -> label */
static int labelLen(const char *t)
{
	int i = 0;
	while (t[i] && t[i] != ' ' && t[i] != '\t') {
		i++;
	}
	return (i > 1 && t[i - 1] == ')') ? i : 0;
}

/* Name list helper: append " name" to a growing string. */
static char *addName(char *list, const char *name, int len)
{
	size_t old = list ? strlen(list) : 0;
	char  *n   = scrHostRealloc(list, old + len + 2);
	if (!n) {
		scrFail("out of memory");
	}
	n[old] = ' ';
	memcpy(n + old + 1, name, len);
	n[old + 1 + len] = '\0';
	return n;
}

static bool hasName(const char *list, const char *name, int len);

/* name may be set in body fn (-1 = main) of m: no VARS there, or listed */
bool scrNameAllowed(scrMod_t *m, int fn, const char *name, bool global)
{
	bool        has  = fn >= 0 ? m->fns[fn].hasVars : m->mainHasVars;
	const char *list = fn >= 0 ? m->fns[fn].vars : m->mainVars;
	if (!has) {
		return true;
	}
	if (global) {
		char tmp[SCR_NAME_MAX + 2];
		snprintf(tmp, sizeof(tmp), "$%s", name);
		return hasName(list, tmp, strlen(tmp));
	}
	return strcasecmp(name, "arg") == 0 || hasName(list, name, strlen(name));
}

static bool hasName(const char *list, const char *name, int len)
{
	if (!list) {
		return false;
	}
	for (const char *p = list; *p;) {
		while (*p == ' ') {
			p++;
		}
		int l = 0;
		while (p[l] && p[l] != ' ') {
			l++;
		}
		if (l == len && strncasecmp(p, name, len) == 0) {
			return true;
		}
		p += l;
	}
	return false;
}

/* VARS line: add its names to *list. */
static void varsLine(scrStmt_t *s, char **list)
{
	const char *p = s->arg;
	if (!*p) {
		loadFail(s->src, s->line, "usage: VARS $a [$b ...]%s", NULL);
	}
	while (*p) {
		bool g = (p[0] == '$' && p[1] == '$');
		if (p[0] != '$') {
			loadFail(s->src, s->line, "VARS: expected $name%s", NULL);
		}
		p += g ? 2 : 1;
		int l = 0;
		while (scrIsName(p[l], l == 0)) {
			l++;
		}
		if (l == 0 || l > SCR_NAME_MAX || (p[l] && p[l] != ' ' && p[l] != '\t')) {
			loadFail(s->src, s->line, "VARS: bad name%s", NULL);
		}
		if (g) {
			char tmp[SCR_NAME_MAX + 2];
			tmp[0] = '$';
			memcpy(tmp + 1, p, l);
			*list = addName(*list, tmp, l + 1);
		} else {
			*list = addName(*list, p, l);
		}
		p = scrSkip(p + l);
	}
}

/* Check every $name in statement s against list (VARS + params + arg). */
static void checkNames(scrStmt_t *s, const char *list)
{
	for (const char *p = s->text; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p != '$') {
			continue;
		}
		bool g = (p[1] == '$');
		const char *n = p + (g ? 2 : 1);
		if (*n == '.') {
			p = n;
			continue;					/* built-in */
		}
		bool brace = (*n == '{');
		if (brace) {
			n++;
		}
		int l = 0;
		while (scrIsName(n[l], l == 0) || (g && l > 0 && n[l] == '.')) {
			l++;
		}
		if (l == 0) {
			continue;
		}
		if (g && memchr(n, '.', l)) {
			p = n + l - 1;
			continue;					/* NV */
		}
		bool ok;
		if (g) {
			char tmp[SCR_NAME_MAX + 2];
			tmp[0] = '$';
			int cl = l > SCR_NAME_MAX ? SCR_NAME_MAX : l;
			memcpy(tmp + 1, n, cl);
			ok = hasName(list, tmp, cl + 1);
		} else {
			ok = (l == 3 && strncasecmp(n, "arg", 3) == 0) || hasName(list, n, l);
		}
		if (!ok) {
			char nm[SCR_NAME_MAX + 4];
			snprintf(nm, sizeof(nm), "%s%.*s", g ? "$$" : "$", l > SCR_NAME_MAX ? SCR_NAME_MAX : l, n);
			loadFail(s->src, s->line, "%s is not in VARS", nm);
		}
		p = n + l - 1;
	}
}

/* .CONST NAME value... */
static void constLine(scrStmt_t *s)
{
	const char *p = s->arg;
	int         l = 0;
	while (scrIsName(p[l], l == 0)) {
		l++;
	}
	if (l == 0 || l > SCR_NAME_MAX || !(p[l] == ' ' || p[l] == '\t')) {
		loadFail(s->src, s->line, "usage: .CONST NAME value%s", NULL);
	}
	char name[SCR_NAME_MAX + 1];
	memcpy(name, p, l);
	name[l] = '\0';
	if (scrIsReserved(name, l) || scrHostIsCommand(name)) {
		loadFail(s->src, s->line, ".CONST %s: reserved word or command", name);
	}
	for (int i = 0; i < scr.nConst; i++) {
		if (strcasecmp(scr.cname[i], name) == 0) {
			loadFail(s->src, s->line, ".CONST %s defined twice", name);
		}
	}
	if (scr.nConst >= SCR_CONSTS) {
		loadFail(s->src, s->line, "too many .CONST (max 32)%s", NULL);
	}
	/* value words, 0x / 0b numbers written in decimal; or one quoted string:
	 * text, kept exactly, without the quotes */
	char  val[SCR_LINE_MAX + 1];
	int   vn = 0;
	bool  text = false;
	p = scrSkip(p + l);
	if (!*p) {
		loadFail(s->src, s->line, "usage: .CONST NAME value%s", NULL);
	}
	if (*p == '"' || *p == '\'') {
		const char *e = strchr(p + 1, *p);
		if (e && !*scrSkip(e + 1)) {
			vn = snprintf(val, sizeof(val), "%.*s", (int)(e - p - 1), p + 1);
			text = true;
			p = "";
		}
	}
	while (*p) {
		int wl = 0;
		while (p[wl] && p[wl] != ' ' && p[wl] != '\t') {
			wl++;
		}
		double d;
		char   w[64];
		if (wl > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B') &&
		    scrParseNum(p, wl, &d)) {
			scrFmtNumBuf(d, w, sizeof(w));
		} else {
			snprintf(w, sizeof(w), "%.*s", wl > 63 ? 63 : wl, p);
		}
		vn += snprintf(val + vn, sizeof(val) - vn, "%s%s", vn ? " " : "", w);
		if (vn >= (int)sizeof(val)) {
			loadFail(s->src, s->line, ".CONST value too long%s", NULL);
		}
		p = scrSkip(p + wl);
	}
	char *v = scrHostAlloc(vn + 1);
	if (!v) {
		loadFail(s->src, s->line, "out of memory%s", NULL);
	}
	memcpy(v, val, vn + 1);
	snprintf(scr.cname[scr.nConst], sizeof(scr.cname[0]), "%s", name);
	scr.ctext[scr.nConst] = text;
	scr.cval[scr.nConst++] = v;
}

static void structure(scrMod_t *m)
{
	scrStmt_t *in  = m->st;
	int        nIn = m->n;
	m->raw  = in;			/* freed by scrUnloadAll if a load error jumps out */
	m->nRaw = nIn;
	m->st   = NULL;
	m->n    = m->cap = 0;

	struct { int open, last; bool seenElse; } stk[SCR_BLOCK_MAX];
	int  sp    = 0;
	int  curFn = -1;
	bool other = false;		/* a statement other than .STACK/.FILES/.CONST seen */
	bool bodyStarted = false;	/* VARS allowed only at the start of a body */

	for (int i = 0; i < nIn; i++) {
		char *rest = in[i].text;
		bool  ls   = in[i].lineStart;
		while (rest) {
			char *next = NULL;
			int   k    = kindOf(rest);
			/* SWITCH label: "value) statement" */
			if (sp > 0 && m->st[stk[sp - 1].open].kind == K_SWITCH && k != K_END) {
				int ll = labelLen(rest);
				if (ll) {
					rest[ll - 1] = '\0';
					char *after = (char *)scrSkip(rest + ll);
					if (*after) {
						next = after;
					}
					k = K_CASE;
				}
			}
			addStmt(m, in[i].src, in[i].line, ls, rest);
			ls = false;
			int        idx = m->n - 1;
			scrStmt_t *s   = &m->st[idx];
			s->kind = k;
			setArg(s);
			s->fn   = curFn;
			s->blk  = sp ? stk[sp - 1].open : -1;
			int src = s->src, line = s->line;

			if (k == K_STACK || k == K_FILES || k == K_CONST) {
				if (other) {
					loadFail(src, line, "%s must be at the top of the script",
					         k == K_STACK ? ".STACK" : k == K_FILES ? ".FILES" : ".CONST");
				}
				if (k == K_STACK || k == K_FILES) {
					double d;
					int    max = (k == K_STACK) ? SCR_STACK_MAX : SCR_FILES_MAX;
					if (!scrParseNum(s->arg, strlen(s->arg), &d) || d < 1 || d > max || d != (int)d) {
						loadFail(src, line, k == K_STACK ? ".STACK must be 1..32%s" : ".FILES must be 1..16%s", NULL);
					}
					if (k == K_STACK) {
						scr.stackMax = (int)d;
					} else {
						scr.filesMax = (int)d;
					}
				} else {
					constLine(s);
				}
				s = NULL;
			} else {
				other = true;
			}
			if (!s) {
				rest = next;
				continue;
			}
			if (k == K_VARS) {
				if (bodyStarted) {
					loadFail(src, line, "VARS must be at the start of the body%s", NULL);
				}
				if (curFn >= 0) {
					m->fns[curFn].hasVars = true;
					varsLine(s, &m->fns[curFn].vars);
				} else {
					m->mainHasVars = true;
					varsLine(s, &m->mainVars);
				}
			} else if (k != K_FN && k != K_END) {
				bodyStarted = true;
			}

			switch (k) {
			case K_IF:
			case K_LOOP:
			case K_SWITCH:
				if (sp >= SCR_BLOCK_MAX) {
					loadFail(src, line, "blocks nested too deep (max 8)%s", NULL);
				}
				stk[sp].open = stk[sp].last = idx;
				stk[sp].seenElse = false;
				sp++;
				break;
			case K_ELIF:
			case K_ELSE:
				if (!sp || m->st[stk[sp - 1].open].kind != K_IF) {
					loadFail(src, line, "%s without IF", k == K_ELIF ? "ELIF" : "ELSE");
				}
				if (stk[sp - 1].seenElse) {
					loadFail(src, line, "%s after ELSE", k == K_ELIF ? "ELIF" : "ELSE");
				}
				m->st[stk[sp - 1].last].jump = idx;
				stk[sp - 1].last = idx;
				stk[sp - 1].seenElse = (k == K_ELSE);
				s->blk = sp > 1 ? stk[sp - 2].open : -1;
				break;
			case K_CASE:
				m->st[stk[sp - 1].last].jump = idx;
				stk[sp - 1].last = idx;
				s->blk = sp > 1 ? stk[sp - 2].open : -1;
				break;
			case K_FN: {
				if (sp || curFn >= 0) {
					loadFail(src, line, "FN inside a block or function%s", NULL);
				}
				const char *a = s->arg;
				int         l = 0;
				while (scrIsName(a[l], l == 0)) {
					l++;
				}
				if (l == 0 || l > SCR_NAME_MAX || (a[l] && a[l] != ' ' && a[l] != '\t')) {
					loadFail(src, line, "usage: FN name [$a ...]%s", NULL);
				}
				if (scrFindFn(m, a, l) >= 0) {
					char nm[SCR_NAME_MAX + 1];
					snprintf(nm, sizeof(nm), "%.*s", l, a);
					loadFail(src, line, "function %s defined twice", nm);
				}
				if (scrIsReserved(a, l)) {
					loadFail(src, line, "FN: reserved name%s", NULL);
				}
				scrFn_t *f = scrHostRealloc(m->fns, (m->nFns + 1) * sizeof(scrFn_t));
				if (!f) {
					loadFail(src, line, "out of memory%s", NULL);
				}
				m->fns = f;
				f = &m->fns[m->nFns];
				memset(f, 0, sizeof(*f));
				memcpy(f->name, a, l);
				f->start = idx;
				f->end   = -1;
				/* parameters */
				for (const char *p = scrSkip(a + l); *p;) {
					if (*p != '$' || p[1] == '$') {
						loadFail(src, line, "FN parameters must be $name%s", NULL);
					}
					int pl = 0;
					while (scrIsName(p[1 + pl], pl == 0)) {
						pl++;
					}
					if (pl == 0 || pl > SCR_NAME_MAX) {
						loadFail(src, line, "FN: bad parameter name%s", NULL);
					}
					f->vars = addName(f->vars, p + 1, pl);
					p = scrSkip(p + 1 + pl);
				}
				curFn = m->nFns++;
				s->fn = -1;
				bodyStarted = false;
				break;
			}
			case K_END:
				if (sp) {
					sp--;
					int o = stk[sp].open;
					m->st[o].end = idx;
					if (m->st[o].kind != K_LOOP) {
						m->st[stk[sp].last].jump = idx;
					}
					s->jump = o;
					s->blk  = sp ? stk[sp - 1].open : -1;
				} else if (curFn >= 0) {
					m->fns[curFn].end = idx;
					m->st[m->fns[curFn].start].end = idx;
					s->jump = m->fns[curFn].start;
					curFn = -1;
					bodyStarted = true;		/* main body continues */
				} else {
					loadFail(src, line, "END without IF / LOOP / SWITCH / FN%s", NULL);
				}
				break;
			case K_LABEL: {
				const char *a = s->arg;
				int         l = 0;
				while (scrIsName(a[l], l == 0)) {
					l++;
				}
				if (l == 0 || l > SCR_NAME_MAX || a[l]) {
					loadFail(src, line, "bad label%s", NULL);
				}
				if (scrFindLabel(m, curFn, a, l) >= 0) {
					loadFail(src, line, "label %s defined twice", a);
				}
				scrLabel_t *lb = scrHostRealloc(m->labels, (m->nLabels + 1) * sizeof(scrLabel_t));
				if (!lb) {
					loadFail(src, line, "out of memory%s", NULL);
				}
				m->labels = lb;
				lb = &m->labels[m->nLabels++];
				memcpy(lb->name, a, l);
				lb->name[l] = '\0';
				lb->stmt = idx;
				lb->fn   = curFn;
				break;
			}
			default:
				if (sp && m->st[stk[sp - 1].open].kind == K_SWITCH && stk[sp - 1].last == stk[sp - 1].open) {
					loadFail(src, line, "statement before the first SWITCH label%s", NULL);
				}
				break;
			}
			rest = next;
		}
	}
	if (sp) {
		scrStmt_t *o = &m->st[stk[sp - 1].open];
		loadFail(o->src, o->line, "block without END%s", NULL);
	}
	if (curFn >= 0) {
		scrStmt_t *o = &m->st[m->fns[curFn].start];
		loadFail(o->src, o->line, "FN without END%s", NULL);
	}
	/* in[] texts were copied by addStmt */
	for (int i = 0; i < nIn; i++) {
		scrHostFree(in[i].text);
	}
	scrHostFree(in);
	m->raw  = NULL;
	m->nRaw = 0;

	/* VARS name checks */
	for (int i = 0; i < m->n; i++) {
		scrStmt_t *s = &m->st[i];
		if (s->kind == K_VARS || s->kind == K_FN) {
			continue;
		}
		if (s->fn >= 0 && m->fns[s->fn].hasVars) {
			checkNames(s, m->fns[s->fn].vars);
		} else if (s->fn < 0 && m->mainHasVars) {
			checkNames(s, m->mainVars);
		}
	}
}

/* Literal "file.fn" in CALL / ON / LET = CALL: load that file. */
static void loadCalled(scrMod_t *m)
{
	for (int i = 0; i < m->n; i++) {
		scrStmt_t  *s = &m->st[i];
		const char *a = NULL;
		if (s->kind == K_CALL) {
			a = s->arg;
		} else if (s->kind == K_ON) {
			a = s->arg;
			while (*a && *a != ' ' && *a != '\t') {
				a++;
			}
			a = scrSkip(a);
		} else if (s->kind == K_LET) {
			const char *c = strstr(s->arg, "=");
			if (c) {
				c = scrSkip(c + 1);
				if (scrWordIs(c, "CALL")) {
					a = scrSkip(c + 4);
				}
			}
		}
		if (!a || *a == '$') {
			continue;
		}
		int l = 0;
		while (a[l] && a[l] != ' ' && a[l] != '\t' && a[l] != '.') {
			l++;
		}
		if (a[l] != '.' || l == 0 || l > SCR_FNAME_MAX) {
			continue;
		}
		char fname[SCR_FNAME_MAX + 1];
		memcpy(fname, a, l);
		fname[l] = '\0';
		if (!scrFindModule(fname)) {
			scr.locSrc  = s->src;
			scr.locLine = s->line;
			scrLoadModule(fname);
		}
		scrMod_t *cm = scrFindModule(fname);
		const char *fnm = a + l + 1;
		int fl = 0;
		while (scrIsName(fnm[fl], fl == 0)) {
			fl++;
		}
		if (cm && scrFindFn(cm, fnm, fl) < 0) {
			char nm[64];
			snprintf(nm, sizeof(nm), "%.*s.%.*s", l, a, fl, fnm);
			loadFail(s->src, s->line, "unknown function %s", nm);
		}
	}
}

scrMod_t *scrFindModule(const char *name)
{
	for (int i = 0; i < scr.nMod; i++) {
		if (strcasecmp(scr.mod[i]->name, name) == 0) {
			return scr.mod[i];
		}
	}
	return NULL;
}

scrMod_t *scrLoadModule(const char *name)
{
	if (scr.nMod >= SCR_MOD_MAX) {
		scrFail("too many script files (max %d)", SCR_MOD_MAX);
	}
	scrMod_t *m = scrHostAlloc(sizeof(scrMod_t));
	if (!m) {
		scrFail("out of memory");
	}
	snprintf(m->name, sizeof(m->name), "%s", name);
	scr.mod[scr.nMod++] = m;

	const char *chain[SCR_INSERT_DEPTH + 2];
	size_t      before = scr.loaded;
	scr.loaded = 0;
	loadFile(m, name, 0, chain);
	scr.loaded = before;
	structure(m);
	for (int i = 0; i < scr.nConst; i++) {
		int f = scrFindFn(m, scr.cname[i], strlen(scr.cname[i]));
		if (f >= 0) {
			scrStmt_t *fs = &m->st[m->fns[f].start];
			loadFail(fs->src, fs->line, "function %s has the name of a .CONST", scr.cname[i]);
		}
	}
	loadCalled(m);
	return m;
}

int scrFindFn(scrMod_t *m, const char *name, int len)
{
	for (int i = 0; i < m->nFns; i++) {
		if ((int)strlen(m->fns[i].name) == len && strncasecmp(m->fns[i].name, name, len) == 0) {
			return i;
		}
	}
	return -1;
}

int scrFindLabel(scrMod_t *m, int fn, const char *name, int len)
{
	for (int i = 0; i < m->nLabels; i++) {
		if (m->labels[i].fn == fn && (int)strlen(m->labels[i].name) == len &&
		    strncasecmp(m->labels[i].name, name, len) == 0) {
			return i;
		}
	}
	return -1;
}

void scrUnloadAll(void)
{
	for (int i = 0; i < scr.nMod; i++) {
		scrMod_t *m = scr.mod[i];
		for (int j = 0; j < m->n; j++) {
			scrHostFree(m->st[j].text);
		}
		for (int j = 0; j < m->nFns; j++) {
			scrHostFree(m->fns[j].vars);
		}
		for (int j = 0; j < m->nRaw; j++) {
			scrHostFree(m->raw[j].text);
		}
		scrHostFree(m->raw);
		scrHostFree(m->st);
		scrHostFree(m->fns);
		scrHostFree(m->labels);
		scrHostFree(m->mainVars);
		scrHostFree(m);
	}
	scr.nMod = 0;
	for (int i = 0; i < scr.nSrc; i++) {
		scrHostFree(scr.src[i].text);
		scrHostFree(scr.src[i].lines);
	}
	scr.nSrc = 0;
	for (int i = 0; i < scr.nConst; i++) {
		scrHostFree(scr.cval[i]);
	}
	scr.nConst = 0;
}
