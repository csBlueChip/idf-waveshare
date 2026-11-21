#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Script interpreter core (portable C, no ESP-IDF). The language is defined in
 * SCRIPTING.txt; the firmware side (task, commands, events) is script.c, which
 * implements the hooks in scr_host.h. Only one script runs at a time; all
 * functions below are called from the script task, except the console helpers
 * (scrCmd*), which are called while the script is paused or not running.
 */

typedef void (*scrReply_t)(const char *text);

typedef enum {
	SCR_RES_END,		/* finished, EXIT or RETURN in the main body */
	SCR_RES_STOPPED,	/* STOP */
	SCR_RES_ERROR,		/* load or run-time error (reported) */
	SCR_RES_NOFN,		/* fn given and the file has no such function */
} scrResult_t;

/* Run name.s with args. fn != NULL: run only that function (system scripts,
 * e.g. sys/onWait "before"); top-level code does not run. load: pause before the
 * first line (LOAD). Blocks until the script ends. Sends <<SCRIPT ...>>. */
scrResult_t scrRun(const char *name, const char *args, const char *fn, bool load);

/* UI CALL file.fn: NULL if name.s loads and has function fn, else why not
 * ("no function fn in name" or the load error). */
const char *scrFnCheck(const char *name, const char *fn);

/* name.s exists and loads, and has function fn; nonEmpty: with at least one
 * statement in its body. */
bool scrHasFn(const char *name, const char *fn, bool nonEmpty);

/* UI EVENTS volume of the running script (3 if none): what the client sees
 * of it: 3 all, 2 no command replies / OKs, 1 also no events (default), 0
 * also no command ERRs. PRINT and <<SCRIPT ...>> are always sent. */
int scrUiVol(void);

/* Event sources the running script has turned on (EVENTS src 1, ON src fn,
 * TIMER n ms); 0 if none / no script. Events of other sources are not
 * queued for it. */
int scrEvMask(void);

/* Name of the running / paused script ("" if none). */
const char *scrActiveName(void);

/* The running script has a handler for source mask (SCR_SRC_*). */
bool scrHasHandler(int srcMask);

/* Console commands (return NULL or an error text). */
const char *scrCmdVars(scrReply_t reply);			/* paused only */
const char *scrCmdStack(scrReply_t reply);			/* paused only */
const char *scrCmdList(int argc, char *argv[], scrReply_t reply);
const char *scrCmdBpt(int argc, char *argv[], scrReply_t reply);

/* Event sources */
#define SCR_SRC_BTN	0x01
#define SCR_SRC_IMU	0x02
#define SCR_SRC_UI	0x04
#define SCR_SRC_TIMER	0x08
#define SCR_SRC_SYS	0x10
#define SCR_SRC_ALL	0x1F
#define SCR_SRCS	5		/* BTN IMU UI TIMER SYS */

/* Source mask of an event text ("BTN 1 SHORT" -> SCR_SRC_BTN), 0 if none. */
int scrEventSource(const char *text);

/* Resume modes returned by scrHostPause() */
enum { SCR_GO_CONT, SCR_GO_STEP, SCR_GO_IN, SCR_GO_OUT, SCR_GO_STOP };
