#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cmd.h"

#define LOG_LINE_MAX	256	/* ESP_LOGx line (longer is cut) */
#define DBGF_MAX	200	/* dbgf text */
#define LOG_LOCK_MS	100	/* busy: fall back to the console */

static SemaphoreHandle_t s_lock;		/* s_buf */
static char              s_buf[LOG_LINE_MAX + 2];
static vprintf_like_t    s_console;		/* the original output */
static volatile int      s_uiPort = -1;
static volatile bool     s_debug;
static volatile int      s_mode = LOG_AUTO;	/* LOG command, this boot only */

static const char *const s_modeNames[] = { "AUTO", "UART", "USB", "BOTH" };

static int  logVprintf(const char *fmt, va_list ap);
static void logOut(const char *text, size_t len);
static const char *logCmd(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t logInit(void)
{
	s_lock = xSemaphoreCreateMutex();
	if (!s_lock) {
		return ESP_ERR_NO_MEM;
	}
	s_console = esp_log_set_vprintf(logVprintf);
	return cmdRegister("LOG", 0, 1, logCmd, "LOG [UART|USB|BOTH|AUTO]  log output port(s)");
}

void logSetUiPort(int port)
{
	s_uiPort = port;
	s_debug  = false;
}

void logSetDebug(bool on)
{
	s_debug = on;
}

bool logGetDebug(void)
{
	return s_debug;
}

void logSetMode(logMode_t mode)
{
	if (mode >= LOG_AUTO && mode <= LOG_BOTH) {
		s_mode = mode;
	}
}

logMode_t logGetMode(void)
{
	return (logMode_t)s_mode;
}

void dbgf(const char *fmt, ...)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOG_LOCK_MS)) != pdTRUE) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(s_buf, DBGF_MAX + 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		n = 0;
	} else if (n > DBGF_MAX) {
		n = DBGF_MAX;
	}
	s_buf[n++] = '\n';
	logOut(s_buf, n);
	xSemaphoreGive(s_lock);
}

/* ================= Internals ================= */

/* esp_log output: format, then send by the routing rule. Interrupts, the
 * time before the scheduler and a busy buffer use the original console. */
static int logVprintf(const char *fmt, va_list ap)
{
	if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
	    xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOG_LOCK_MS)) != pdTRUE) {
		return s_console(fmt, ap);
	}
	int n = vsnprintf(s_buf, LOG_LINE_MAX + 1, fmt, ap);
	if (n > LOG_LINE_MAX) {
		n = LOG_LINE_MAX;
		s_buf[n++] = '\n';				/* cut: keep the line end */
	}
	if (n > 0) {
		logOut(s_buf, n);
	}
	xSemaphoreGive(s_lock);
	return n;
}

/* Caller holds s_lock. AUTO: every port except the UI port; UART / USB /
 * BOTH: exactly those ports. Debug on: all ports. */
static void logOut(const char *text, size_t len)
{
	int ui   = s_uiPort;
	int mode = s_mode;

	for (int p = 0; p < SER_COUNT; p++) {
		bool fixed = (mode == LOG_BOTH) || (mode == LOG_UART && p == SER_UART) ||
		             (mode == LOG_USB && p == SER_USB);
		if (s_debug || (mode == LOG_AUTO ? p != ui : fixed)) {
			serWrite((serPort_t)p, text, len, true);
		}
	}
}

/* ================= Commands ================= */

static const char *logCmd(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 1) {
		int m;
		for (m = LOG_AUTO; m <= LOG_BOTH; m++) {
			if (strcasecmp(argv[0], s_modeNames[m]) == 0) {
				break;
			}
		}
		if (m > LOG_BOTH) {
			return "usage: LOG [UART|USB|BOTH|AUTO]";
		}
		s_mode = m;
	}
	char text[32];
	snprintf(text, sizeof(text), "LOG %s%s\n", s_modeNames[s_mode], s_debug ? " (debug on)" : "");
	reply(text);
	return NULL;
}
