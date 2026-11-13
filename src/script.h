#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Scripts: firmware side of the interpreter (scr_*.c, language in SCRIPTING.txt).
 * One script task runs one script at a time; scripts live in SD:/scripts/name.s.
 *
 * Commands registered by scriptInit():
 *   RUN                 list the scripts
 *   RUN name [args]     start name.s (OK at once; output follows)
 *   LOAD name [args]    load and pause before the first line
 *   STOP [NOW]          stop the script (NOW: also cut a long command short)
 *   PAUSE               pause after the current line
 *   CONT                resume a paused script
 *   STEP [IN|OUT]       one line / into a call / out of the function (paused)
 *   BPT [[file:]n | DEL ALL|#n|[file:]n]   breakpoints
 *   VARS                variables of the paused script
 *   LIST [name [from [to]]]   list a script with line numbers
 *   STACK               call stack of the paused script
 * While a script runs, only STOP, STOP NOW and PAUSE are accepted (and lines
 * for INPUT); everything else replies "ERR script running".
 *
 * System scripts (NV sys.autorun 1), in SD:/sys/: onBoot.s at the end of
 * start-up, onWait.s (FN before / instead / after) around each Wait screen,
 * onConnect.s around the connect splash. By hand: RUN sys/onBoot etc.
 * While onBoot or onWait runs, holding BOOT for 3 s sets sys.autorun 0 and
 * reboots.
 */

/* Register the commands (the script task is created per run). Call after
 * cmdInit(), nvInit(), sdInit(). */
esp_err_t scriptInit(void);

/* Main task: a console line from the client (instead of cmdExecute). */
void scriptConsoleLine(const char *line);

/* uiSend() hook: queue <<BTN|IMU|UI ...>> events for the running script.
 * true = don't send it to the client (the script's QUIET is 2 or 3). */
bool scriptEvent(const char *text);

/* End of start-up: run onBoot (sys.autorun 1), blocking. */
void scriptBoot(void);

/* Main task, Wait screen: runs onWait "before"; true = an "instead" was started
 * (skip the built-in Wait screen). details = the onWait argument. */
bool scriptWaitBegin(const char *details);

/* Main task, the Wait screen ended (connect / UI switch): wait for a running
 * "instead" to exit, then run "after". */
void scriptWaitEnd(void);

/* Main task, client connected: onConnect before / instead (or builtin) / after. */
void scriptConnect(const char *uiName, void (*builtin)(void));

/* Main task, the client went away: a RUN script with ON UI is waited for, one
 * without is stopped; a paused one is stopped. */
void scriptClientGone(void);

/* Script name ("name" or "sys/name") exists on the card. */
bool scriptHas(const char *name);

/* Main task: run a script as a system script (BOOT 3 s escape on) and wait
 * for it to end (RESET! screen: sys/reset). false = could not start. */
bool scriptRunWait(const char *name, const char *args);

/* A script is running or paused. */
bool scriptActive(void);

/* STOP NOW was given: long commands (IMU REC, screen fades, image decode)
 * should end at once with "aborted". */
bool scriptAbortRequested(void);

/* Serial (USB) input while a script is paused with no client: the USB port is
 * the console. true = the bytes were taken. */
bool scriptUsbRx(const uint8_t *data, size_t len);
