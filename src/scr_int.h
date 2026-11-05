#pragma once

/* Interpreter internals shared by scr_load.c, scr_eval.c and scr_exec.c. */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include "scr.h"
#include "scr_host.h"
#include "script_cfg.h"

/* ---------- statements ---------- */

typedef enum {
	K_CMD, K_LET, K_INCR, K_GET, K_VARS, K_DEL, K_INS,
	K_IF, K_ELIF, K_ELSE, K_END, K_LOOP, K_BREAK, K_NEXT,
	K_SWITCH, K_CASE, K_GOTO, K_LABEL, K_EXIT, K_FN, K_RETURN, K_CALL,
	K_ON, K_ONERROR, K_QUIET, K_TRACE, K_PRINT, K_DEBUG, K_RNG, K_TIMER,
	K_WAIT, K_INPUT, K_BPT, K_ACC, K_GYR, K_STACK, K_CONST,
	K_CONSOLE, K_PAUSE,
	K_VARSIO, K_FWRITE, K_FREAD, K_FTELL, K_FSEEK, K_FCLOSE, K_FILES,
} scrKind_t;

typedef struct {
	uint8_t  kind;
	bool     lineStart;	/* first statement of its source line */
	uint8_t  src;		/* source file (scrSrc_t index) */
	uint16_t line;		/* 1-based line in that file */
	int16_t  jump;		/* IF/ELIF/ELSE/SWITCH/CASE: next branch; END: opener */
	int16_t  end;		/* openers: their END */
	int16_t  blk;		/* innermost enclosing IF/LOOP/SWITCH opener, -1 = none */
	int16_t  fn;		/* enclosing function, -1 = main body */
	char    *text;		/* statement text */
	char    *arg;		/* text after the keyword (trimmed) */
} scrStmt_t;

typedef struct {
	char  name[SCR_NAME_MAX + 1];
	int   start, end;	/* FN and END statements */
	bool  hasVars;
	char *vars;		/* allowed names "a b $g ..." (locals plain, globals with '$') */
} scrFn_t;

typedef struct {
	char name[SCR_NAME_MAX + 1];
	int  stmt;
	int  fn;
} scrLabel_t;

typedef struct {
	char       name[SCR_FNAME_MAX + 1];
	scrStmt_t *st;
	int        n, cap;
	scrFn_t   *fns;
	int        nFns;
	scrLabel_t *labels;
	int        nLabels;
	bool       mainHasVars;
	char      *mainVars;	/* main body VARS, as scrFn_t.vars */
	scrStmt_t *raw;		/* loader: statements before the structure pass */
	int        nRaw;
} scrMod_t;

typedef struct {
	char   name[SCR_FNAME_MAX + 1];
	char  *text;		/* whole file, lines NUL-separated */
	char **lines;
	int    nLines;
} scrSrc_t;

/* ---------- values ---------- */

typedef struct {
	bool        num;	/* number (n) or text (s, len) */
	double      n;
	const char *s;
	int         len;
} scrVal_t;

typedef struct {
	char   name[SCR_NAME_MAX + 1];
	bool   num;
	double n;
	char  *s;		/* text (scrHostAlloc), NULL = "" */
	int    len;
} scrVar_t;

typedef struct {
	int     opener;		/* LOOP statement */
	int     type;		/* L_TO, L_WHILE, L_TIMES, L_FOREVER */
	double  step;
	long    remaining;
	char    var[SCR_NAME_MAX + 2];	/* TO loops: "$x" or "$$x" style name */
} scrLoop_t;
enum { L_TO, L_WHILE, L_TIMES, L_FOREVER };

enum { RET_NONE, RET_LET, RET_WAITEV, RET_HANDLER };

typedef struct {
	scrMod_t *mod;
	int       pc;
	int       fn;		/* -1 = main body */
	scrVar_t *locals;
	int       nLocals;
	scrLoop_t loops[SCR_BLOCK_MAX];
	int       nLoops;
	int       ret;		/* RET_* */
	char      retVar[SCR_NAME_MAX + 3];
	bool      handler;
} scrFrame_t;

/* ---------- run state ---------- */

typedef struct {
	/* loaded */
	scrSrc_t  src[SCR_SRC_MAX];
	int       nSrc;
	size_t    loaded;		/* bytes of the main file + INSERTs being loaded */
	scrMod_t *mod[SCR_MOD_MAX];
	int       nMod;
	char      cname[SCR_CONSTS][SCR_NAME_MAX + 1];
	char     *cval[SCR_CONSTS];
	int       nConst;
	int       stackMax;

	/* running */
	bool       active;
	char       name[SCR_FNAME_MAX + 1];
	scrFrame_t frames[SCR_STACK_MAX + 1];
	int        depth;		/* frames in use */
	scrVar_t   globals[SCR_GLOBALS];
	int        nGlobals;
	int        handlers[4][2];	/* per source: module, function (-1 = none) */
	bool       inHandler;
	int        quiet, trace;
	int        onerr;		/* 0 STOP, 1 CONT, 2 GOTO */
	char       onerrLabel[SCR_NAME_MAX + 1];
	int        onerrDepth;
	bool       ok;
	char       err[160];
	scrVar_t   ret, event;
	bool       prng;
	int64_t    t0;
	int        stepMode, stepDepth;
	bool       pauseNext, resumed;
	scrStmt_t *cur;			/* statement being run */
	int        curDepth;
	bool       exitReq;		/* EXIT / UI switch */

	/* data files: positions (1 = first byte) */
	struct scrFile { char name[SCR_FNAME_MAX + 1]; long pos; } *files;	/* filesMax slots */
	int        nFiles, filesMax;		/* run.files or .FILES n */

	/* errors */
	jmp_buf    jb;
	char       msg[160];
	int        locSrc, locLine;	/* loader position (scr.cur == NULL) */

	/* arena (per statement) */
	char       arena[SCR_ARENA];
	size_t     aUsed;
	int        exprDepth;
} scrCtx_t;

/* the run state lives in a block from scrHostAlloc (PSRAM in the firmware) */
extern scrCtx_t *scrCtxP;
#define scr (*scrCtxP)
bool scrEnsureCtx(void);

/* breakpoints (kept between runs) */
typedef struct { char file[SCR_FNAME_MAX + 1]; int line; } scrBpt_t;
extern scrBpt_t scrBpts[SCR_BPT_MAX];
extern int      scrNBpts;

/* ---------- shared helpers ---------- */

void  scrFail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
char *scrArena(size_t n);
char *scrArenaStr(const char *s, int len);

bool  scrIsName(char c, bool first);
const char *scrSkip(const char *p);
bool  scrWordIs(const char *p, const char *w);	/* keyword at p, case-insensitive, then end/space */
int   scrStrcasecmp(const char *a, const char *b);

bool  scrParseNum(const char *s, int len, double *out);
const char *scrFmtNum(double v);		/* arena */
void  scrFmtNumBuf(double v, char *out, size_t n);
bool  scrToNum(scrVal_t v, double *out);
const char *scrToText(scrVal_t v, int *len);	/* arena for numbers */
bool  scrTruth(scrVal_t v);
scrVal_t scrNum(double n);
scrVal_t scrText(const char *s, int len);

/* words */
int   scrWords(const char *s, int len);
bool  scrWord(const char *s, int len, int n, int *start, int *wlen);

/* variables */
scrVar_t *scrFindVar(bool global, const char *name, bool create);
void  scrSetVar(scrVar_t *v, scrVal_t val);
scrVal_t scrVarVal(const scrVar_t *v);
void  scrFreeVar(scrVar_t *v);

/* expressions / text (scr_eval.c) */
scrVal_t scrEval(const char **pp, const char *stopWords);
scrVal_t scrEvalAll(const char *s);			/* whole string, error if extra */
const char *scrSubst(const char *s, int mode, char quote);	/* arena */
/* substitution modes: CMD '#' -> "\#", "\$" -> '$'; TEXT '\' -> "\\" (an escape step
 * follows); PLAIN values as they are; RAW (ECHO / FLOG) '\' '#' and the quote
 * escaped, "\$" kept (the command does quotes and escapes) */
enum { SUB_CMD, SUB_TEXT, SUB_PLAIN, SUB_RAW };
const char *scrUnquote(const char *in);		/* arena; fails on bad quotes */
void  scrUnescape(char *s);			/* \n \\ \# \$ ; \1..\9 dropped */
scrVal_t scrReadVarRef(const char **pp);	/* $x $$x $.x ${..} [n] at *pp */
bool  scrConstFind(const char *name, int len, const char **val);
void  scrRngSeed(uint32_t seed);
double scrRnd16(void);

/* loader (scr_load.c) */
scrMod_t *scrLoadModule(const char *name);
scrMod_t *scrFindModule(const char *name);
void  scrUnloadAll(void);
int   scrFindFn(scrMod_t *m, const char *name, int len);
int   scrFindLabel(scrMod_t *m, int fn, const char *name, int len);
const char *scrSrcName(int src);
bool  scrIsReserved(const char *w, int len);
bool  scrNameAllowed(scrMod_t *m, int fn, const char *name, bool global);	/* VARS rule */
