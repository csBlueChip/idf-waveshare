#include "rad.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cmd.h"
#include "nv.h"

#define RAD_PAIRS_MAX	8

static const struct {
	const char *name;
	const char *key;
} s_map[RAD_PAIRS_MAX] = {
	{ "ssid",    "rad.ssid" },
	{ "pwd",     "rad.pwd"  },
	{ "ble",     "rad.ble"  },
	{ "ip",      "rad.ip"   },
	{ "netmask", "rad.mask" },
	{ "pwr",     "rad.pwr"  },
	{ "nogw",    "rad.nogw" },
	{ "maxcli",  "rad.maxcli" },
};

#define RAD_COUNT	(int)(sizeof(s_map) / sizeof(s_map[0]))

static int         radFind(const char *name);
static const char *radCmd(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t radInit(void)
{
	return cmdRegister("RAD", 1, 1 + 2 * RAD_PAIRS_MAX, radCmd,
	                   "RAD SET key value [key value..] | RAD GET [key..]  ssid pwd ble ip netmask pwr nogw maxcli");
}

/* ================= Internals ================= */

static int radFind(const char *name)
{
	for (int i = 0; i < RAD_COUNT; i++) {
		if (strcasecmp(name, s_map[i].name) == 0) {
			return i;
		}
	}
	return -1;
}

static const char *radCmd(int argc, char *argv[], cmdReply_t reply)
{
	static char err[96];
	char        text[96];

	if (strcasecmp(argv[0], "SET") == 0 && argc >= 3 && (argc - 1) % 2 == 0) {
		const char *keys[RAD_PAIRS_MAX], *vals[RAD_PAIRS_MAX];
		int         n = (argc - 1) / 2;
		for (int p = 0; p < n; p++) {
			int i = radFind(argv[1 + 2 * p]);
			if (i < 0) {
				snprintf(err, sizeof(err), "unknown key %s", argv[1 + 2 * p]);
				return err;
			}
			nvUnescape(argv[2 + 2 * p]);
			keys[p] = s_map[i].key;
			vals[p] = argv[2 + 2 * p];
		}
		return nvSetMany(n, keys, vals, NULL);
	}

	if (strcasecmp(argv[0], "GET") == 0) {
		for (int a = 1; a < argc; a++) {		/* check all names first */
			if (radFind(argv[a]) < 0) {
				snprintf(err, sizeof(err), "unknown key %s", argv[a]);
				return err;
			}
		}
		int n = (argc == 1) ? RAD_COUNT : argc - 1;
		for (int k = 0; k < n; k++) {
			int i = (argc == 1) ? k : radFind(argv[1 + k]);
			snprintf(text, sizeof(text), "%s=%s\n", s_map[i].name, nvGet(s_map[i].key));
			reply(text);
		}
		return NULL;
	}

	return "usage: RAD SET key value [key value..] | RAD GET [key..]";
}
