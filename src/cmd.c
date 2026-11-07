#include "cmd.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_attr.h"

typedef struct {
	const char   *name;
	int           minArgs;
	int           maxArgs;
	cmdHandler_t  handler;
	cmdRawHandler_t raw;		/* set for raw commands (handler unused) */
	const char   *help;
} cmdEntry_t;

static EXT_RAM_BSS_ATTR cmdEntry_t s_cmds[CMD_MAX_COMMANDS];
static int        s_cmdCount;
static volatile bool s_disabled;
static cmdPostHook_t s_postHook;
static volatile cmdGate_t s_gate;

static const char *cmdHelp(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t cmdInit(void)
{
	s_cmdCount = 0;
	return cmdRegister("HELP", 0, 0, cmdHelp, "HELP  list commands");
}

esp_err_t cmdRegister(const char *name, int minArgs, int maxArgs,
                      cmdHandler_t handler, const char *help)
{
	if (!name || !handler || minArgs < 0 || maxArgs < minArgs || maxArgs > CMD_MAX_ARGS) {
		return ESP_ERR_INVALID_ARG;
	}
	if (s_cmdCount >= CMD_MAX_COMMANDS) {
		return ESP_ERR_NO_MEM;
	}
	for (int i = 0; i < s_cmdCount; i++) {
		if (strcasecmp(s_cmds[i].name, name) == 0) {
			return ESP_ERR_INVALID_STATE;   /* duplicate */
		}
	}

	s_cmds[s_cmdCount++] = (cmdEntry_t) {
		.name = name, .minArgs = minArgs, .maxArgs = maxArgs,
		.handler = handler, .help = help ? help : name,
	};
	return ESP_OK;
}

esp_err_t cmdRegisterRaw(const char *name, cmdRawHandler_t handler, const char *help)
{
	if (!name || !handler) {
		return ESP_ERR_INVALID_ARG;
	}
	if (s_cmdCount >= CMD_MAX_COMMANDS) {
		return ESP_ERR_NO_MEM;
	}
	for (int i = 0; i < s_cmdCount; i++) {
		if (strcasecmp(s_cmds[i].name, name) == 0) {
			return ESP_ERR_INVALID_STATE;
		}
	}

	s_cmds[s_cmdCount++] = (cmdEntry_t) {
		.name = name, .raw = handler, .help = help ? help : name,
	};
	return ESP_OK;
}

int cmdCount(void)
{
	return s_cmdCount;
}

const char *cmdName(int i)
{
	return (i >= 0 && i < s_cmdCount) ? s_cmds[i].name : NULL;
}

void cmdSetGate(cmdGate_t gate)
{
	s_gate = gate;
}

esp_err_t cmdSetPostHook(cmdPostHook_t hook)
{
	s_postHook = hook;
	return ESP_OK;
}

void cmdDisable(void)
{
	s_disabled = true;
}

bool cmdExists(const char *name)
{
	for (int i = 0; i < s_cmdCount; i++) {
		if (strcasecmp(s_cmds[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

void cmdExecute(const char *line, cmdReply_t reply)
{
	if (s_disabled) {
		reply("ERR commands disabled\n");
		return;
	}

	char clean[CMD_LINE_MAX + 1];
	char buf[CMD_LINE_MAX + 1];
	char out[CMD_LINE_MAX + 32];

	/* strip comment: '#' to end of line, except an escaped "\#"; backslash
	 * pairs are skipped as a unit so "\\#" is a backslash then a comment */
	size_t n = 0;
	for (const char *p = line; *p && n < sizeof(clean) - 1; p++) {
		if (*p == '#') {
			break;
		}
		clean[n++] = *p;
		if (*p == '\\' && p[1] && n < sizeof(clean) - 1) {
			clean[n++] = *++p;
		}
	}
	while (n > 0 && (clean[n - 1] == ' ' || clean[n - 1] == '\t')) {
		n--;					/* trailing blanks before the comment */
	}
	clean[n] = '\0';
	line = clean;				/* everything below sees the stripped line */

	strlcpy(buf, line, sizeof(buf));        /* tokenise a copy, keep original for "unknown" */

	char *argv[CMD_MAX_ARGS + 2];           /* name + args + 1 to detect overflow */
	int   argc = 0;
	char *save = NULL;

	for (char *tok = strtok_r(buf, " \t", &save);
	     tok != NULL && argc < CMD_MAX_ARGS + 2;
	     tok = strtok_r(NULL, " \t", &save)) {
		argv[argc++] = tok;
	}

	if (argc == 0) {
		return;                              /* empty or comment-only line: silent */
	}

	cmdGate_t gate = s_gate;
	const char *refused = gate ? gate(argc, argv) : NULL;
	if (refused) {
		snprintf(out, sizeof(out), "ERR %s\n", refused);
		reply(out);
		return;
	}

	for (int i = 0; i < s_cmdCount; i++) {
		const cmdEntry_t *c = &s_cmds[i];
		if (strcasecmp(c->name, argv[0]) != 0) {
			continue;
		}

		const char *err;
		if (c->raw) {
			/* rest of the original line after the name and ONE separator
			 * (further spaces are kept: PRINT  x prints " x") */
			const char *p = line;
			while (*p == ' ' || *p == '\t') p++;
			while (*p && *p != ' ' && *p != '\t') p++;
			if (*p) p++;
			err = c->raw(p, reply);
		} else {
			int nArgs = argc - 1;
			if (nArgs < c->minArgs || nArgs > c->maxArgs) {
				snprintf(out, sizeof(out), "ERR usage: %s\n", c->help);
				reply(out);
				return;
			}
			err = c->handler(nArgs, &argv[1], reply);
		}
		if (s_postHook) {
			s_postHook();			/* e.g. push screen changes before replying */
		}
		if (err) {
			snprintf(out, sizeof(out), "ERR %s\n", err);
			reply(out);
		} else {
			reply("OK\n");
		}
		return;
	}

	snprintf(out, sizeof(out), "unknown |%s|\n", line);
	reply(out);
}

bool cmdParseU8(const char *str, uint8_t *out)
{
	if (!str || !*str) {
		return false;
	}

	unsigned v = 0;
	for (const char *p = str; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		v = v * 10 + (unsigned)(*p - '0');
		if (v > 255) {
			return false;
		}
	}
	*out = (uint8_t)v;
	return true;
}

bool cmdParseInt(const char *str, int min, int max, int *out)
{
	if (!str) {
		return false;
	}

	bool neg = (*str == '-');
	const char *p = neg ? str + 1 : str;
	if (!*p) {
		return false;
	}

	long v = 0;
	for (; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		v = v * 10 + (*p - '0');
		if (v > 1000000L) {
			return false;		/* far outside any sane range; stop before overflow */
		}
	}
	if (neg) {
		v = -v;
	}
	if (v < min || v > max) {
		return false;
	}
	*out = (int)v;
	return true;
}

/* ================= Built-ins ================= */

static const char *cmdHelp(int argc, char *argv[], cmdReply_t reply)
{
	char out[128];
	for (int i = 0; i < s_cmdCount; i++) {
		snprintf(out, sizeof(out), "%s\n", s_cmds[i].help);
		reply(out);
	}
	return NULL;
}
