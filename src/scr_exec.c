/* Script execution: frames, statements, events, errors, debugger, console helpers. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "scr_int.h"

scrCtx_t *scrCtxP;
scrBpt_t scrBpts[SCR_BPT_MAX];
int      scrNBpts;

static bool s_stopped;			/* a wait returned "stopped" */

void scrDoCall(scrFrame_t *f, const char *c, int ret, const char *rv);

static const char *const s_srcNames[4] = { "BTN", "IMU", "UI", "TIMER" };

#define F()	(&scr.frames[scr.depth - 1])
#define ST(f)	(&(f)->mod->st[(f)->pc])

/* ================= helpers ================= */

bool scrEnsureCtx(void)
{
	if (!scrCtxP) {
		scrCtxP = scrHostAlloc(sizeof(scrCtx_t));
	}
	return scrCtxP != NULL;
}

int scrEventSource(const char *text)
{
	for (int i = 0; i < 4; i++) {
		size_t l = strlen(s_srcNames[i]);
		if (strncmp(text, s_srcNames[i], l) == 0 && (text[l] == ' ' || text[l] == '\0')) {
			return 1 << i;
		}
	}
	return 0;
}

static int srcIndex(int mask)
{
	for (int i = 0; i < 4; i++) {
		if (mask == (1 << i)) {
			return i;
		}
	}
	return -1;
}

bool scrHasHandler(int mask)
{
	if (!scrCtxP || !scr.active) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if ((mask & (1 << i)) && scr.handlers[i][1] >= 0) {
			return true;
		}
	}
	return false;
}

int scrQuiet(void)
{
	return (scrCtxP && scr.active) ? scr.quiet : 0;
}

const char *scrActiveName(void)
{
	return (scrCtxP && scr.active) ? scr.name : "";
}

/* text after the keyword and ONE space (PRINT / DEBUG keep further spaces) */
static const char *rawArg(const scrStmt_t *s)
{
	const char *p = s->text;
	while (*p && *p != ' ' && *p != '\t') {
		p++;
	}
	if (*p) {
		p++;
	}
	return p;
}

static void setText(scrVar_t *v, const char *t)
{
	scrSetVar(v, scrText(t, strlen(t)));
}

/* target "$name" / "$$name" at *pp -> variable (create). Read-only names fail. */
static scrVar_t *target(const char **pp, bool create)
{
	const char *p = scrSkip(*pp);
	if (*p != '$') {
		scrFail("variable expected");
	}
	bool g = (p[1] == '$');
	p += g ? 2 : 1;
	if (*p == '.') {
		int rl = 0;
		while (scrIsName(p[1 + rl], rl == 0)) {
			rl++;
		}
		scrFail("$.%.*s is read-only", rl, p + 1);
	}
	bool brace = (*p == '{');
	if (brace) {
		p++;
	}
	int l = 0;
	while (scrIsName(p[l], l == 0) || (g && l > 0 && p[l] == '.')) {
		l++;
	}
	if (l == 0) {
		scrFail("bad variable name");
	}
	if (g && memchr(p, '.', l)) {
		scrFail("NV settings are read-only: use NV SET");
	}
	if (l > SCR_NAME_MAX) {
		scrFail("name too long");
	}
	char name[SCR_NAME_MAX + 1];
	memcpy(name, p, l);
	name[l] = '\0';
	p += l;
	if (brace) {
		if (*p != '}') {
			scrFail("missing }");
		}
		p++;
	}
	*pp = p;
	scrVar_t *v = scrFindVar(g, name, create);
	if (!v) {
		scrFail("%s%s is not set", g ? "$$" : "$", name);
	}
	return v;
}

/* ================= frames ================= */

static void freeFrame(scrFrame_t *f)
{
	if (f->locals) {
		for (int i = 0; i < f->nLocals; i++) {
			scrFreeVar(&f->locals[i]);
		}
		scrHostFree(f->locals);
	}
	memset(f, 0, sizeof(*f));
}

static scrFrame_t *pushFrame(scrMod_t *m, int fn, int pc)
{
	int calls = scr.depth;			/* frames beyond the first */
	if (scr.depth > 0 && calls > scr.stackMax) {
		scrFail("stack overflow (.STACK %d)", scr.stackMax);
	}
	if (scr.depth > SCR_STACK_MAX) {
		scrFail("stack overflow");
	}
	scrVar_t *loc = scrHostAlloc(SCR_LOCALS * sizeof(scrVar_t));
	if (!loc) {
		scrFail("out of memory");
	}
	scrFrame_t *f = &scr.frames[scr.depth++];
	memset(f, 0, sizeof(*f));
	f->mod    = m;
	f->fn     = fn;
	f->pc     = pc;
	f->locals = loc;
	return f;
}

/* Call function fi of module m with args. */
static void pushCall(scrMod_t *m, int fi, const char *args, int ret, const char *retVar, bool handler)
{
	scrFn_t    *fn = &m->fns[fi];
	scrStmt_t  *fs = &m->st[fn->start];
	/* parameters from the FN line */
	char        pnames[8][SCR_NAME_MAX + 1];
	int         np = 0;
	const char *p  = fs->arg;
	while (*p && *p != ' ' && *p != '\t') {
		p++;
	}
	for (p = scrSkip(p); *p && np < 8;) {
		int l = 0;
		p++;					/* '$' */
		while (scrIsName(p[l], l == 0)) {
			l++;
		}
		memcpy(pnames[np], p, l);
		pnames[np++][l] = '\0';
		p = scrSkip(p + l);
	}
	int alen = strlen(args);
	if (scrWords(args, alen) < np) {
		scrFail("%s needs %d argument%s", fn->name, np, np == 1 ? "" : "s");
	}
	scrFrame_t *f = pushFrame(m, fi, fn->start + 1);
	f->ret     = ret;
	f->handler = handler;
	if (retVar) {
		snprintf(f->retVar, sizeof(f->retVar), "%s", retVar);
	}
	setText(scrFindVar(false, "arg", true), args);
	for (int i = 0; i < np; i++) {
		int ws, wl;
		scrWord(args, alen, i + 1, &ws, &wl);
		scrSetVar(scrFindVar(false, pnames[i], true), scrText(args + ws, wl));
	}
}

static void popFrame(void)
{
	freeFrame(F());
	scr.depth--;
}

/* RETURN value: back to the caller. Returns false when the run ends. */
static bool doReturn(scrVal_t val)
{
	scrSetVar(&scr.ret, val);
	scrFrame_t *f   = F();
	int         ret = f->ret;
	bool        h   = f->handler;
	char        rv[sizeof(f->retVar)];
	memcpy(rv, f->retVar, sizeof(rv));
	if (scr.depth == 1) {
		return false;				/* main body / system function */
	}
	popFrame();
	if (h) {
		scr.inHandler = false;
	}
	if (ret == RET_LET) {
		const char *p = rv;
		scrSetVar(target(&p, true), val);
	}
	return true;
}

/* ================= commands ================= */

typedef struct {
	bool  capture;			/* GET */
	char *buf;			/* captured text */
	int   n, cap;
	char  last[128];		/* last reply piece (status) */
} cmdCtx_t;

static void cmdCb(void *ctx, const char *text)
{
	cmdCtx_t *c = ctx;
	snprintf(c->last, sizeof(c->last), "%s", text);
	bool status = (strcmp(text, "OK\n") == 0 || strncmp(text, "ERR ", 4) == 0);
	if (c->capture) {
		if (!status) {
			int l = strlen(text);
			if (c->n + l < c->cap) {
				memcpy(c->buf + c->n, text, l);
				c->n += l;
				c->buf[c->n] = '\0';
			}
		}
		return;
	}
	bool err = strncmp(text, "ERR ", 4) == 0;
	if (scr.quiet == 0 || (err && scr.quiet <= 2)) {	/* QUIET 1, 2: ERR only; 3: nothing */
		scrHostPrint(text);
	}
}

/* Argument words equal to a .CONST name -> its value (command name kept). */
static const char *constWords(const char *line)
{
	if (!scr.nConst) {
		return line;
	}
	char *o = scrArena(SCR_LINE_MAX * 4 + 1);
	int   n = 0;
	bool  firstWord = true;
	for (const char *p = line; *p;) {
		if (*p == ' ' || *p == '\t') {
			o[n++] = *p++;
			continue;
		}
		int l = 0;
		while (p[l] && p[l] != ' ' && p[l] != '\t') {
			l++;
		}
		const char *cv;
		if (!firstWord && scrConstFind(p, l, &cv)) {
			int cl = strlen(cv);
			if (n + cl >= SCR_LINE_MAX * 4) {
				scrFail("line too long after substitution");
			}
			memcpy(o + n, cv, cl);
			n += cl;
		} else {
			if (n + l >= SCR_LINE_MAX * 4) {
				scrFail("line too long after substitution");
			}
			memcpy(o + n, p, l);
			n += l;
		}
		firstWord = false;
		p += l;
	}
	o[n] = '\0';
	return o;
}

/* quote char of a raw-text command (ECHO n "..." / FLOG f '...') */
static char rawQuote(const char *line, int skipWords)
{
	const char *p = scrSkip(line);
	for (int i = 0; i < skipWords; i++) {
		while (*p && *p != ' ' && *p != '\t') {
			p++;
		}
		p = scrSkip(p);
	}
	return (*p == '"' || *p == '\'') ? *p : 0;
}

/* Build and run a console command line; returns true if it replied OK. */
static bool runCmd(const char *text, cmdCtx_t *c)
{
	const char *t = constWords(text);
	const char *line;
	if (scrWordIs(t, "ECHO") || scrWordIs(t, "FLOG")) {
		line = scrSubst(t, SUB_RAW, rawQuote(t, 2));	/* the command does quotes / escapes */
	} else {
		line = scrSubst(t, SUB_CMD, 0);
	}
	if (strlen(line) > SCR_LINE_MAX) {
		scrFail("line too long after substitution");
	}
	c->last[0] = '\0';
	scrHostCmd(line, cmdCb, c);
	if (strncmp(c->last, "ERR ", 4) == 0) {
		scr.ok = false;
		snprintf(scr.err, sizeof(scr.err), "%s", c->last + 4);
		char *nl = strchr(scr.err, '\n');
		if (nl) {
			*nl = '\0';
		}
		return false;
	}
	scr.ok     = true;
	scr.err[0] = '\0';
	/* UI switch ends the script */
	if (scrWordIs(line, "UI")) {
		const char *a = scrSkip(line + 2);
		if (*a) {
			scr.exitReq = true;
		}
	}
	return true;
}

/* ================= control flow helpers ================= */

static bool isAncestor(scrMod_t *m, int blk, int of)
{
	/* is block 'blk' the block 'of' or one of its enclosing blocks? (-1 = top) */
	if (blk < 0) {
		return true;
	}
	for (int b = of; b >= 0; b = m->st[b].blk) {
		if (b == blk) {
			return true;
		}
	}
	return false;
}

static void doGoto(scrFrame_t *f, const char *name, int len, int ref)
{
	int li = scrFindLabel(f->mod, f->fn, name, len);
	if (li < 0) {
		scrFail("unknown label %.*s", len, name);
	}
	int tgt = f->mod->labels[li].stmt;
	int tb  = f->mod->st[tgt].blk;
	if (!isAncestor(f->mod, tb, f->mod->st[ref].blk)) {
		scrFail("GOTO %.*s: jumps into a block", len, name);
	}
	while (f->nLoops > 0) {
		int o = f->loops[f->nLoops - 1].opener;
		if (o == tb || isAncestor(f->mod, o, tb)) {
			break;
		}
		f->nLoops--;
	}
	f->pc = tgt;
}

/* end of an IF / SWITCH chain from a branch statement */
static int chainEnd(scrMod_t *m, int t)
{
	while (m->st[t].kind != K_END) {
		t = m->st[t].jump;
	}
	return t;
}

static bool caseMatch(const char *label, scrVal_t v, bool ci)
{
	int         tl;
	const char *t = scrToText(v, &tl);
	double      x;
	bool        isNum = scrToNum(v, &x);
	for (const char *p = label; *p;) {
		int l = 0;
		while (p[l] && p[l] != '|') {
			l++;
		}
		if (l == 1 && p[0] == '*') {
			return true;
		}
		const char *dd = NULL;
		for (int i = 0; i + 1 < l; i++) {
			if (p[i] == '.' && p[i + 1] == '.') {
				dd = p + i;
			}
		}
		double a, b;
		if (dd && isNum && scrParseNum(p, dd - p, &a) && scrParseNum(dd + 2, p + l - dd - 2, &b)) {
			if (x >= a && x <= b) {
				return true;
			}
		} else if (l == tl && (ci ? strncasecmp(p, t, l) == 0 : memcmp(p, t, l) == 0)) {
			return true;
		}
		p += l;
		if (*p == '|') {
			p++;
		}
	}
	return false;
}

static void loopEnd(scrFrame_t *f, scrStmt_t *endSt)
{
	if (f->nLoops == 0 || f->loops[f->nLoops - 1].opener != endSt->jump) {
		scrFail("internal: loop stack");
	}
	scrLoop_t *L = &f->loops[f->nLoops - 1];
	scrStmt_t *o = &f->mod->st[L->opener];
	bool again;
	switch (L->type) {
	case L_TO: {
		const char *p = L->var;
		scrVar_t   *v = target(&p, false);
		double      d;
		if (!scrToNum(scrVarVal(v), &d)) {
			scrFail("LOOP variable is no longer a number");
		}
		scrSetVar(v, scrNum(d + L->step));
		again = (--L->remaining > 0);
		break;
	}
	case L_WHILE:
		again = scrTruth(scrEvalAll(scrSkip(o->arg + 5)));
		break;
	case L_TIMES:
		again = (--L->remaining > 0);
		break;
	default:
		again = true;
		break;
	}
	if (again) {
		f->pc = L->opener + 1;
	} else {
		f->nLoops--;
		f->pc = o->end + 1;
	}
}

static void execLoop(scrFrame_t *f, scrStmt_t *s)
{
	const char *a = s->arg;
	if (f->nLoops >= SCR_BLOCK_MAX) {
		scrFail("too many nested loops");
	}
	scrLoop_t L = { .opener = f->pc };
	if (scrWordIs(a, "FOREVER")) {
		L.type = L_FOREVER;
	} else if (scrWordIs(a, "WHILE")) {
		if (!scrTruth(scrEvalAll(scrSkip(a + 5)))) {
			f->pc = s->end + 1;
			return;
		}
		L.type = L_WHILE;
	} else if (*a == '$' && strchr(a, '=')) {
		const char *p = a;
		const char *vs = p;
		scrVar_t   *v  = target(&p, true);
		snprintf(L.var, sizeof(L.var), "%.*s", (int)(p - vs), vs);
		p = scrSkip(p);
		if (*p != '=') {
			scrFail("usage: LOOP $v = a TO b [STEP n]");
		}
		p++;
		double from = 0, to = 0, step = 1;
		scrVal_t va = scrEval(&p, "TO STEP");
		if (!scrToNum(va, &from)) {
			scrFail("LOOP start must be a number");
		}
		p = scrSkip(p);
		if (!scrWordIs(p, "TO")) {
			scrFail("usage: LOOP $v = a TO b [STEP n]");
		}
		p += 2;
		scrVal_t vb = scrEval(&p, "STEP");
		if (!scrToNum(vb, &to)) {
			scrFail("LOOP end must be a number");
		}
		p = scrSkip(p);
		if (scrWordIs(p, "STEP")) {
			p += 4;
			scrVal_t vs2 = scrEval(&p, NULL);
			if (!scrToNum(vs2, &step) || step == 0 || step != floor(step)) {
				scrFail("STEP must be a whole number, not 0");
			}
			p = scrSkip(p);
		}
		if (*p) {
			scrFail("unexpected \"%.20s\"", p);
		}
		scrSetVar(v, scrNum(from));
		double passes = step > 0 ? (to >= from ? floor((to - from) / step + 1e-9) + 1 : 0)
		                         : (from >= to ? floor((from - to) / -step + 1e-9) + 1 : 0);
		if (passes <= 0) {
			f->pc = s->end + 1;
			return;
		}
		L.type      = L_TO;
		L.step      = step;
		L.remaining = (long)passes;
	} else {
		const char *p = a;
		scrVal_t    n = scrEval(&p, "TIMES");
		p = scrSkip(p);
		if (!scrWordIs(p, "TIMES") || *scrSkip(p + 5)) {
			scrFail("usage: LOOP n TIMES | WHILE e | FOREVER | $v = a TO b");
		}
		double d;
		if (!scrToNum(n, &d)) {
			scrFail("LOOP n TIMES: n must be a number");
		}
		long cnt = (long)trunc(d);
		if (cnt <= 0) {
			f->pc = s->end + 1;
			return;
		}
		L.type      = L_TIMES;
		L.remaining = cnt;
	}
	f->loops[f->nLoops++] = L;
	f->pc++;
}

/* ================= word edits ================= */

static bool isSepC(char c)
{
	return c == ' ' || c == '\t' || c == ':' || c == '|' || c == '\n' || c == '\r';
}

/* "$v[n]" target at *pp -> variable and n (1..words) */
static scrVar_t *indexed(const char **pp, int *n, bool insert)
{
	scrVar_t   *v = target(pp, false);
	const char *p = *pp;
	if (*p != '[') {
		scrFail("expected $v[n]");
	}
	p++;
	scrVal_t ix = scrEval(&p, NULL);
	p = scrSkip(p);
	if (*p != ']') {
		scrFail("missing ]");
	}
	*pp = p + 1;
	double d;
	int    words = v->num ? 1 : scrWords(v->s ? v->s : "", v->len);
	if (!scrToNum(ix, &d) || d != floor(d) || d < 1 || d > words) {
		scrFail("word index must be 1..%d", words);
	}
	(void)insert;
	*n = (int)d;
	return v;
}

static const char *oneWord(scrVal_t v, int *len)
{
	const char *t = scrToText(v, len);
	if (*len == 0) {
		scrFail("the new word is empty");
	}
	for (int i = 0; i < *len; i++) {
		if (isSepC(t[i])) {
			scrFail("the new word contains a separator");
		}
	}
	return t;
}

/* ================= statements ================= */

static scrVal_t letValue(scrVar_t *v, char op, scrVal_t e)
{
	scrVal_t cur = scrVarVal(v);
	double   x, y;
	bool     curEmpty = (!cur.num && cur.len == 0);
	if (op == '+') {
		bool cn = curEmpty || scrToNum(cur, &x);
		if (curEmpty) {
			x = 0;
		}
		if (cn && scrToNum(e, &y)) {
			return scrNum(x + y);
		}
		if (curEmpty) {
			return e;
		}
		int         cl, el;
		const char *ct = scrToText(cur, &cl);
		const char *et = scrToText(e, &el);
		char       *o  = scrArena(cl + el + 2);
		memcpy(o, ct, cl);
		o[cl] = ' ';
		memcpy(o + cl + 1, et, el);
		return scrText(o, cl + el + 1);
	}
	if (curEmpty) {
		x = 0;
	} else if (!scrToNum(cur, &x)) {
		scrFail("%c= needs numbers", op);
	}
	if (!scrToNum(e, &y)) {
		scrFail("%c= needs numbers", op);
	}
	if (op == '/' && y == 0) {
		scrFail("divide by zero");
	}
	return scrNum(op == '-' ? x - y : op == '*' ? x * y : x / y);
}

/* "$name" / "$$name" / "${name}" syntax at *pp, no lookup: advances *pp */
static void targetSyntax(const char **pp)
{
	const char *p = scrSkip(*pp);
	if (*p != '$') {
		scrFail("variable expected");
	}
	bool g = (p[1] == '$');
	p += g ? 2 : 1;
	if (*p == '.') {
		int rl = 0;
		while (scrIsName(p[1 + rl], rl == 0)) {
			rl++;
		}
		scrFail("$.%.*s is read-only", rl, p + 1);
	}
	bool brace = (*p == '{');
	if (brace) {
		p++;
	}
	int l = 0;
	while (scrIsName(p[l], l == 0) || (g && l > 0 && p[l] == '.')) {
		l++;
	}
	if (l == 0) {
		scrFail("bad variable name");
	}
	if (g && memchr(p, '.', l)) {
		scrFail("NV settings are read-only: use NV SET");
	}
	p += l;
	if (brace) {
		if (*p != '}') {
			scrFail("missing }");
		}
		p++;
	}
	*pp = p;
}

static void stLet(scrFrame_t *f, scrStmt_t *s)
{
	const char *p  = s->arg;
	const char *ts = p;
	targetSyntax(&p);
	int  tlen = p - ts;
	bool idx  = (*p == '[');
	if (idx) {
		int depth = 0;
		for (; *p; p++) {
			if (*p == '[') {
				depth++;
			} else if (*p == ']' && --depth == 0) {
				p++;
				break;
			}
		}
	}
	p = scrSkip(p);
	char op = 0;
	if ((p[0] == '+' || p[0] == '-' || p[0] == '*' || p[0] == '/') && p[1] == '=') {
		op = p[0];
		p += 2;
	} else if (p[0] == '=' && p[1] != '=') {
		p++;
	} else {
		scrFail("usage: LET $v = expr");
	}
	p = scrSkip(p);
	char tname[SCR_NAME_MAX + 8];
	snprintf(tname, sizeof(tname), "%.*s", tlen, ts);
	if (!op && !idx && scrWordIs(p, "CALL")) {
		f->pc++;
		scrDoCall(f, scrSkip(p + 4), RET_LET, tname);	/* return value goes to tname */
		return;
	}
	scrVal_t e = scrEvalAll(p);
	if (idx) {
		if (op) {
			scrFail("LET $v[n] takes '=' only");
		}
		const char *q = ts;
		int         n;
		scrVar_t   *v  = indexed(&q, &n, false);
		int         wl;
		const char *w  = oneWord(e, &wl);
		int         tl;
		const char *t  = scrToText(scrVarVal(v), &tl);
		int         ws, ol;
		scrWord(t, tl, n, &ws, &ol);
		char *o = scrArena(tl - ol + wl + 1);
		memcpy(o, t, ws);
		memcpy(o + ws, w, wl);
		memcpy(o + ws + wl, t + ws + ol, tl - ws - ol);
		scrSetVar(v, scrText(o, tl - ol + wl));
	} else {
		const char *q = ts;
		scrVar_t   *v = target(&q, !op);	/* '=' creates; += etc need it set */
		scrSetVar(v, op ? letValue(v, op, e) : e);
	}
	f->pc++;
}

void scrDoCall(scrFrame_t *f, const char *c, int ret, const char *rv)
{
	int l = 0;
	while (c[l] && c[l] != ' ' && c[l] != '\t') {
		l++;
	}
	scrMod_t   *m  = f->mod;
	const char *fn = c;
	int         fl = l;
	const char *dot = memchr(c, '.', l);
	if (dot) {
		char mn[SCR_FNAME_MAX + 1];
		snprintf(mn, sizeof(mn), "%.*s", (int)(dot - c), c);
		m = scrFindModule(mn);
		if (!m) {
			scr.locSrc = -1;
			m = scrLoadModule(mn);
		}
		fn = dot + 1;
		fl = l - (dot + 1 - c);
	}
	int fi = scrFindFn(m, fn, fl);
	if (fi < 0) {
		scrFail("unknown function %.*s", l, c);
	}
	const char *args = scrSubst(scrSkip(c + l), SUB_PLAIN, 0);
	pushCall(m, fi, args, ret, rv, false);
}

static void pushHandler(const char *ev, int ret)
{
	int si = srcIndex(scrEventSource(ev));
	scrMod_t *m  = scr.mod[scr.handlers[si][0]];
	int       fi = scr.handlers[si][1];
	const char *arg = ev;
	while (*arg && *arg != ' ') {
		arg++;
	}
	arg = scrSkip(arg);
	pushCall(m, fi, arg, ret, NULL, true);
	scr.inHandler = true;
}

/* ================= data files (FWRITE FREAD FTELL FSEEK FCLOSE, VARS SAVE / LOAD) ================= */

/* file name word at *pp (letters, digits, '_', max 31; no extension) */
static void fileWord(const char **pp, char *name)
{
	const char *p = scrSkip(*pp);
	int         l = 0;
	while (p[l] && p[l] != ' ' && p[l] != '\t') {
		char c = p[l];
		if (!scrIsName(c, false)) {
			scrFail("file: letters, digits, '_' only (no extension)");
		}
		l++;
	}
	if (l == 0) {
		scrFail("file name expected");
	}
	if (l > SCR_FNAME_MAX) {
		scrFail("file: max %d characters", SCR_FNAME_MAX);
	}
	memcpy(name, p, l);
	name[l] = '\0';
	*pp = scrSkip(p + l);
}

/* position slot of name.txt (opened at 1 on first use) */
static long *filePos(const char *name)
{
	for (int i = 0; i < scr.nFiles; i++) {
		if (strcasecmp(scr.files[i].name, name) == 0) {
			return &scr.files[i].pos;
		}
	}
	if (scr.nFiles >= scr.filesMax) {
		scrFail("too many files open (max %d): FCLOSE one", scr.filesMax);
	}
	snprintf(scr.files[scr.nFiles].name, sizeof(scr.files[0].name), "%s", name);
	scr.files[scr.nFiles].pos = 1;
	return &scr.files[scr.nFiles++].pos;
}

/* size of fname (with extension); missing: -1 (fails if mustExist) */
static long fileSize(const char *fname, bool *ro, bool mustExist)
{
	const char *err = NULL;
	bool        r   = false;
	long        n   = scrHostFileSize(fname, &r, &err);
	if (n < 0 && err) {
		scrFail("%s: %s", fname, err);
	}
	if (n < 0 && mustExist) {
		scrFail("%s: no such file", fname);
	}
	if (ro) {
		*ro = r;
	}
	return n;
}

/* FSEEK file TAG text: from the start of the file, the first line equal to
 * text (PRINT text rules; trailing spaces, tabs and CR ignored on both
 * sides, case-sensitive). Returns the position of the line after it. */
static long seekTag(const char *fname, const char *arg)
{
	const char *t   = scrUnquote(arg);
	const char *sub = scrSubst(t, SUB_TEXT, 0);
	char       *tag = scrArenaStr(sub, strlen(sub));
	scrUnescape(tag);
	int tl = strlen(tag);
	while (tl > 0 && (tag[tl - 1] == ' ' || tag[tl - 1] == '\t' || tag[tl - 1] == '\r')) {
		tag[--tl] = '\0';
	}
	if (tl == 0) {
		scrFail("usage: FSEEK file TAG text");
	}

	enum { BLK = 1024 };
	char       *blk  = scrArena(BLK);
	char       *lb   = scrArena(tl + 1);	/* first tl + 1 chars of the line */
	long        off  = 0;			/* file offset of blk[0] */
	int         len  = 0;			/* chars of the current line so far */
	int         trim = 0;			/* its length without trailing blanks */
	const char *err  = NULL;

	for (;;) {
		int got = scrHostFileRead(fname, off, blk, BLK, &err);
		if (got < 0) {
			scrFail("%s: %s", fname, err ? err : "read failed");
		}
		for (int i = 0; i <= got; i++) {
			bool end = (i == got);
			if (end && got == BLK) {
				break;				/* more to read */
			}
			char c = end ? '\n' : blk[i];
			if (c == '\n') {
				if (trim == tl && memcmp(lb, tag, tl) == 0) {
					return off + i + 2;	/* 1-based: the byte after '\n' */
				}
				if (end) {
					scrFail("no tag %s", tag);
				}
				len  = 0;
				trim = 0;
				continue;
			}
			if (len <= tl) {
				lb[len] = c;
			}
			len++;
			if (c != ' ' && c != '\t' && c != '\r') {
				trim = len;
			}
		}
		off += got;
	}
}

static void stFile(scrFrame_t *f, scrStmt_t *s)
{
	const char *p = s->arg;
	char        name[SCR_FNAME_MAX + 1], fname[SCR_FNAME_MAX + 8];
	scrVar_t   *v = NULL;

	if (s->kind == K_FREAD || s->kind == K_FTELL) {
		v = target(&p, true);
	}
	fileWord(&p, name);
	snprintf(fname, sizeof(fname), "%s.txt", name);

	switch (s->kind) {
	case K_FCLOSE:
		if (*p) {
			scrFail("usage: FCLOSE file");
		}
		for (int i = 0; i < scr.nFiles; i++) {
			if (strcasecmp(scr.files[i].name, name) == 0) {
				scr.files[i] = scr.files[--scr.nFiles];
				break;
			}
		}
		break;

	case K_FTELL:
		if (*p) {
			scrFail("usage: FTELL $v file");
		}
		scrSetVar(v, scrNum((double)*filePos(name)));
		break;

	case K_FSEEK: {
		long *pos  = filePos(name);
		long  size = fileSize(fname, NULL, false);
		long  np;
		double d = 0;
		if (size < 0) {
			size = 0;
		}
		if (scrWordIs(p, "START") && !*scrSkip(p + 5)) {
			np = 1;
		} else if (scrWordIs(p, "END")) {
			const char *n = scrSkip(p + 3);
			if (*n && (!scrToNum(scrEvalAll(n), &d) || d != floor(d))) {
				scrFail("FSEEK END n: n must be a whole number");
			}
			np = size + 1 - (long)d;
		} else if (scrWordIs(p, "SET") || scrWordIs(p, "OFFS")) {
			bool set = scrWordIs(p, "SET");
			if (!scrToNum(scrEvalAll(scrSkip(p + (set ? 3 : 4))), &d) || d != floor(d)) {
				scrFail("FSEEK %s n: n must be a whole number", set ? "SET" : "OFFS");
			}
			np = set ? (long)d : *pos + (long)d;
		} else if (scrWordIs(p, "TAG")) {
			np = seekTag(fname, scrSkip(p + 3));
		} else {
			scrFail("usage: FSEEK file START | END [n] | SET n | OFFS n | TAG text");
		}
		*pos = np < 1 ? 1 : np > size + 1 ? size + 1 : np;
		break;
	}

	case K_FREAD: {
		long *pos = filePos(name);
		fileSize(fname, NULL, true);
		bool  eol = false;
		double n  = SCR_FREAD_MAX;
		if (scrWordIs(p, "EOL") && !*scrSkip(p + 3)) {
			eol = true;
		} else if (*p) {
			if (!scrToNum(scrEvalAll(p), &n) || n != floor(n) || n < 1 || n > SCR_FREAD_MAX) {
				scrFail("FREAD n: n must be 1..%d (or EOL)", SCR_FREAD_MAX);
			}
		} else {
			scrFail("usage: FREAD $v file n|EOL");
		}
		char        buf[SCR_FREAD_MAX + 1];
		const char *err = NULL;
		int         got = scrHostFileRead(fname, *pos - 1, buf, (int)n, &err);
		if (got < 0) {
			scrFail("%s: %s", fname, err ? err : "read failed");
		}
		int keep = got, used = got;
		if (eol) {
			for (int i = 0; i < got; i++) {
				if (buf[i] == '\n') {
					keep = i;
					used = i + 1;
					break;
				}
			}
			if (keep > 0 && buf[keep - 1] == '\r') {
				keep--;
			}
		}
		for (int i = 0; i < keep; i++) {
			if ((unsigned char)buf[i] >= 0x80 || buf[i] == '\0') {
				buf[i] = '?';			/* text only */
			}
		}
		scrSetVar(v, scrText(buf, keep));
		*pos += used;
		if (eol && got == 0) {
			scr.ok = false;
			scrFail("EOF");
		}
		if (!eol && got < (int)n) {
			scr.ok = false;
			scrFail("over-read %d", (int)n - got);
		}
		scr.ok = true;
		break;
	}

	case K_FWRITE: {
		long *pos = filePos(name);
		bool  ro  = false;
		fileSize(fname, &ro, false);
		if (ro) {
			scrFail("%s is read-only", fname);
		}
		const char *t   = scrUnquote(p);
		const char *sub = scrSubst(t, SUB_TEXT, 0);
		char       *o   = scrArenaStr(sub, strlen(sub));
		scrUnescape(o);
		int         l   = strlen(o);
		const char *e   = scrHostFileWrite(fname, *pos - 1, o, l, false);
		if (e) {
			scrFail("%s: %s", fname, e);
		}
		*pos += l;
		break;
	}

	default:
		break;
	}
	f->pc++;
}

/* VARS SAVE name [$a $$b ...]  /  VARS LOAD name [$a $$b ... | ALL] */
static void stVarsIo(scrFrame_t *f, scrStmt_t *s)
{
	const char *p    = s->arg;
	bool        save = scrWordIs(p, "SAVE");
	char        name[SCR_FNAME_MAX + 1], fname[SCR_FNAME_MAX + 8];
	p = scrSkip(p + 4);
	fileWord(&p, name);
	snprintf(fname, sizeof(fname), "%s.var", name);

	if (save) {
		int   cap = 16384, n = 0;
		char *o   = scrArena(cap);
		/* one variable as "$name=value" / "$$name=..." */
		#define PUTV(pre, var) do { \
			const scrVar_t *vv = (var); \
			char nb[40]; \
			if (n + SCR_VAL_MAX * 2 + 48 >= cap) scrFail("too much to save"); \
			if (vv->num) { snprintf(nb, sizeof(nb), "%.17g", vv->n); \
				n += snprintf(o + n, cap - n, "%s%s=%s\n", pre, vv->name, nb); } \
			else { n += snprintf(o + n, cap - n, "%s%s=\"", pre, vv->name); \
				for (int k = 0; k < vv->len; k++) { char c = vv->s[k]; \
					if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = c; } \
					else if (c == '\n') { o[n++] = '\\'; o[n++] = 'n'; } \
					else if (c == '\r') { o[n++] = '\\'; o[n++] = 'r'; } \
					else o[n++] = c; } \
				o[n++] = '"'; o[n++] = '\n'; } \
		} while (0)
		if (!*p) {
			for (int i = 0; i < scr.nGlobals; i++) {
				PUTV("$$", &scr.globals[i]);		/* no list: globals only */
			}
		} else {
			while (*p) {
				const char *q = p;
				bool        g = (q[0] == '$' && q[1] == '$');
				scrVar_t   *var = target(&p, false);
				(void)q;
				PUTV(g ? "$$" : "$", var);
				p = scrSkip(p);
			}
		}
		#undef PUTV
		const char *e = scrHostFileWrite(fname, 0, o, n, true);
		if (e) {
			scrFail("%s: %s", fname, e);
		}
		f->pc++;
		return;
	}

	/* LOAD */
	bool all = !*p || (scrWordIs(p, "ALL") && !*scrSkip(p + 3));
	long size = fileSize(fname, NULL, true);
	if (size > 32768) {
		scrFail("%s: too big", fname);
	}
	char       *buf = scrArena(size + 1);
	const char *err = NULL;
	int         got = scrHostFileRead(fname, 0, buf, (int)size, &err);
	if (got < 0) {
		scrFail("%s: %s", fname, err ? err : "read failed");
	}
	buf[got] = '\0';
	/* list names that must be found */
	const char *list = all ? NULL : p;
	int         want = 0, found = 0;
	if (list) {
		for (const char *q = list; *q; want++) {
			targetSyntax(&q);
			q = scrSkip(q);
		}
	}
	char *line = buf;
	int   ln   = 0;
	while (*line) {
		char *e = strchr(line, '\n');
		if (e) {
			*e = '\0';
		}
		ln++;
		char *r = line + strlen(line);
		if (r > line && r[-1] == '\r') {
			*--r = '\0';
		}
		if (*line) {
			bool  g  = (line[0] == '$' && line[1] == '$');
			char *nm = line + (g ? 2 : 1);
			char *eq = strchr(line, '=');
			if (line[0] != '$' || !eq || eq == nm) {
				scrFail("%s line %d: expected $name=value", fname, ln);
			}
			*eq = '\0';
			if (strlen(nm) > SCR_NAME_MAX) {
				scrFail("%s line %d: name too long", fname, ln);
			}
			bool take = all;
			if (!all) {
				for (const char *q = list; *q;) {
					const char *st = q;
					targetSyntax(&q);
					bool qg = (st[1] == '$');
					const char *qn = st + (qg ? 2 : 1);
					if (*qn == '{') {
						qn++;
					}
					int ql = 0;
					while (scrIsName(qn[ql], ql == 0)) {
						ql++;
					}
					if (qg == g && (int)strlen(nm) == ql && strncasecmp(qn, nm, ql) == 0) {
						take = true;
						found++;
						break;
					}
					q = scrSkip(q);
				}
			}
			if (take) {
				if (!scrNameAllowed(f->mod, f->fn, nm, g)) {
					scrFail("%s%s is not in VARS", g ? "$$" : "$", nm);
				}
				char *val = eq + 1;
				scrVal_t vv;
				if (*val == '"') {
					char *d = val, *sp = val + 1;
					while (*sp && *sp != '"') {
						if (*sp == '\\' && sp[1]) {
							sp++;
							*d++ = (*sp == 'n') ? '\n' : (*sp == 'r') ? '\r' : *sp;
						} else {
							*d++ = *sp;
						}
						sp++;
					}
					if (*sp != '"') {
						scrFail("%s line %d: missing closing quote", fname, ln);
					}
					vv = scrText(val, d - val);
				} else {
					double dn;
					if (!scrParseNum(val, strlen(val), &dn)) {
						scrFail("%s line %d: bad number", fname, ln);
					}
					vv = scrNum(dn);
				}
				scrSetVar(scrFindVar(g, nm, true), vv);
			}
		}
		if (!e) {
			break;
		}
		line = e + 1;
	}
	if (!all && found < want) {
		/* name the first one missing */
		for (const char *q = list; *q;) {
			const char *st = q;
			targetSyntax(&q);
			char tn[SCR_NAME_MAX + 4];
			snprintf(tn, sizeof(tn), "%.*s", (int)(q - st), st);
			bool seen = false;
			for (char *l2 = buf; l2 < buf + got; l2 += strlen(l2) + 1) {
				if (strcasecmp(l2, tn) == 0) {
					seen = true;
				}
			}
			if (!seen) {
				scrFail("%s not in %s", tn, fname);
			}
			q = scrSkip(q);
		}
	}
	f->pc++;
}

/* Run one statement. Advances / sets pc. */
static void execStmt(scrFrame_t *f, scrStmt_t *s)
{
	scrMod_t *m = f->mod;
	char      buf[SCR_VAL_MAX + 1];

	switch (s->kind) {
	case K_STACK:
	case K_FILES:
	case K_CONST:
	case K_LABEL:
		f->pc++;
		break;

	case K_VARS:
		for (const char *p = s->arg; *p; p = scrSkip(p)) {
			bool g = (p[1] == '$');
			p += g ? 2 : 1;
			int l = 0;
			while (scrIsName(p[l], l == 0)) {
				l++;
			}
			char name[SCR_NAME_MAX + 1];
			snprintf(name, sizeof(name), "%.*s", l, p);
			scrVar_t *v = scrFindVar(g, name, false);
			if (!v) {
				setText(scrFindVar(g, name, true), "");
			} else if (!g) {
				setText(v, "");
			}
			p += l;
		}
		f->pc++;
		break;

	case K_CMD: {
		cmdCtx_t c = { 0 };
		bool     fstat = scrWordIs(s->text, "FSTAT");	/* reply goes to $.ret */
		if (fstat) {
			c.capture = true;
			c.cap     = 64;
			c.buf     = scrArena(c.cap + 1);
			c.buf[0]  = '\0';
		}
		if (!runCmd(s->text, &c)) {
			scrFail("%s", scr.err);
		}
		if (fstat) {
			int l = c.n;
			while (l > 0 && (c.buf[l - 1] == '\n' || c.buf[l - 1] == '\r')) {
				l--;
			}
			scrSetVar(&scr.ret, scrText(c.buf, l));
		}
		f->pc++;
		break;
	}

	case K_GET: {
		const char *p = s->arg;
		scrVar_t   *v = target(&p, true);
		p = scrSkip(p);
		if (*p != '=') {
			scrFail("usage: GET $v = command");
		}
		p = scrSkip(p + 1);
		cmdCtx_t c = { .capture = true, .cap = SCR_VAL_MAX * 2 };
		c.buf = scrArena(c.cap + 1);
		c.buf[0] = '\0';
		runCmd(p, &c);
		/* lines joined with '|' */
		int n = c.n;
		while (n > 0 && (c.buf[n - 1] == '\n' || c.buf[n - 1] == '\r')) {
			n--;
		}
		for (int i = 0; i < n; i++) {
			if (c.buf[i] == '\n') {
				c.buf[i] = '|';
			} else if (c.buf[i] == '\r') {
				c.buf[i] = ' ';
			}
		}
		scrSetVar(v, scrText(c.buf, n));
		f->pc++;
		break;
	}

	case K_LET:
		stLet(f, s);
		break;

	case K_INCR: {
		const char *p = s->text;
		scrVar_t   *v = target(&p, false);
		scrVal_t    cur = scrVarVal(v);
		double      x = 0;
		if (!(!cur.num && cur.len == 0) && !scrToNum(cur, &x)) {
			scrFail("++ / -- need a number");
		}
		p = scrSkip(p);
		scrSetVar(v, scrNum(p[0] == '+' ? x + 1 : x - 1));
		f->pc++;
		break;
	}

	case K_DEL: {
		const char *p = s->arg;
		int         n;
		scrVar_t   *v = indexed(&p, &n, false);
		if (*scrSkip(p)) {
			scrFail("usage: DEL $v[n]");
		}
		int         tl;
		const char *t = scrToText(scrVarVal(v), &tl);
		int         ws, wl;
		scrWord(t, tl, n, &ws, &wl);
		int b = ws, e = ws + wl;
		if (n == 1) {
			while (e < tl && isSepC(t[e])) {
				e++;
			}
		} else {
			while (b > 0 && isSepC(t[b - 1])) {
				b--;
			}
		}
		char *o = scrArena(tl + 1);
		memcpy(o, t, b);
		memcpy(o + b, t + e, tl - e);
		scrSetVar(v, scrText(o, tl - (e - b)));
		f->pc++;
		break;
	}

	case K_INS: {
		const char *p = s->arg;
		int         n;
		scrVar_t   *v = indexed(&p, &n, true);
		p = scrSkip(p);
		if (*p != '=') {
			scrFail("usage: INS $v[n] = expr");
		}
		scrVal_t    e  = scrEvalAll(p + 1);
		int         wl;
		const char *w  = oneWord(e, &wl);
		int         tl;
		const char *t  = scrToText(scrVarVal(v), &tl);
		int         ws, ol;
		scrWord(t, tl, n, &ws, &ol);
		char *o = scrArena(tl + wl + 2);
		memcpy(o, t, ws);
		memcpy(o + ws, w, wl);
		o[ws + wl] = ' ';
		memcpy(o + ws + wl + 1, t + ws, tl - ws);
		scrSetVar(v, scrText(o, tl + wl + 1));
		f->pc++;
		break;
	}

	case K_IF:
		if (scrTruth(scrEvalAll(s->arg))) {
			f->pc++;
		} else {
			int t = s->jump;
			for (;;) {
				scrStmt_t *b = &m->st[t];
				if (b->kind == K_ELIF) {
					f->pc = t;		/* errors point at the ELIF */
					if (scrTruth(scrEvalAll(b->arg))) {
						f->pc = t + 1;
						break;
					}
					t = b->jump;
				} else {
					f->pc = t + 1;		/* ELSE or END */
					break;
				}
			}
		}
		break;

	case K_ELIF:
	case K_ELSE:
	case K_CASE:
		f->pc = chainEnd(m, f->pc) + 1;		/* end of the branch that ran */
		break;

	case K_SWITCH: {
		const char *p  = s->arg;
		bool        ci = (*p == '~');
		if (ci) {
			p++;
		}
		scrVal_t v = scrEvalAll(p);
		int      t = s->jump;
		while (m->st[t].kind == K_CASE) {
			if (caseMatch(m->st[t].text, v, ci)) {
				f->pc = t + 1;
				return;
			}
			t = m->st[t].jump;
		}
		f->pc = t + 1;
		break;
	}

	case K_LOOP:
		execLoop(f, s);
		break;

	case K_END: {
		scrStmt_t *o = &m->st[s->jump];
		if (o->kind == K_FN) {
			if (!doReturn(scrText("", 0))) {
				scr.exitReq = true;
			}
		} else if (o->kind == K_LOOP) {
			loopEnd(f, s);
		} else {
			f->pc++;
		}
		break;
	}

	case K_BREAK:
	case K_NEXT: {
		const char *a = s->arg;
		int         k;				/* loop entry */
		if (*a == '$' && s->kind == K_NEXT) {
			k = -1;
			int l = strlen(a);
			for (int i = f->nLoops - 1; i >= 0; i--) {
				if (f->loops[i].type == L_TO && (int)strlen(f->loops[i].var) == l &&
				    strncasecmp(f->loops[i].var, a, l) == 0) {
					k = i;
					break;
				}
			}
			if (k < 0) {
				scrFail("NEXT %s: no LOOP counts it", a);
			}
		} else {
			double d = 1;
			if (*a && (!scrToNum(scrEvalAll(a), &d) || d < 1 || d != floor(d))) {
				scrFail("%s n: n must be 1 or more", s->kind == K_BREAK ? "BREAK" : "NEXT");
			}
			if (d > f->nLoops) {
				scrFail("%s %d: only %d loop%s", s->kind == K_BREAK ? "BREAK" : "NEXT", (int)d, f->nLoops,
				        f->nLoops == 1 ? "" : "s");
			}
			k = f->nLoops - (int)d;
		}
		int opener = f->loops[k].opener;
		if (s->kind == K_BREAK) {
			f->nLoops = k;
			f->pc     = m->st[opener].end + 1;
		} else {
			f->nLoops = k + 1;
			f->pc     = m->st[opener].end;	/* its END runs the next pass */
		}
		break;
	}

	case K_GOTO: {
		const char *a = s->arg;
		int         l = 0;
		while (scrIsName(a[l], l == 0)) {
			l++;
		}
		if (!l || *scrSkip(a + l)) {
			scrFail("usage: GOTO label");
		}
		doGoto(f, a, l, f->pc);
		break;
	}

	case K_EXIT:
		scr.exitReq = true;
		f->pc++;
		break;

	case K_FN:
		f->pc = s->end + 1;			/* skipped: runs only when called */
		break;

	case K_RETURN: {
		scrVal_t v = *s->arg ? scrEvalAll(s->arg) : scrText("", 0);
		if (!doReturn(v)) {
			scr.exitReq = true;
		}
		break;
	}

	case K_CALL:
		if (!*s->arg) {
			scrFail("usage: CALL name [args]");
		}
		f->pc++;
		scrDoCall(f, s->arg, RET_NONE, NULL);
		break;

	case K_ON: {
		const char *a  = s->arg;
		int         sl = 0;
		while (a[sl] && a[sl] != ' ' && a[sl] != '\t') {
			sl++;
		}
		int si = -1;
		for (int i = 0; i < 4; i++) {
			if ((int)strlen(s_srcNames[i]) == sl && strncasecmp(a, s_srcNames[i], sl) == 0) {
				si = i;
			}
		}
		const char *h = scrSkip(a + sl);
		if (si < 0 || !*h) {
			scrFail("usage: ON BTN|IMU|UI|TIMER function|OFF");
		}
		if (scrWordIs(h, "OFF")) {
			scr.handlers[si][0] = scr.handlers[si][1] = -1;
		} else {
			int         l   = strlen(h);
			const char *dot = memchr(h, '.', l);
			scrMod_t   *hm  = m;
			const char *fn  = h;
			int         fl  = l;
			if (dot) {
				char mn[SCR_FNAME_MAX + 1];
				snprintf(mn, sizeof(mn), "%.*s", (int)(dot - h), h);
				hm = scrFindModule(mn);
				if (!hm) {
					hm = scrLoadModule(mn);
				}
				fn = dot + 1;
				fl = l - (dot + 1 - h);
			}
			int fi = scrFindFn(hm, fn, fl);
			if (fi < 0) {
				scrFail("unknown function %s", h);
			}
			int mi = 0;
			while (scr.mod[mi] != hm) {
				mi++;
			}
			scr.handlers[si][0] = mi;
			scr.handlers[si][1] = fi;
		}
		f->pc++;
		break;
	}

	case K_ONERROR: {
		const char *a = s->arg;
		if (scrWordIs(a, "STOP")) {
			scr.onerr = 0;
		} else if (scrWordIs(a, "CONT")) {
			scr.onerr = 1;
		} else if (scrWordIs(a, "GOTO")) {
			const char *l = scrSkip(a + 4);
			int         n = 0;
			while (scrIsName(l[n], n == 0)) {
				n++;
			}
			if (!n || n > SCR_NAME_MAX || scrFindLabel(m, f->fn, l, n) < 0) {
				scrFail("ONERROR GOTO: unknown label");
			}
			scr.onerr = 2;
			snprintf(scr.onerrLabel, sizeof(scr.onerrLabel), "%.*s", n, l);
			scr.onerrDepth = scr.depth;
		} else {
			scrFail("usage: ONERROR STOP|CONT|GOTO label");
		}
		f->pc++;
		break;
	}

	case K_QUIET:
	case K_TRACE: {
		double d;
		int    max = s->kind == K_QUIET ? 3 : 1;
		if (!scrToNum(scrEvalAll(s->arg), &d) || d < 0 || d > max || d != floor(d)) {
			scrFail(s->kind == K_QUIET ? "usage: QUIET 0|1|2|3" : "usage: TRACE 0|1");
		}
		if (s->kind == K_QUIET) {
			scr.quiet = (int)d;
		} else {
			scr.trace = (int)d;
		}
		f->pc++;
		break;
	}

	case K_PRINT:
	case K_DEBUG: {
		const char *t   = scrUnquote(rawArg(s));
		const char *sub = scrSubst(t, SUB_TEXT, 0);
		char       *o   = scrArenaStr(sub, strlen(sub));
		scrUnescape(o);
		if (s->kind == K_PRINT) {
			int   l = strlen(o);
			char *pl = scrArena(l + 2);
			memcpy(pl, o, l);
			pl[l]     = '\n';
			pl[l + 1] = '\0';
			scrHostPrint(pl);
		} else {
			scrHostDebug(o);
		}
		f->pc++;
		break;
	}

	case K_RNG: {
		const char *a = s->arg;
		if (scrWordIs(a, "TRNG")) {
			scr.prng = false;
		} else if (scrWordIs(a, "PRNG")) {
			scr.prng = true;
			const char *sd = scrSkip(a + 4);
			if (*sd) {
				double d;
				if (!scrToNum(scrEvalAll(sd), &d)) {
					scrFail("RNG PRNG seed must be a number");
				}
				scrRngSeed((uint32_t)(int64_t)d);
			}
		} else {
			scrFail("usage: RNG TRNG | RNG PRNG [seed]");
		}
		f->pc++;
		break;
	}

	case K_TIMER: {
		const char *p  = s->arg;
		int         nl = 0;
		while (p[nl] && p[nl] != ' ' && p[nl] != '\t') {
			nl++;
		}
		scrVal_t    n  = scrEvalAll(scrArenaStr(p, nl));
		scrVal_t    ms = scrEvalAll(scrSkip(p + nl));
		double      dn, dm;
		if (!scrToNum(n, &dn) || dn < 1 || dn > SCR_TIMERS || dn != floor(dn) ||
		    !scrToNum(ms, &dm) || dm < 0 || dm > 86400000) {
			scrFail("usage: TIMER n ms (n 1..%d, ms 0 = off)", SCR_TIMERS);
		}
		scrHostTimer((int)dn, (int)dm);
		f->pc++;
		break;
	}

	case K_WAIT: {
		const char *a = s->arg;
		if (scrWordIs(a, "EVENT")) {
			if (scr.inHandler) {
				scrFail("WAIT EVENT inside an event handler");
			}
			double      ms = -1;
			const char *t  = scrSkip(a + 5);
			if (*t && (!scrToNum(scrEvalAll(t), &ms) || ms < 0)) {
				scrFail("usage: WAIT EVENT [ms]");
			}
			int r = scrHostEvent(SCR_SRC_ALL, buf, sizeof(buf), (int)ms);
			f->pc++;
			if (r < 0) {
				s_stopped = true;
			} else if (r == 0) {
				setText(&scr.event, "");
				setText(&scr.ret, "");
			} else {
				setText(&scr.event, buf);
				int si = srcIndex(scrEventSource(buf));
				if (si >= 0 && scr.handlers[si][1] >= 0) {
					pushHandler(buf, RET_WAITEV);
				} else {
					setText(&scr.ret, "");
				}
			}
		} else {
			double ms;
			if (!scrToNum(scrEvalAll(a), &ms) || ms < 0 || ms > 86400000) {
				scrFail("usage: WAIT ms | WAIT EVENT [ms]");
			}
			f->pc++;
			if (!scrHostSleep((int)ms)) {
				s_stopped = true;
			}
		}
		break;
	}

	case K_INPUT: {
		const char *p = s->arg;
		scrVar_t   *v = target(&p, true);
		double      ms = -1;
		p = scrSkip(p);
		if (*p && (!scrToNum(scrEvalAll(p), &ms) || ms < 0)) {
			scrFail("usage: INPUT $v [ms]");
		}
		int r = scrHostInput(buf, sizeof(buf), (int)ms);
		f->pc++;
		if (r == -2) {
			s_stopped = true;
		} else if (r == 1) {
			setText(v, buf);
			scr.ok = true;
		} else {
			setText(v, "");
			scr.ok = false;
		}
		break;
	}

	case K_BPT:
		scr.pauseNext = true;
		f->pc++;
		break;

	case K_ACC:
	case K_GYR: {
		const char *p = s->arg;
		scrVar_t   *v[3];
		if (!*p) {
			scrFail("%s needs $x $y $z in a script", s->kind == K_ACC ? "ACC" : "GYR");
		}
		for (int i = 0; i < 3; i++) {
			v[i] = target(&p, true);
		}
		if (*scrSkip(p)) {
			scrFail("usage: %s $x $y $z", s->kind == K_ACC ? "ACC" : "GYR");
		}
		double      d[3];
		const char *e = scrHostImu(s->kind == K_GYR, d);
		if (e) {
			scrFail("%s", e);
		}
		for (int i = 0; i < 3; i++) {
			scrSetVar(v[i], scrNum(d[i]));
		}
		f->pc++;
		break;
	}

	case K_FWRITE:
	case K_FREAD:
	case K_FTELL:
	case K_FSEEK:
	case K_FCLOSE:
		stFile(f, s);
		break;

	case K_VARSIO:
		stVarsIo(f, s);
		break;

	case K_CONSOLE: {
		int l = 0;
		while (s->text[l] && s->text[l] != ' ' && s->text[l] != '\t') {
			l++;
		}
		scrFail("%.*s is a console command (not allowed in a script)", l, s->text);
	}

	case K_PAUSE:
		scrFail("PAUSE is a console command: use BPT!");

	default:
		scrFail("internal: statement kind %d", s->kind);
	}
}

/* ================= run loop ================= */

static bool bptAt(scrStmt_t *s)
{
	const char *fn = scrSrcName(s->src);
	for (int i = 0; i < scrNBpts; i++) {
		if (scrBpts[i].line == s->line && strcasecmp(scrBpts[i].file, fn) == 0) {
			return true;
		}
	}
	return false;
}

static bool shouldPause(scrStmt_t *s)
{
	if (scr.resumed) {
		scr.resumed = false;
		return false;
	}
	if (scr.pauseNext || bptAt(s)) {
		return true;
	}
	switch (scr.stepMode) {
	case SCR_GO_STEP: return scr.depth <= scr.stepDepth;
	case SCR_GO_IN:   return true;
	case SCR_GO_OUT:  return scr.depth < scr.stepDepth;
	default:          return false;
	}
}

static void report(int src, int line, const char *reason)
{
	char msg[300];
	if (src >= 0) {
		snprintf(msg, sizeof(msg), "<<SCRIPT %s ERR %s:%d: %s>>\n", scr.name, scrSrcName(src), line, reason);
	} else {
		snprintf(msg, sizeof(msg), "<<SCRIPT %s ERR %s>>\n", scr.name, reason);
	}
	scrHostNotify(msg);
}

/* ONERROR GOTO from statement ref of the current frame */
static bool gotoSafe(int ref, scrStmt_t *s)
{
	if (setjmp(scr.jb)) {
		report(s->src, s->line, scr.msg);
		return false;
	}
	doGoto(F(), scr.onerrLabel, strlen(scr.onerrLabel), ref);
	return true;
}

/* Error in statement s (index si) of the current frame: ONERROR. false = stop. */
static bool onError(scrStmt_t *s, int si, int depthAtStart)
{
	if (scr.onerr == 1) {
		scr.ok = false;
		snprintf(scr.err, sizeof(scr.err), "%s", scr.msg);
		while (scr.depth > depthAtStart) {
			popFrame();			/* a frame pushed by the failing statement */
		}
		F()->pc = si + 1;
		return true;
	}
	if (scr.onerr == 2 && scr.depth >= scr.onerrDepth) {
		scr.ok = false;
		snprintf(scr.err, sizeof(scr.err), "%s", scr.msg);
		int ref = si;
		while (scr.depth > depthAtStart) {
			popFrame();
		}
		bool unwound = false;
		while (scr.depth > scr.onerrDepth) {
			if (F()->handler) {
				scr.inHandler = false;
			}
			popFrame();
			unwound = true;
		}
		if (unwound) {
			ref = F()->pc - 1;
		}
		return gotoSafe(ref < 0 ? 0 : ref, s);
	}
	report(s->src, s->line, scr.msg);
	return false;
}

enum { ONE_OK, ONE_ERR, ONE_STOP, ONE_END };

/* One statement of the current frame (between-line work first). */
static int runOne(void)
{
	static char ev[SCR_VAL_MAX + 1];
	if (setjmp(scr.jb)) {
		return ONE_ERR;
	}
	scrFrame_t *f = F();
	scrStmt_t  *s = ST(f);
	if (s->lineStart) {
		/* events for handlers, between lines */
		if (!scr.inHandler) {
			int mask = 0;
			for (int i = 0; i < 4; i++) {
				if (scr.handlers[i][1] >= 0) {
					mask |= 1 << i;
				}
			}
			if (mask && scrHostEvent(mask, ev, sizeof(ev), 0) == 1) {
				pushHandler(ev, RET_HANDLER);
				return ONE_OK;
			}
		}
		if (shouldPause(s)) {
			char msg[120];
			snprintf(msg, sizeof(msg), "<<SCRIPT %s PAUSED %s:%d>>\n", scr.name, scrSrcName(s->src), s->line);
			scrHostNotify(msg);
			scr.pauseNext = false;
			int go = scrHostPause();
			if (go == SCR_GO_STOP || (scrHostCheck() & SCR_CHK_STOP)) {
				return ONE_STOP;
			}
			scr.stepMode  = go;
			scr.stepDepth = scr.depth;
			scr.resumed   = false;
		}
		if (scr.trace) {
			char            msg[SCR_LINE_MAX + 40];
			const scrSrc_t *sf = &scr.src[s->src];
			snprintf(msg, sizeof(msg), "%s:%d %s", sf->name, s->line,
			         s->line <= sf->nLines ? sf->lines[s->line - 1] : s->text);
			scrHostDebug(msg);
		}
	}
	execStmt(f, s);
	if (s_stopped) {
		return ONE_STOP;
	}
	return scr.exitReq ? ONE_END : ONE_OK;
}

static scrResult_t runLoop(void)
{
	for (;;) {
		scrFrame_t *f = F();
		if (f->pc >= f->mod->n) {
			return SCR_RES_END;		/* past the last statement */
		}
		scr.aUsed     = 0;
		scr.exprDepth = 0;
		scr.cur       = ST(f);

		int chk = scrHostCheck();
		if (chk & SCR_CHK_STOP) {
			return SCR_RES_STOPPED;
		}
		if (chk & SCR_CHK_PAUSE) {
			scr.pauseNext = true;
			scr.resumed   = false;
		}
		int si    = f->pc;
		int depth = scr.depth;
		switch (runOne()) {
		case ONE_ERR:
			if (scrHostCheck() & SCR_CHK_STOP) {
				return SCR_RES_STOPPED;		/* e.g. STOP NOW cut a command: "aborted" */
			}
			if (!onError(&scr.frames[depth - 1].mod->st[si], si, depth)) {
				return SCR_RES_ERROR;
			}
			break;
		case ONE_STOP:
			return SCR_RES_STOPPED;
		case ONE_END:
			return SCR_RES_END;
		default:
			break;
		}
	}
}

static void cleanup(void)
{
	scrHostTimer(0, 0);
	while (scr.depth > 0) {
		popFrame();
	}
	for (int i = 0; i < scr.nGlobals; i++) {
		scrFreeVar(&scr.globals[i]);
	}
	scr.nGlobals = 0;
	scrFreeVar(&scr.ret);
	scrFreeVar(&scr.event);
	scrHostFree(scr.files);
	scr.files  = NULL;
	scr.nFiles = 0;
	scrUnloadAll();
	scr.active = false;
	scr.cur    = NULL;
}

static void resetRun(const char *name)
{
	scr.nSrc = scr.nMod = scr.nConst = 0;
	scr.depth = scr.nGlobals = 0;
	scr.stackMax = SCR_STACK_DEF;
	for (int i = 0; i < 4; i++) {
		scr.handlers[i][0] = scr.handlers[i][1] = -1;
	}
	scr.inHandler = false;
	scr.trace = scr.onerr = 0;
	scr.quiet = SCR_QUIET_DEF;
	scr.ok = true;
	scr.err[0] = '\0';
	memset(&scr.ret, 0, sizeof(scr.ret));
	memset(&scr.event, 0, sizeof(scr.event));
	scr.prng = false;
	scr.stepMode = SCR_GO_CONT;
	scr.pauseNext = scr.resumed = scr.exitReq = false;
	scr.cur = NULL;
	scr.locSrc = -1;
	scr.locLine = 0;
	scr.loaded = 0;
	scr.nFiles = 0;
	scr.files  = NULL;
	scr.filesMax = scrHostFilesDefault();
	if (scr.filesMax < 1 || scr.filesMax > SCR_FILES_MAX) {
		scr.filesMax = SCR_FILES_DEF;
	}
	s_stopped = false;
	snprintf(scr.name, sizeof(scr.name), "%s", name);
}

scrResult_t scrRun(const char *name, const char *args, const char *fn, bool load)
{
	char msg[100];
	if (!scrEnsureCtx()) {
		return SCR_RES_ERROR;
	}
	resetRun(name);
	scr.active = true;
	scr.t0     = scrHostMs();
	scr.aUsed  = 0;

	if (setjmp(scr.jb)) {
		report(scr.locSrc, scr.locLine, scr.msg);
		cleanup();
		return SCR_RES_ERROR;
	}
	scrMod_t *m = scrLoadModule(name);
	scr.files = scrHostAlloc(scr.filesMax * sizeof(scr.files[0]));	/* .FILES n or run.files */
	if (!scr.files) {
		scrFail("out of memory");
	}
	if (fn) {
		int fi = scrFindFn(m, fn, strlen(fn));
		if (fi < 0) {
			cleanup();
			return SCR_RES_NOFN;
		}
		pushCall(m, fi, args ? args : "", RET_NONE, NULL, false);
	} else {
		pushFrame(m, -1, 0);
		setText(scrFindVar(false, "arg", true), args ? args : "");
	}
	scr.pauseNext = load;

	scrResult_t r = runLoop();
	if (r == SCR_RES_END) {
		snprintf(msg, sizeof(msg), "<<SCRIPT %s END>>\n", scr.name);
		scrHostNotify(msg);
	} else if (r == SCR_RES_STOPPED) {
		snprintf(msg, sizeof(msg), "<<SCRIPT %s STOPPED>>\n", scr.name);
		scrHostNotify(msg);
	}
	cleanup();
	return r;
}

bool scrHasFn(const char *name, const char *fn, bool nonEmpty)
{
	if (!scrEnsureCtx() || scr.active) {
		return false;
	}
	resetRun(name);
	scr.active = true;
	bool ok = false;
	if (setjmp(scr.jb) == 0) {
		scrMod_t *m  = scrLoadModule(name);
		int       fi = scrFindFn(m, fn, strlen(fn));
		if (fi >= 0) {
			ok = !nonEmpty || m->fns[fi].end > m->fns[fi].start + 1;
			if (ok && nonEmpty) {
				ok = false;
				for (int i = m->fns[fi].start + 1; i < m->fns[fi].end; i++) {
					if (m->st[i].kind != K_VARS) {
						ok = true;
					}
				}
			}
		}
	}
	cleanup();
	return ok;
}

/* ================= console helpers ================= */

static void showVal(scrReply_t reply, const char *prefix, const scrVar_t *v)
{
	char line[SCR_VAL_MAX + 48];
	if (v->num) {
		char nb[64];
		scrFmtNumBuf(v->n, nb, sizeof(nb));
		snprintf(line, sizeof(line), "%s%s = %s\n", prefix, v->name, nb);
	} else {
		snprintf(line, sizeof(line), "%s%s = \"%.*s\"\n", prefix, v->name, v->len, v->s ? v->s : "");
	}
	reply(line);
}

const char *scrCmdVars(scrReply_t reply)
{
	if (!scrEnsureCtx() || !scr.active || scr.depth == 0) {
		return "no script paused";
	}
	scrFrame_t *f = F();
	for (int i = 0; i < f->nLocals; i++) {
		showVal(reply, "$", &f->locals[i]);
	}
	for (int i = 0; i < scr.nGlobals; i++) {
		showVal(reply, "$$", &scr.globals[i]);
	}
	return NULL;
}

const char *scrCmdStack(scrReply_t reply)
{
	if (!scrEnsureCtx() || !scr.active || scr.depth == 0) {
		return "no script paused";
	}
	char line[400];
	int  n = 0;
	for (int i = 0; i < scr.depth; i++) {
		scrFrame_t *f  = &scr.frames[i];
		int         pc = (i < scr.depth - 1) ? f->pc - 1 : f->pc;
		if (pc < 0) {
			pc = 0;
		}
		if (pc >= f->mod->n) {
			pc = f->mod->n - 1;
		}
		scrStmt_t *s = &f->mod->st[pc];
		n += snprintf(line + n, sizeof(line) - n, "%s%s.s:%d", i ? " > " : "", scrSrcName(s->src), s->line);
		if (f->fn >= 0 && n < (int)sizeof(line)) {
			n += snprintf(line + n, sizeof(line) - n, " (%s)", f->mod->fns[f->fn].name);
		}
		if (n >= (int)sizeof(line) - 1) {
			break;
		}
	}
	snprintf(line + (n < (int)sizeof(line) - 2 ? n : (int)sizeof(line) - 2), 2, "\n");
	reply(line);
	return NULL;
}

/* "[file:]n" -> file (default: active main script) and line */
static const char *bptParse(const char *a, char *file, int *line)
{
	const char *c = strchr(a, ':');
	if (c) {
		int l = c - a;
		if (l > 2 && strncasecmp(c - 2, ".s", 2) == 0) {
			l -= 2;
		}
		if (l <= 0 || l > SCR_FNAME_MAX) {
			return "bad file name";
		}
		memcpy(file, a, l);
		file[l] = '\0';
		a = c + 1;
	} else {
		if (!scr.active) {
			return "no active script: use BPT file:n";
		}
		snprintf(file, SCR_FNAME_MAX + 1, "%s", scr.name);
	}
	char *e;
	long  n = strtol(a, &e, 10);
	if (e == a || *e || n < 1 || n > 65535) {
		return "line must be a number";
	}
	*line = (int)n;
	return NULL;
}

static const char *srcLine(const char *file, int line)
{
	for (int i = 0; i < scr.nSrc; i++) {
		if (strcasecmp(scr.src[i].name, file) == 0 && line <= scr.src[i].nLines) {
			return scr.src[i].lines[line - 1];
		}
	}
	return "";
}

const char *scrCmdBpt(int argc, char *argv[], scrReply_t reply)
{
	char file[SCR_FNAME_MAX + 1];
	int  line;
	char out[SCR_LINE_MAX + 400];

	if (!scrEnsureCtx()) {
		return "out of memory";
	}
	if (argc == 0) {
		for (int i = 0; i < scrNBpts; i++) {
			snprintf(out, sizeof(out), "#%d : %s.s:%d  %.200s\n", i + 1, scrBpts[i].file, scrBpts[i].line,
			         scr.active ? srcLine(scrBpts[i].file, scrBpts[i].line) : "");
			reply(out);
		}
		if (!scrNBpts) {
			reply("no breakpoints\n");
		}
		return NULL;
	}
	if (strcasecmp(argv[0], "DEL") == 0) {
		if (argc != 2) {
			return "usage: BPT DEL ALL | #n | [file:]n";
		}
		if (strcasecmp(argv[1], "ALL") == 0) {
			scrNBpts = 0;
			return NULL;
		}
		int k = -1;
		if (argv[1][0] == '#') {
			k = atoi(argv[1] + 1) - 1;
			if (k < 0 || k >= scrNBpts) {
				return "no such breakpoint";
			}
		} else {
			const char *e = bptParse(argv[1], file, &line);
			if (e) {
				return e;
			}
			for (int i = 0; i < scrNBpts; i++) {
				if (scrBpts[i].line == line && strcasecmp(scrBpts[i].file, file) == 0) {
					k = i;
				}
			}
			if (k < 0) {
				return "no breakpoint there";
			}
		}
		memmove(&scrBpts[k], &scrBpts[k + 1], (scrNBpts - k - 1) * sizeof(scrBpt_t));
		scrNBpts--;
		return NULL;
	}
	if (argc != 1) {
		return "usage: BPT [[file:]n | DEL ALL|#n|[file:]n]";
	}
	const char *e = bptParse(argv[0], file, &line);
	if (e) {
		return e;
	}
	for (int i = 0; i < scrNBpts; i++) {
		if (scrBpts[i].line == line && strcasecmp(scrBpts[i].file, file) == 0) {
			return NULL;
		}
	}
	if (scrNBpts >= SCR_BPT_MAX) {
		return "too many breakpoints (max 16)";
	}
	snprintf(scrBpts[scrNBpts].file, sizeof(scrBpts[0].file), "%s", file);
	scrBpts[scrNBpts++].line = line;
	return NULL;
}

const char *scrCmdList(int argc, char *argv[], scrReply_t reply)
{
	char name[SCR_FNAME_MAX + 1];
	if (!scrEnsureCtx()) {
		return "out of memory";
	}
	if (argc == 0) {
		if (!scr.active) {
			return "no active script";
		}
		snprintf(name, sizeof(name), "%s", scr.name);
	} else {
		int l = strlen(argv[0]);
		if (l > 2 && strcasecmp(argv[0] + l - 2, ".s") == 0) {
			l -= 2;
		}
		if (l > SCR_FNAME_MAX) {
			return "bad file name";
		}
		snprintf(name, sizeof(name), "%.*s", l, argv[0]);
	}
	long from = argc > 1 ? atol(argv[1]) : 1;
	long to   = argc > 2 ? atol(argv[2]) : 100000;

	/* loaded text, else read the file */
	char      **lines = NULL;
	int         n     = 0;
	char       *text  = NULL;
	for (int i = 0; i < scr.nSrc; i++) {
		if (strcasecmp(scr.src[i].name, name) == 0) {
			lines = scr.src[i].lines;
			n     = scr.src[i].nLines;
		}
	}
	if (!lines) {
		size_t      len;
		const char *err = "file not found";
		text = scrHostRead(name, &len, &err);
		if (!text) {
			return err;
		}
	}
	int curLine = -1;
	if (scr.active && scr.cur && strcasecmp(scrSrcName(scr.cur->src), name) == 0) {
		curLine = scr.cur->line;
	}
	char out[SCR_LINE_MAX + 16];
	int  ln = 0;
	for (char *p = text;; ) {
		const char *l;
		if (lines) {
			if (ln >= n) {
				break;
			}
			l = lines[ln];
		} else {
			if (!*p) {
				break;
			}
			char *e = p;
			while (*e && *e != '\n' && *e != '\r') {
				e++;
			}
			char *next = e;
			if (*next == '\r' && next[1] == '\n') {
				next += 2;
			} else if (*next) {
				next++;
			}
			*e = '\0';
			l = p;
			p = next;
		}
		ln++;
		if (ln < from || ln > to) {
			continue;
		}
		bool bp = false;
		for (int i = 0; i < scrNBpts; i++) {
			if (scrBpts[i].line == ln && strcasecmp(scrBpts[i].file, name) == 0) {
				bp = true;
			}
		}
		snprintf(out, sizeof(out), "%4d%c%c %s\n", ln, ln == curLine ? '>' : ' ', bp ? '*' : ' ', l);
		reply(out);
	}
	scrHostFree(text);
	return NULL;
}
