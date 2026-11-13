#include "script.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"

#include "scr.h"
#include "scr_host.h"
#include "script_cfg.h"
#include "cmd.h"
#include "nv.h"
#include "sd.h"
#include "ui.h"
#include "log.h"
#include "imu.h"
#include "bat.h"
#include "lcd.h"
#include "sys.h"
#include "ser.h"

static const char *TAG = "script";

#define SCRIPT_DIR		SD_MOUNT_POINT "/scripts"
#define SCRIPT_TASK_STACK	16384
#define SCRIPT_TASK_PRIO	1		/* = main task: STOP is always read */
#define SCRIPT_EV_TEXT		64
#define SCRIPT_LIST_MAX		64
#define SCRIPT_WATCH_MS		50		/* BOOT escape watcher poll */
#define SCRIPT_ESCAPE_MS	3000		/* hold BOOT this long: autorun off */
#define SCRIPT_BOOT_GPIO	GPIO_NUM_0

typedef struct {
	char              name[SCR_FNAME_MAX + 1];
	char              args[CMD_LINE_MAX + 1];
	char              fn[SCR_NAME_MAX + 1];
	bool              load;
	bool              system;		/* BOOT watcher on */
	SemaphoreHandle_t done;			/* given when the run ends (NULL: none) */
	scrResult_t      *res;
} scriptJob_t;

typedef struct {
	char text[SCRIPT_EV_TEXT];
	int  src;
	int  timer;				/* TIMER n: n, else 0 */
	int  cnt;
} scriptEv_t;

static QueueHandle_t     s_jobs;
static SemaphoreHandle_t s_taskLock;	/* s_task vs. the job queue */
static TaskHandle_t      s_task;	/* exists only while there are runs (RAM) */
static SemaphoreHandle_t s_sig;		/* "something happened" for the script task */
static SemaphoreHandle_t s_evLock;
static esp_timer_handle_t s_wake;	/* deadline for waits */
static esp_timer_handle_t s_watch;	/* BOOT escape */
static esp_timer_handle_t s_tim[SCR_TIMERS + 1];
static int               s_timMs[SCR_TIMERS + 1];

static EXT_RAM_BSS_ATTR scriptEv_t s_ev[SCR_EVQ];
static int               s_nev, s_dropped;

static volatile bool     s_running, s_paused, s_system;
static volatile bool     s_stopReq, s_pauseReq, s_abort;
static volatile int      s_go = -1;
static volatile bool     s_inputWait, s_inputHave;
static EXT_RAM_BSS_ATTR char s_inputLine[CMD_LINE_MAX + 1];
static int64_t           s_lastYield;
static int64_t           s_watchDown;
static bool              s_waitActive, s_waitInstead;
static EXT_RAM_BSS_ATTR char s_waitDetails[200];

/* USB console while paused with no client */
static EXT_RAM_BSS_ATTR char s_usbLine[CMD_LINE_MAX + 1];
static int               s_usbLen;
static volatile bool     s_usbHave;
static EXT_RAM_BSS_ATTR char s_usbCmd[CMD_LINE_MAX + 1];

static void        scriptTask(void *arg);
static void        scriptWakeCb(void *arg);
static void        scriptWatchCb(void *arg);
static void        scriptTimerCb(void *arg);
static bool        scriptStart(const char *name, const char *args, const char *fn, bool load, bool system,
                               bool wait, scrResult_t *res);
static bool        scriptExists(const char *name);
static bool        nameOk(const char *n, int l);
static bool        scriptPath(const char *name, char *out, size_t n);
static bool        scriptAutorun(void);
static const char *scriptCmdRun(const char *args, cmdReply_t reply);
static const char *scriptCmdLoad(const char *args, cmdReply_t reply);
static const char *scriptCmdStop(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdPause(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdCont(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdStep(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdBpt(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdVars(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdList(int argc, char *argv[], cmdReply_t reply);
static const char *scriptCmdStack(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t scriptInit(void)
{
	s_jobs   = xQueueCreate(2, sizeof(scriptJob_t));
	s_sig    = xSemaphoreCreateBinary();
	s_evLock = xSemaphoreCreateMutex();
	s_taskLock = xSemaphoreCreateMutex();
	if (!s_jobs || !s_sig || !s_evLock || !s_taskLock) {
		return ESP_ERR_NO_MEM;
	}
	esp_timer_create_args_t w = { .callback = scriptWakeCb, .name = "scrwake" };
	esp_timer_create_args_t b = { .callback = scriptWatchCb, .name = "scrboot" };
	if (esp_timer_create(&w, &s_wake) != ESP_OK || esp_timer_create(&b, &s_watch) != ESP_OK) {
		return ESP_ERR_NO_MEM;
	}
	for (int i = 1; i <= SCR_TIMERS; i++) {
		esp_timer_create_args_t t = { .callback = scriptTimerCb, .arg = (void *)(intptr_t)i, .name = "scrtim" };
		if (esp_timer_create(&t, &s_tim[i]) != ESP_OK) {
			return ESP_ERR_NO_MEM;
		}
	}

	esp_err_t err = cmdRegisterRaw("RUN", scriptCmdRun, "RUN [name [args]]  list / start a script");
	if (err == ESP_OK) err = cmdRegisterRaw("LOAD", scriptCmdLoad, "LOAD name [args]  load, pause at the first line");
	if (err == ESP_OK) err = cmdRegister("STOP", 0, 1, scriptCmdStop, "STOP [NOW]  stop the script");
	if (err == ESP_OK) err = cmdRegister("PAUSE", 0, 0, scriptCmdPause, "PAUSE  pause the script");
	if (err == ESP_OK) err = cmdRegister("CONT", 0, 0, scriptCmdCont, "CONT  resume a paused script");
	if (err == ESP_OK) err = cmdRegister("STEP", 0, 1, scriptCmdStep, "STEP [IN|OUT]  run one line (paused)");
	if (err == ESP_OK) err = cmdRegister("BPT", 0, 2, scriptCmdBpt, "BPT [[file:]n | DEL ALL|#n|[file:]n]  breakpoints");
	if (err == ESP_OK) err = cmdRegister("VARS", 0, 0, scriptCmdVars, "VARS  variables (paused)");
	if (err == ESP_OK) err = cmdRegister("LIST", 0, 3, scriptCmdList, "LIST [name [from [to]]]  list a script");
	if (err == ESP_OK) err = cmdRegister("STACK", 0, 0, scriptCmdStack, "STACK  call stack (paused)");
	return err;
}

bool scriptHas(const char *name)
{
	return scriptExists(name);
}

bool scriptRunWait(const char *name, const char *args)
{
	if (s_running) {
		return false;
	}
	return scriptStart(name, args, NULL, false, true, true, NULL);
}

bool scriptActive(void)
{
	return s_running;
}

bool scriptAbortRequested(void)
{
	return s_abort;
}

void scriptConsoleLine(const char *line)
{
	if (s_running && !s_paused) {
		const char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		/* while INPUT waits only !!STOP [NOW] / !!PAUSE are commands (any other
		 * line is the input text); otherwise STOP / PAUSE with or without !! */
		bool bang = p[0] == '!' && p[1] == '!';
		if (bang) {
			p += 2;
		}
		bool ctl = (strncasecmp(p, "STOP", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) ||
		           (strncasecmp(p, "PAUSE", 5) == 0 && (p[5] == '\0' || p[5] == ' ' || p[5] == '\t'));
		if (ctl && !bang && s_inputWait && !s_inputHave) {
			ctl = false;				/* "stop" typed as input text */
		}
		if (ctl) {
			/* run here, not through cmdExecute: its post-command screen update
			 * must not race the script's own drawing */
			char        buf[CMD_LINE_MAX + 1];
			char       *argv[2];
			int         argc = 0;
			snprintf(buf, sizeof(buf), "%s", p);
			char       *save = NULL;
			char       *w    = strtok_r(buf, " \t", &save);
			for (char *a = strtok_r(NULL, " \t", &save); a && argc < 2; a = strtok_r(NULL, " \t", &save)) {
				argv[argc++] = a;
			}
			const char *err = (strcasecmp(w, "STOP") == 0) ? (argc <= 1 ? scriptCmdStop(argc, argv, uiSend)
			                                                             : "usage: STOP [NOW]")
			                                                 : (argc == 0 ? scriptCmdPause(0, argv, uiSend)
			                                                              : "usage: PAUSE");
			char out[80];
			snprintf(out, sizeof(out), err ? "ERR %s\n" : "OK\n", err);
			uiSend(out);
		} else if (s_inputWait && !s_inputHave) {
			snprintf(s_inputLine, sizeof(s_inputLine), "%s", line);
			s_inputHave = true;
			xSemaphoreGive(s_sig);
		} else if (*p) {
			uiSend("ERR script running\n");
		}
		return;
	}
	cmdExecute(line, uiSend);
}

bool scriptEvent(const char *text)
{
	if (!s_running || text[0] != '<' || text[1] != '<') {
		return false;
	}
	const char *t   = text + 2;
	int         src = scrEventSource(t);
	if (!(src & (SCR_SRC_BTN | SCR_SRC_IMU | SCR_SRC_UI))) {
		return false;
	}
	const char *e = strstr(t, ">>");
	int         n = e ? (int)(e - t) : (int)strlen(t);
	xSemaphoreTake(s_evLock, portMAX_DELAY);
	if (s_nev == SCR_EVQ) {
		memmove(&s_ev[0], &s_ev[1], sizeof(s_ev[0]) * (SCR_EVQ - 1));
		s_nev--;
		s_dropped++;
	}
	scriptEv_t *ev = &s_ev[s_nev++];
	snprintf(ev->text, sizeof(ev->text), "%.*s", n, t);
	ev->src   = src;
	ev->timer = 0;
	ev->cnt   = 1;
	xSemaphoreGive(s_evLock);
	xSemaphoreGive(s_sig);
	return scrQuiet() >= 2;			/* QUIET 2, 3: the client does not see it */
}

void scriptBoot(void)
{
	if (scriptAutorun() && scriptExists("sys/onBoot")) {
		ESP_LOGI(TAG, "sys/onBoot.s");
		scriptStart("sys/onBoot", "", NULL, false, true, true, NULL);
	}
}

bool scriptWaitBegin(const char *details)
{
	s_waitActive  = false;
	s_waitInstead = false;
	if (!scriptAutorun() || s_running || !scriptExists("sys/onWait")) {
		return false;
	}
	snprintf(s_waitDetails, sizeof(s_waitDetails), "%s", details);
	s_waitActive = true;
	scriptStart("sys/onWait", details, "before", false, true, true, NULL);
	if (scrHasFn("sys/onWait", "instead", true)) {
		s_waitInstead = true;
		scriptStart("sys/onWait", details, "instead", false, true, false, NULL);
		return true;
	}
	return false;
}

void scriptWaitEnd(void)
{
	if (!s_waitActive) {
		return;
	}
	s_waitActive = false;
	if (s_waitInstead) {
		while (s_running) {				/* 'instead' must exit by itself */
			vTaskDelay(pdMS_TO_TICKS(20));
		}
		s_waitInstead = false;
	}
	if (scriptAutorun()) {
		scriptStart("sys/onWait", s_waitDetails, "after", false, true, true, NULL);
	}
}

void scriptConnect(const char *uiName, void (*builtin)(void))
{
	if (!scriptAutorun() || s_running || !scriptExists("sys/onConnect")) {
		builtin();
		return;
	}
	char arg[32];
	snprintf(arg, sizeof(arg), "UI:%s", uiName);
	scriptStart("sys/onConnect", arg, "before", false, false, true, NULL);
	if (scrHasFn("sys/onConnect", "instead", true)) {
		scriptStart("sys/onConnect", arg, "instead", false, false, true, NULL);
	} else {
		builtin();
	}
	scriptStart("sys/onConnect", arg, "after", false, false, true, NULL);
}

void scriptClientGone(void)
{
	if (!s_running || s_system) {
		return;
	}
	if (s_paused || !scrHasHandler(SCR_SRC_UI)) {
		s_stopReq = true;				/* nobody left to see it */
		s_go      = SCR_GO_STOP;
		xSemaphoreGive(s_sig);
	}
	while (s_running) {
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

/* USB serial bytes while paused without a client: assemble a console line. */
bool scriptUsbRx(const uint8_t *data, size_t len)
{
	if (!s_paused || uiHasClient()) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = data[i];
		if (c == '\r' || c == '\n') {
			serWrite(SER_USB, "\n", 1, true);	/* echo */
			if (s_usbLen && !s_usbHave) {
				s_usbLine[s_usbLen] = '\0';
				snprintf(s_usbCmd, sizeof(s_usbCmd), "%s", s_usbLine);
				s_usbHave = true;
				xSemaphoreGive(s_sig);
			}
			s_usbLen = 0;
		} else if ((c == 0x08 || c == 0x7F) && s_usbLen) {
			s_usbLen--;
			serWrite(SER_USB, "\b \b", 3, false);
		} else if (c >= ' ' && c <= '~' && s_usbLen < CMD_LINE_MAX) {
			s_usbLine[s_usbLen++] = c;
			serWrite(SER_USB, &c, 1, false);
		}
	}
	return true;
}

/* ================= Internals ================= */

static bool scriptAutorun(void)
{
	return nvGetInt("sys.autorun") == 1;
}

/* "name" -> SD:/scripts/name.s, "sys/name" -> SD:/sys/name.s. false = bad name. */
static bool scriptPath(const char *name, char *out, size_t n)
{
	if (!nameOk(name, strlen(name))) {
		return false;
	}
	if (strncasecmp(name, "sys/", 4) == 0) {
		snprintf(out, n, "%s/sys/%s.s", SD_MOUNT_POINT, name + 4);
	} else {
		snprintf(out, n, "%s/%s.s", SCRIPT_DIR, name);
	}
	return true;
}

static bool scriptExists(const char *name)
{
	char        path[sizeof(SCRIPT_DIR) + SCR_FNAME_MAX + 8];
	struct stat st;
	if (!scriptPath(name, path, sizeof(path))) {
		return false;
	}
	sdLock();
	bool ok = sdEnsureMounted() == ESP_OK && stat(path, &st) == 0 && S_ISREG(st.st_mode);
	sdUnlock();
	return ok;
}

/* Queue a run; wait: block until it ends. */
static bool scriptStart(const char *name, const char *args, const char *fn, bool load, bool system,
                        bool wait, scrResult_t *res)
{
	scriptJob_t job = { .load = load, .system = system, .res = res };
	snprintf(job.name, sizeof(job.name), "%s", name);
	snprintf(job.args, sizeof(job.args), "%s", args ? args : "");
	snprintf(job.fn, sizeof(job.fn), "%s", fn ? fn : "");
	if (wait) {
		job.done = xSemaphoreCreateBinary();
		if (!job.done) {
			return false;
		}
	}
	s_running = true;				/* from now: console refuses, events queue */
	xSemaphoreTake(s_taskLock, portMAX_DELAY);
	bool ok = (xQueueSend(s_jobs, &job, 0) == pdTRUE);
	if (ok && !s_task &&
	    xTaskCreate(scriptTask, "script", SCRIPT_TASK_STACK, NULL, SCRIPT_TASK_PRIO, &s_task) != pdPASS) {
		xQueueReset(s_jobs);			/* no RAM for the task */
		s_task = NULL;
		ok     = false;
		ESP_LOGE(TAG, "no memory for the script task");
	}
	xSemaphoreGive(s_taskLock);
	if (!ok) {
		s_running = false;
		if (job.done) {
			vSemaphoreDelete(job.done);
		}
		return false;
	}
	if (wait) {
		xSemaphoreTake(job.done, portMAX_DELAY);
		vSemaphoreDelete(job.done);
	}
	return true;
}

/* Runs the queued jobs, then deletes itself (its stack is only used while
 * scripts run). */
static void scriptTask(void *arg)
{
	scriptJob_t job;
	for (;;) {
		xSemaphoreTake(s_taskLock, portMAX_DELAY);
		if (xQueueReceive(s_jobs, &job, 0) != pdTRUE) {
			s_task = NULL;
			xSemaphoreGive(s_taskLock);
			vTaskDelete(NULL);
		}
		xSemaphoreGive(s_taskLock);
		xSemaphoreTake(s_evLock, portMAX_DELAY);
		s_nev     = 0;
		s_dropped = 0;
		xSemaphoreGive(s_evLock);
		s_stopReq   = false;
		s_pauseReq  = false;
		s_abort     = false;
		s_go        = -1;
		s_inputWait = s_inputHave = false;
		s_system    = job.system;
		s_running   = true;
		s_lastYield = esp_timer_get_time() / 1000;
		xSemaphoreTake(s_sig, 0);
		if (job.system) {
			s_watchDown = 0;
			gpio_set_pull_mode(SCRIPT_BOOT_GPIO, GPIO_PULLUP_ONLY);
			esp_timer_start_periodic(s_watch, SCRIPT_WATCH_MS * 1000);
		}

		scrResult_t r = scrRun(job.name, job.args, job.fn[0] ? job.fn : NULL, job.load);

		if (job.system) {
			esp_timer_stop(s_watch);
		}
		scrHostTimer(0, 0);
		s_paused  = false;
		s_abort   = false;
		s_system  = false;
		s_running = false;
		if (job.res) {
			*job.res = r;
		}
		if (job.done) {
			xSemaphoreGive(job.done);
		}
	}
}

static void scriptWakeCb(void *arg)
{
	xSemaphoreGive(s_sig);
}

/* BOOT held 3 s while a system script runs: autorun off, reboot. */
static void scriptWatchCb(void *arg)
{
	int64_t now = esp_timer_get_time() / 1000;
	if (gpio_get_level(SCRIPT_BOOT_GPIO) != 0) {
		s_watchDown = 0;
		return;
	}
	if (!s_watchDown) {
		s_watchDown = now;
	} else if (now - s_watchDown >= SCRIPT_ESCAPE_MS) {
		const char *k[] = { "sys.autorun" }, *v[] = { "0" };
		nvSetMany(1, k, v, NULL);
		ESP_LOGW(TAG, "BOOT held: sys.autorun 0, rebooting");
		esp_restart();
	}
}

/* Timer tick: one queued event per timer, merged ("TIMER n cnt"). */
static void scriptTimerCb(void *arg)
{
	int n = (int)(intptr_t)arg;
	xSemaphoreTake(s_evLock, portMAX_DELAY);
	bool merged = false;
	for (int i = 0; i < s_nev; i++) {
		if (s_ev[i].timer == n) {
			s_ev[i].cnt++;
			merged = true;
		}
	}
	if (!merged) {
		if (s_nev == SCR_EVQ) {
			memmove(&s_ev[0], &s_ev[1], sizeof(s_ev[0]) * (SCR_EVQ - 1));
			s_nev--;
			s_dropped++;
		}
		scriptEv_t *ev = &s_ev[s_nev++];
		snprintf(ev->text, sizeof(ev->text), "TIMER %d", n);
		ev->src   = SCR_SRC_TIMER;
		ev->timer = n;
		ev->cnt   = 1;
	}
	xSemaphoreGive(s_evLock);
	xSemaphoreGive(s_sig);
}

/* Wait for s_sig, at most until deadline (ms since boot, < 0 = none). */
static void scriptWaitSig(int64_t deadline)
{
	if (deadline >= 0) {
		int64_t left = deadline - esp_timer_get_time() / 1000;
		if (left <= 0) {
			return;
		}
		esp_timer_stop(s_wake);
		esp_timer_start_once(s_wake, (uint64_t)left * 1000);
	}
	xSemaphoreTake(s_sig, portMAX_DELAY);
	if (deadline >= 0) {
		esp_timer_stop(s_wake);
	}
}

static void usbReply(const char *text)
{
	serWrite(SER_USB, text, strlen(text), true);
}

/* ================= Host hooks (scr_host.h) ================= */

char *scrHostRead(const char *name, size_t *len, const char **err)
{
	char        path[sizeof(SCRIPT_DIR) + SCR_FNAME_MAX + 8];
	struct stat st;
	if (!scriptPath(name, path, sizeof(path))) {
		*err = "bad file name (letters, digits, '_'; system scripts sys/name)";
		return NULL;
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		*err = "no SD card";
		return NULL;
	}
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		sdUnlock();
		*err = "file not found";
		return NULL;
	}
	if (st.st_size > SCR_FILE_MAX) {
		sdUnlock();
		*err = "file too big (max 32 KB)";
		return NULL;
	}
	char *buf = scrHostAlloc(st.st_size + 1);
	FILE *f   = buf ? fopen(path, "rb") : NULL;
	size_t n  = f ? fread(buf, 1, st.st_size, f) : 0;
	if (f) {
		fclose(f);
	}
	sdUnlock();
	if (!buf || n != (size_t)st.st_size) {
		scrHostFree(buf);
		*err = buf ? "read failed" : "out of memory";
		return NULL;
	}
	buf[n] = '\0';
	*len   = n;
	return buf;
}

void *scrHostAlloc(size_t n)
{
	void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
	return p ? p : calloc(1, n);
}

void *scrHostRealloc(void *p, size_t n)
{
	void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
	return q ? q : realloc(p, n);
}

void scrHostFree(void *p)
{
	free(p);
}

static void (*s_cmdCb)(void *ctx, const char *text);
static void *s_cmdCtx;

static void cmdTrampoline(const char *text)
{
	s_cmdCb(s_cmdCtx, text);
}

void scrHostCmd(const char *line, void (*cb)(void *ctx, const char *text), void *ctx)
{
	s_cmdCb  = cb;
	s_cmdCtx = ctx;
	cmdExecute(line, cmdTrampoline);
}

bool scrHostIsCommand(const char *name)
{
	return cmdExists(name);
}

void scrHostPrint(const char *text)
{
	if (s_paused && !uiHasClient()) {
		usbReply(text);
		return;
	}
	uiSendText(text);				/* not taken as an event */
}

void scrHostDebug(const char *text)
{
	dbgf("%s", text);
}

void scrHostNotify(const char *text)
{
	if (!uiHasClient()) {
		ESP_LOGI(TAG, "%.*s", (int)strcspn(text, "\n"), text);
		if (s_paused || strstr(text, " PAUSED ")) {
			usbReply(text);			/* USB is the console while paused */
		}
		return;
	}
	uiSendText(text);
}

bool scrHostNv(const char *key, char *out, size_t n)
{
	const char *v = nvGet(key);
	if (!v) {
		return false;
	}
	snprintf(out, n, "%s", v);
	return true;
}

bool scrHostBuiltin(const char *name, char *out, size_t n, const char **err)
{
	float x, y, z;
	if (!strcasecmp(name, "ax") || !strcasecmp(name, "ay") || !strcasecmp(name, "az") ||
	    !strcasecmp(name, "gx") || !strcasecmp(name, "gy") || !strcasecmp(name, "gz")) {
		esp_err_t e = (name[0] == 'a' || name[0] == 'A') ? imuReadAccel(&x, &y, &z) : imuReadGyro(&x, &y, &z);
		if (e != ESP_OK) {
			*err = "IMU read failed";
			return false;
		}
		char  c = name[1] | 0x20;
		snprintf(out, n, "%.4f", c == 'x' ? x : c == 'y' ? y : z);
		return true;
	}
	if (!strcasecmp(name, "temp")) {
		if (imuReadTemp(&x) != ESP_OK) {
			*err = "IMU read failed";
			return false;
		}
		snprintf(out, n, "%.2f", x);
		return true;
	}
	if (!strcasecmp(name, "bat")) {
		if (batRead(&x) != ESP_OK) {
			*err = "battery read failed";
			return false;
		}
		snprintf(out, n, "%.3f", x);
		return true;
	}
	if (!strcasecmp(name, "steps")) {
		snprintf(out, n, "0");			/* IMU STEP not implemented */
		return true;
	}
	if (!strcasecmp(name, "orient")) {
		snprintf(out, n, "%d", imuOrientLast());
		return true;
	}
	if (!strcasecmp(name, "time")) {
		sysTimestamp(out, n);
		return true;
	}
	if (!strcasecmp(name, "uptime")) {
		snprintf(out, n, "%lld", (long long)(esp_timer_get_time() / 1000));
		return true;
	}
	if (!strcasecmp(name, "w") || !strcasecmp(name, "h") || !strcasecmp(name, "rot")) {
		int v = (name[0] | 0x20) == 'w' ? lcdWidth() : (name[0] | 0x20) == 'h' ? lcdHeight() : lcdGetRotation();
		snprintf(out, n, "%d", v);
		return true;
	}
	if (!strcasecmp(name, "fw")) {
		snprintf(out, n, "%s", esp_app_get_description()->version);
		return true;
	}
	return false;
}

const char *scrHostImu(bool gyro, double v[3])
{
	float x, y, z;
	esp_err_t e = gyro ? imuReadGyro(&x, &y, &z) : imuReadAccel(&x, &y, &z);
	if (e != ESP_OK) {
		return "IMU read failed";
	}
	v[0] = x;
	v[1] = y;
	v[2] = z;
	return NULL;
}

uint32_t scrHostTrng(void)
{
	return esp_random();
}

/* RNG PRNG: the user's generator. Replace this (weak) one with your own. */
__attribute__((weak)) uint32_t scrPrng(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

int scrHostFilesDefault(void)
{
	return nvGetInt("run.files");
}

int64_t scrHostMs(void)
{
	return esp_timer_get_time() / 1000;
}

int scrHostCheck(void)
{
	int64_t now = esp_timer_get_time() / 1000;
	if (now - s_lastYield >= SCR_YIELD_MS) {
		vTaskDelay(1);				/* let the idle task run (watchdog) */
		s_lastYield = esp_timer_get_time() / 1000;
	}
	int r = 0;
	if (s_stopReq) {
		r |= SCR_CHK_STOP;
	}
	if (s_pauseReq) {
		s_pauseReq = false;
		r |= SCR_CHK_PAUSE;
	}
	return r;
}

bool scrHostSleep(int ms)
{
	int64_t end = esp_timer_get_time() / 1000 + ms;
	while (!s_stopReq) {
		int64_t now = esp_timer_get_time() / 1000;
		if (now >= end) {
			return true;
		}
		scriptWaitSig(end);
	}
	return false;
}

static int takeEvent(int mask, char *out, size_t n)
{
	int got = 0;
	xSemaphoreTake(s_evLock, portMAX_DELAY);
	for (int i = 0; i < s_nev; i++) {
		if (s_ev[i].src & mask) {
			if (s_ev[i].timer && s_ev[i].cnt > 1) {
				snprintf(out, n, "TIMER %d %d", s_ev[i].timer, s_ev[i].cnt);
			} else {
				snprintf(out, n, "%s", s_ev[i].text);
			}
			memmove(&s_ev[i], &s_ev[i + 1], sizeof(s_ev[0]) * (s_nev - i - 1));
			s_nev--;
			got = 1;
			break;
		}
	}
	xSemaphoreGive(s_evLock);
	return got;
}

int scrHostEvent(int mask, char *out, size_t n, int ms)
{
	int64_t end = ms < 0 ? -1 : esp_timer_get_time() / 1000 + ms;
	for (;;) {
		if (s_stopReq) {
			return -1;
		}
		if (takeEvent(mask, out, n)) {
			return 1;
		}
		if (ms == 0 || (end >= 0 && esp_timer_get_time() / 1000 >= end)) {
			return 0;
		}
		scriptWaitSig(end);
	}
}

int scrHostEventsDropped(void)
{
	return s_dropped;
}

void scrHostTimer(int n, int ms)
{
	if (n == 0) {
		for (int i = 1; i <= SCR_TIMERS; i++) {
			esp_timer_stop(s_tim[i]);
			s_timMs[i] = 0;
		}
		return;
	}
	esp_timer_stop(s_tim[n]);
	s_timMs[n] = ms;
	xSemaphoreTake(s_evLock, portMAX_DELAY);
	for (int i = 0; i < s_nev; i++) {
		if (s_ev[i].timer == n) {		/* a stopped / restarted timer's tick goes */
			memmove(&s_ev[i], &s_ev[i + 1], sizeof(s_ev[0]) * (s_nev - i - 1));
			s_nev--;
			i--;
		}
	}
	xSemaphoreGive(s_evLock);
	if (ms > 0) {
		esp_timer_start_periodic(s_tim[n], (uint64_t)ms * 1000);
	}
}

int scrHostInput(char *out, size_t n, int ms)
{
	if (!uiHasClient()) {
		scriptEvent("<<UI DISCONNECT>>\n");
		return -1;
	}
	int64_t end = ms < 0 ? -1 : esp_timer_get_time() / 1000 + ms;
	s_inputHave = false;
	s_inputWait = true;
	int r = 0;
	for (;;) {
		if (s_stopReq) {
			r = -2;
			break;
		}
		if (s_inputHave) {
			snprintf(out, n, "%s", s_inputLine);
			r = 1;
			break;
		}
		if (end >= 0 && esp_timer_get_time() / 1000 >= end) {
			break;
		}
		scriptWaitSig(end);
	}
	s_inputWait = false;
	s_inputHave = false;
	return r;
}

int scrHostPause(void)
{
	s_go     = -1;
	s_paused = true;
	for (;;) {
		if (s_stopReq) {
			s_go = SCR_GO_STOP;
		}
		if (s_go >= 0) {
			break;
		}
		if (s_usbHave) {				/* USB console: run the line here */
			char line[CMD_LINE_MAX + 1];
			snprintf(line, sizeof(line), "%s", s_usbCmd);
			s_usbHave = false;
			cmdExecute(line, usbReply);
			continue;
		}
		scriptWaitSig(-1);
	}
	s_paused = false;
	int go = s_go;
	s_go = -1;
	return go;
}

/* ================= Data files ================= */

#define SCRIPT_DATA_DIR		SCRIPT_DIR "/data"

/* "name.txt" / "name.var" -> SD:/scripts/data/..., false = bad name */
static bool dataPath(const char *fname, char *out, size_t n)
{
	const char *dot = strrchr(fname, '.');
	if (!dot || (strcasecmp(dot, ".txt") != 0 && strcasecmp(dot, ".var") != 0) ||
	    !nameOk(fname, dot - fname) || strchr(fname, '/')) {
		return false;
	}
	snprintf(out, n, "%s/%s", SCRIPT_DATA_DIR, fname);
	return true;
}

long scrHostFileSize(const char *fname, bool *ro, const char **err)
{
	char        path[sizeof(SCRIPT_DATA_DIR) + SCR_FNAME_MAX + 8];
	struct stat st;
	if (!dataPath(fname, path, sizeof(path))) {
		*err = "bad file name";
		return -1;
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		*err = "no SD card";
		return -1;
	}
	bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode);
	sdUnlock();
	if (!ok) {
		return -1;					/* no such file */
	}
	*ro = !(st.st_mode & S_IWUSR);
	return (long)st.st_size;
}

int scrHostFileRead(const char *fname, long off, char *buf, int n, const char **err)
{
	char path[sizeof(SCRIPT_DATA_DIR) + SCR_FNAME_MAX + 8];
	if (!dataPath(fname, path, sizeof(path))) {
		*err = "bad file name";
		return -1;
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		*err = "no SD card";
		return -1;
	}
	FILE *f   = fopen(path, "rb");
	int   got = -1;
	if (f) {
		got = (fseek(f, off, SEEK_SET) == 0) ? (int)fread(buf, 1, n, f) : 0;
		if (ferror(f)) {
			got = -1;
		}
		fclose(f);
	}
	sdUnlock();
	if (got < 0) {
		*err = f ? "read failed" : "no such file";
	}
	return got;
}

const char *scrHostFileWrite(const char *fname, long off, const char *data, int n, bool trunc)
{
	char        path[sizeof(SCRIPT_DATA_DIR) + SCR_FNAME_MAX + 8];
	struct stat st;
	if (!dataPath(fname, path, sizeof(path))) {
		return "bad file name";
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	const char *err    = "write failed";
	bool        exists = stat(path, &st) == 0 && S_ISREG(st.st_mode);
	if (exists && !(st.st_mode & S_IWUSR)) {
		err = "read-only";
	} else if ((mkdir(SCRIPT_DIR, 0775) == 0 || errno == EEXIST) &&
	           (mkdir(SCRIPT_DATA_DIR, 0775) == 0 || errno == EEXIST)) {
		FILE *f = fopen(path, (trunc || !exists) ? "w+b" : "r+b");
		if (f) {
			bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(data, 1, n, f) == (size_t)n;
			if (fclose(f) == 0 && ok) {
				err = NULL;
			}
		}
	}
	sdUnlock();
	return err;
}

/* ================= Commands ================= */

static bool nameOk(const char *n, int l)
{
	if (l > 4 && strncasecmp(n, "sys/", 4) == 0) {
		n += 4;					/* system script: SD:/sys/name.s */
		l -= 4;
	}
	if (l <= 0 || l > SCR_FNAME_MAX) {
		return false;
	}
	for (int i = 0; i < l; i++) {
		char c = n[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	return true;
}

static const char *runOrLoad(const char *args, cmdReply_t reply, bool load)
{
	while (*args == ' ' || *args == '\t') {
		args++;
	}
	if (!*args) {
		if (load) {
			return "usage: LOAD name [args]";
		}
		static EXT_RAM_BSS_ATTR char names[SCRIPT_LIST_MAX][SD_NAME_MAX];
		sdLock();
		int n = (sdEnsureMounted() == ESP_OK) ? sdListFiles(SCRIPT_DIR, ".s", names, SCRIPT_LIST_MAX) : -1;
		sdUnlock();
		if (n < 0) {
			return "no SD card";
		}
		for (int i = 0; i < n; i++) {
			names[i][strlen(names[i]) - 2] = '\0';
			char line[SD_NAME_MAX + 2];
			snprintf(line, sizeof(line), "%s\n", names[i]);
			reply(line);
		}
		if (n == 0) {
			reply("no scripts in /scripts\n");
		}
		return NULL;
	}
	if (s_running) {
		return "script running";
	}
	int l = 0;
	while (args[l] && args[l] != ' ' && args[l] != '\t') {
		l++;
	}
	char name[SCR_FNAME_MAX + 8];			/* "sys/" + name + ".s" */
	snprintf(name, sizeof(name), "%.*s", l > SCR_FNAME_MAX + 4 ? SCR_FNAME_MAX + 4 : l, args);
	int nl = strlen(name);
	if (nl > 2 && strcasecmp(name + nl - 2, ".s") == 0) {
		name[nl -= 2] = '\0';
	}
	if (!nameOk(name, nl)) {
		return "name: letters, digits, '_' (max 31; system scripts: sys/name)";
	}
	const char *a = args + l;
	while (*a == ' ' || *a == '\t') {
		a++;
	}
	if (!scriptStart(name, a, NULL, load, false, false, NULL)) {
		return "could not start";
	}
	return NULL;
}

static const char *scriptCmdRun(const char *args, cmdReply_t reply)
{
	return runOrLoad(args, reply, false);
}

static const char *scriptCmdLoad(const char *args, cmdReply_t reply)
{
	return runOrLoad(args, reply, true);
}

static const char *scriptCmdStop(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 1 && strcasecmp(argv[0], "NOW") != 0) {
		return "usage: STOP [NOW]";
	}
	if (!s_running) {
		return "no script running";
	}
	if (argc == 1) {
		s_abort = true;
	}
	s_stopReq = true;
	s_go      = SCR_GO_STOP;
	xSemaphoreGive(s_sig);
	return NULL;
}

static const char *scriptCmdPause(int argc, char *argv[], cmdReply_t reply)
{
	if (!s_running) {
		return "no script running";
	}
	if (s_paused) {
		return "already paused";
	}
	s_pauseReq = true;
	return NULL;
}

static const char *scriptGo(int mode)
{
	if (!s_running || !s_paused) {
		return "not paused";
	}
	s_go = mode;
	xSemaphoreGive(s_sig);
	return NULL;
}

static const char *scriptCmdCont(int argc, char *argv[], cmdReply_t reply)
{
	return scriptGo(SCR_GO_CONT);
}

static const char *scriptCmdStep(int argc, char *argv[], cmdReply_t reply)
{
	int mode = SCR_GO_STEP;
	if (argc == 1) {
		if (strcasecmp(argv[0], "IN") == 0) {
			mode = SCR_GO_IN;
		} else if (strcasecmp(argv[0], "OUT") == 0) {
			mode = SCR_GO_OUT;
		} else {
			return "usage: STEP [IN|OUT]";
		}
	}
	return scriptGo(mode);
}

static const char *scriptCmdBpt(int argc, char *argv[], cmdReply_t reply)
{
	return scrCmdBpt(argc, argv, reply);
}

static const char *scriptCmdVars(int argc, char *argv[], cmdReply_t reply)
{
	return s_paused ? scrCmdVars(reply) : "no script paused";
}

static const char *scriptCmdList(int argc, char *argv[], cmdReply_t reply)
{
	return scrCmdList(argc, argv, reply);
}

static const char *scriptCmdStack(int argc, char *argv[], cmdReply_t reply)
{
	return s_paused ? scrCmdStack(reply) : "no script paused";
}
