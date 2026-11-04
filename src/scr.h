#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Script interpreter core (portable C, no ESP-IDF). The language is defined in
 * scripting.txt; the firmware side (task, commands, events) is script.c, which
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

/* name.s exists and loads, and has function fn; nonEmpty: with at least one
 * statement in its body. */
bool scrHasFn(const char *name, const char *fn, bool nonEmpty);

/* QUIET level of the running script (0 if none):
 * 0 all, 1 no command replies / OKs, 2 also no events to the client (default),
 * 3 also no command ERRs. */
int scrQuiet(void);

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
#define SCR_SRC_ALL	0x0F

/* Source mask of an event text ("BTN 1 SHORT" -> SCR_SRC_BTN), 0 if none. */
int scrEventSource(const char *text);

/* Resume modes returned by scrHostPause() */
enum { SCR_GO_CONT, SCR_GO_STEP, SCR_GO_IN, SCR_GO_OUT, SCR_GO_STOP };
