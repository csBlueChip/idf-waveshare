#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Command parser - transport independent.
 *   - '#' starts a comment to end of line ("\#" is not a comment; it is passed
 *     on for the command, e.g. ECHO prints '#'). Blank/comment-only lines are
 *     ignored silently.
 *   - Line is split on spaces/tabs.
 *   - Command names are case-insensitive.
 *   - Handlers receive only the arguments (argv[0] = first arg, not the name).
 *   - Handler returns NULL on success (parser replies "OK"),
 *     or an error string (parser replies "ERR <string>").
 */

#define CMD_MAX_COMMANDS  64
#define CMD_MAX_ARGS      18
#define CMD_LINE_MAX      200

/* Sends text back to whoever issued the command (BLE, UART, ...). */
typedef void (*cmdReply_t)(const char *text);

typedef const char *(*cmdHandler_t)(int argc, char *argv[], cmdReply_t reply);

/* Clear registry and register built-in commands (HELP). */
esp_err_t cmdInit(void);

/* Register a command. help = usage line shown by HELP, e.g. "LED r g b  set LED colour". */
esp_err_t cmdRegister(const char *name, int minArgs, int maxArgs,
                      cmdHandler_t handler, const char *help);

/* Raw handler: receives the rest of the line after the command name and ONE
 * separating space/tab, untouched (further spaces preserved; trailing blanks
 * and the comment are already removed). Same return rules. */
typedef const char *(*cmdRawHandler_t)(const char *args, cmdReply_t reply);

/* Register a raw command (no tokenising, e.g. ECHO). */
esp_err_t cmdRegisterRaw(const char *name, cmdRawHandler_t handler, const char *help);

/* Optional function run after every executed command (e.g. screen update). */
typedef void (*cmdPostHook_t)(void);
esp_err_t cmdSetPostHook(cmdPostHook_t hook);

/* Reject all further commands ("ERR commands disabled") until reboot. */
void cmdDisable(void);

/* A command of that name is registered. */
bool cmdExists(const char *name);

/* Parse and run one line (no trailing newline). Empty lines are ignored. */
void cmdExecute(const char *line, cmdReply_t reply);

/* Strict 0..255 decimal parse. Rejects empty, sign, non-digits, overflow. */
bool cmdParseU8(const char *str, uint8_t *out);

/* Strict signed decimal parse within min..max. Optional leading '-'. */
bool cmdParseInt(const char *str, int min, int max, int *out);
