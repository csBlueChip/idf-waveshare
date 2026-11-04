#pragma once

/* Script language limits (see scripting.txt section 15). One place to tweak. */

#define SCR_LINE_MAX		200		/* source line / command after substitution */
#define SCR_FILE_MAX		(32 * 1024)	/* a file plus its INSERTs */
#define SCR_INSERT_DEPTH	4
#define SCR_BLOCK_MAX		8		/* IF / LOOP / SWITCH nesting */
#define SCR_STACK_DEF		8		/* call depth (.STACK n) */
#define SCR_STACK_MAX		32
#define SCR_LOCALS		64		/* per call */
#define SCR_GLOBALS		64
#define SCR_NAME_MAX		15		/* variable / function / label / constant */
#define SCR_VAL_MAX		512		/* value length (longer text is cut) */
#define SCR_EVQ			16		/* event queue */
#define SCR_TIMERS		4
#define SCR_CONSTS		32
#define SCR_FNAME_MAX		31		/* script file name, without .s */
#define SCR_QUIET_DEF		2		/* QUIET at the start of a run */

/* Internal sizes */
#define SCR_SRC_MAX		24		/* source files per run (main, INSERTs, CALLed) */
#define SCR_MOD_MAX		8		/* script files with functions per run */
#define SCR_BPT_MAX		16		/* breakpoints */
#define SCR_ARENA		(24 * 1024)	/* scratch per statement (expression values) */
#define SCR_EXPR_DEPTH		16		/* nested ( ) / function calls in one expression */
#define SCR_YIELD_MS		1000		/* host yields 1 tick this often while running */
