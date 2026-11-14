#include "ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "ble_serial.h"
#include "cmd.h"
#include "telnet.h"
#include "web.h"
#include "wifi_ap.h"
#include "nv.h"
#include "lcd.h"
#include "uiscr.h"
#include "log.h"
#include "script.h"
#include "fm.h"
#include "usbdisk.h"
#include "reset.h"
#include "sd.h"

static const char *TAG = "ui";

#define UI_CHUNK		64	/* bytes per queued chunk */
#define UI_QUEUE_DEPTH		32
#define UI_SWITCH_DELAY_MS	300	/* let the reply reach the client before switching */
#define UI_SWITCH_STACK		6144	/* Wi-Fi start-up needs a fair bit */
#define UI_SWITCH_PRIO		5
#define UI_SWITCH_NOW		0x100	/* uiSwitchTask arg flag: no reply to wait for */
#define UI_FEED_WAIT_MS		100	/* telnet back-pressure re-check interval */
#define UI_BOOT_GPIO		GPIO_NUM_0	/* BOOT button: shows the Wi-Fi password */
#define UI_BOOT_POLL_US		50000
#define UI_SER_ESC_MS		1000	/* "+++" / "---" then this long with no input */
#define UI_SER_BANNER_ENTERS	3	/* empty lines in a row -> banner */
#define UI_RST_CONFIRM_MS	5000	/* RESET!: second long press within this of the first release */

/* Queue item kinds: data, or screen events handled in the main task */
enum {
	UI_EV_DATA = 0,
	UI_EV_START,		/* waiting for a client: details screen */
	UI_EV_CONNECT,		/* client connected: splash */
	UI_EV_BUTTON,		/* BOOT long press down/up: password */
	UI_EV_NEXT,		/* BOOT double press: next interface */
	UI_EV_LINK,		/* Wi-Fi link up/down (station with IP / none left) */
	UI_EV_CALL,		/* uiRunMain(): data = function + argument */
	UI_EV_FMSTOP,		/* BOOT short press on the SD manager screen */
	UI_EV_MSC,		/* BOOT held on the MSC Wait screen: start disk mode */
	UI_EV_RSTTAP,		/* RESET! screen: tap = next mode */
	UI_EV_RSTARM,		/* RESET!: first long press */
	UI_EV_RSTGO,		/* RESET!: second long press - do it */
	UI_EV_RSTCANCEL,	/* RESET!: tap or time-out after the first long press */
};

typedef struct {
	uiMainFn_t fn;
	int        arg;
} uiCall_t;

#define UI_STR_(x)		#x
#define UI_STR(x)		UI_STR_(x)

/* Received data, tagged with the session it belongs to */
typedef struct {
	uint32_t session;
	uint8_t  kind;		/* UI_EV_* */
	uint8_t  len;
	char     data[UI_CHUNK];
} uiChunk_t;

static QueueHandle_t     s_rxQueue;
static SemaphoreHandle_t s_switchLock;
static SemaphoreHandle_t s_sendLock;	/* whole messages: replies vs button events */
static volatile uiIf_t   s_active = UI_NONE;
static volatile uint32_t s_session;
static volatile bool     s_switching;
static esp_timer_handle_t s_bootPoll;	/* BOOT button poll while the Wait screen is up */
static volatile bool     s_pwdShown;	/* BOOT long press held */
static bool              s_bootDown;	/* poll state (esp_timer task only) */
static bool              s_bootLong;
static int64_t           s_bootPress, s_bootRelease;
static int               s_bootLongMs, s_bootDblMs;
static volatile bool     s_link;		/* a Wi-Fi station has an IP */
static bool              s_client;	/* a client is connected (main task) */

/* RESET! screen */
static resetMode_t       s_rstMode;	/* main task */
static bool              s_rstNoSd;	/* main task: no card -> NVRAM without backup */
static bool              s_rstArmed;	/* poll: first long press done */
static bool              s_rstTap;	/* poll: a tap waits for the double-press window */
static int64_t           s_rstArmedAt;	/* poll: first long press released (ms) */
static volatile bool     s_rstBusy;	/* main task is carrying it out */

/* EVENTS: which <<src ...>> messages the client receives (this boot) */
static const char *const s_evNames[] = { "BTN", "IMU", "UI", "SCRIPT" };
#define UI_EV_SOURCES		(int)(sizeof(s_evNames) / sizeof(s_evNames[0]))
static volatile uint8_t  s_evMute;	/* bit i set = s_evNames[i] muted */

/* serial UI line editing (serial task only, except the reset in uiStart) */
static bool              s_serConn;	/* first line seen: client "connected" */
static bool              s_serEcho;	/* NV ser.echo */
static EXT_RAM_BSS_ATTR char s_serLine[CMD_LINE_MAX + 1];
static size_t            s_serLen;
static bool              s_serLastCr;	/* CR seen: a following LF is the same Enter */
static int               s_serEmpty;	/* empty lines in a row */
static bool              s_serEsc;	/* the line is exactly +++ or --- */
static int64_t           s_serEscAt;	/* ms when it became so */

/* line assembly state (reader side only) */
static uiChunk_t s_chunk;
static size_t    s_chunkPos;
static bool      s_haveChunk;
static uint32_t  s_lineSession;
static EXT_RAM_BSS_ATTR char s_line[CMD_LINE_MAX + 1];
static size_t    s_lineLen;

static void        uiFeed(uiIf_t src, const uint8_t *data, size_t len);
static void        uiNewSession(void);
static esp_err_t   uiStart(uiIf_t ifc);
static void        uiStop(uiIf_t ifc);
static const char *uiName(uiIf_t ifc);
static void        uiBleRx(const uint8_t *data, size_t len) __attribute__((unused));	/* unused in DEBUG boot */
static void        uiBleConn(bool connected) __attribute__((unused));
static void        uiTelnetRx(const uint8_t *data, size_t len);
static void        uiTelnetConn(bool connected);
static bool        uiWebRx(const uint8_t *data, size_t len);
static void        uiWebConn(bool connected);
static void        uiSwitchTask(void *arg);
static void        uiPost(uint8_t kind);
static void        uiEvent(uint8_t kind);
static void        uiStartScreen(bool afterClient);
static void        uiBootWatch(void);
static void        uiFmScreen(void);
static void        uiBootPoll(void *arg);
static void        uiNextIf(void);
static void        uiWifiLink(bool up);
static const char *uiCmd(int argc, char *argv[], cmdReply_t reply);
static const char *uiCmdEvents(int argc, char *argv[], cmdReply_t reply);
static uiIf_t      uiIfNext(uiIf_t ifc);
static bool        uiEvMuted(const char *text);
static uiIf_t      uiBootIf(void);
static int         uiSerPort(uiIf_t ifc);
static void        uiSerEnter(serPort_t port);
static void        uiSerTick(void);
static void        uiSerBanner(serPort_t port);
static void        uiWaitDetails(char *out, size_t n);
static void        uiResetPoll(bool down, int64_t now);
static void        uiResetEvent(uint8_t kind);
static void        uiResetMsg(const char *a, const char *b);

/* ================= Public API ================= */

esp_err_t uiInit(void)
{
	s_rxQueue    = xQueueCreate(UI_QUEUE_DEPTH, sizeof(uiChunk_t));
	s_switchLock = xSemaphoreCreateMutex();
	s_sendLock   = xSemaphoreCreateMutex();
	if (!s_rxQueue || !s_switchLock || !s_sendLock) {
		return ESP_ERR_NO_MEM;
	}

	wifiApOnLink(uiWifiLink);

	esp_err_t err = bleInit(uiBleRx, uiBleConn);		/* stack up, not advertising */
	if (err != ESP_OK) {
		return err;
	}
	bleDisable();
	s_active = UI_NONE;					/* interface starts in uiBegin() */

	esp_timer_create_args_t poll = { .callback = uiBootPoll, .name = "uiboot" };
	if (esp_timer_create(&poll, &s_bootPoll) != ESP_OK) {
		return ESP_ERR_NO_MEM;
	}
	uiScrBoot();					/* black, backlight, boot square */

	err = cmdRegister("UI", 0, 1, uiCmd, "UI [BLE|TELNET|WEB|UART|USB|NEXT]  show / switch interface");
	if (err == ESP_OK) {
		err = cmdRegister("EVENTS", 0, 2, uiCmdEvents, "EVENTS [BTN|IMU|UI|SCRIPT|ALL 0|1]  events sent to the client");
	}
	return err;
}

void uiBegin(void)
{
	uiIf_t boot = uiBootIf();				/* NV sys.ui */
	if (uiStart(boot) == ESP_OK) {
		s_active = boot;
	} else {
		ESP_LOGE(TAG, "%s start failed, using BLE", uiName(boot));
		if (uiStart(UI_BLE) == ESP_OK) {
			s_active = UI_BLE;
		}
	}
	ESP_LOGI(TAG, "active: %s", uiName(s_active));
	uiStartScreen(false);				/* wait: details for the boot interface */

	int port = uiSerPort(s_active);
	if (port >= 0) {
		uiSerBanner((serPort_t)port);		/* last start-up line */
	}
}

/* Serial task: bytes from a port (len 0 = tick). Line editing for the serial
 * UIs happens here, so echo is immediate; finished lines go to the queue. */
void uiSerRx(serPort_t port, const uint8_t *data, size_t len)
{
	if (len == 0) {
		uiSerTick();
		return;
	}
	if (uiSerPort(s_active) != (int)port) {
		if (port == SER_USB) {
			scriptUsbRx(data, len);		/* console for a paused script without a client */
		}
		return;						/* not the UI: ignore input */
	}
	for (size_t i = 0; i < len; i++) {
		char c = (char)data[i];
		if (c == '\n' && s_serLastCr) {
			s_serLastCr = false;			/* CR+LF = one Enter */
			continue;
		}
		s_serLastCr = (c == '\r');
		if (c == '\r' || c == '\n') {
			uiSerEnter(port);
			continue;
		}
		if (c == 0x08 || c == 0x7F) {			/* Backspace / DEL */
			if (s_serLen > 0) {
				s_serLen--;
				if (s_serEcho) {
					serWrite(port, "\b \b", 3, false);
				}
			}
		} else {
			if (c == '\t') {
				c = ' ';
			}
			if (c < ' ' || c > '~' || s_serLen >= CMD_LINE_MAX) {
				continue;			/* control / non-ASCII / too long */
			}
			s_serLine[s_serLen++] = c;
			if (s_serEcho) {
				serWrite(port, &c, 1, false);
			}
		}
		s_serEsc   = (s_serLen == 3 && (memcmp(s_serLine, "+++", 3) == 0 || memcmp(s_serLine, "---", 3) == 0));
		s_serEscAt = esp_timer_get_time() / 1000;
	}
}

void uiReadLine(char *line, size_t max)
{
	size_t cap = (max - 1 < CMD_LINE_MAX) ? max - 1 : CMD_LINE_MAX;

	for (;;) {
		if (!s_haveChunk) {
			xQueueReceive(s_rxQueue, &s_chunk, portMAX_DELAY);	/* blocks, no CPU */
			if (s_chunk.session != s_session) {
				continue;				/* stale: old session */
			}
			if (s_chunk.kind == UI_EV_CALL) {
				uiCall_t call;
				memcpy(&call, s_chunk.data, sizeof(call));
				call.fn(call.arg);			/* in this (main) task */
				continue;
			}
			if (s_chunk.kind != UI_EV_DATA) {
				uiEvent(s_chunk.kind);			/* screen event, in this (main) task */
				continue;
			}
			if (s_chunk.session != s_lineSession) {
				s_lineLen     = 0;			/* new session: drop partial line */
				s_lineSession = s_chunk.session;
			}
			s_chunkPos  = 0;
			s_haveChunk = true;
		}

		while (s_chunkPos < s_chunk.len) {
			char c = s_chunk.data[s_chunkPos++];
			if (c == '\r') {
				continue;
			}
			if (c != '\n') {
				s_line[s_lineLen++] = c;
			}
			if (c == '\n' || s_lineLen == cap) {		/* end of line, or overlong */
				memcpy(line, s_line, s_lineLen);
				line[s_lineLen] = '\0';
				s_lineLen = 0;
				return;
			}
		}
		s_haveChunk = false;
	}
}

void uiSend(const char *text)
{
	if (scriptEvent(text)) {			/* <<BTN|IMU|UI ...>> for a running script */
		return;					/* its QUIET 2/3 hides events from the client */
	}
	uiSendText(text);
}

bool uiHasClient(void)
{
	return s_client;
}

void uiSendText(const char *text)
{
	esp_err_t err;
	size_t    len = strlen(text);

	if (uiEvMuted(text)) {
		return;						/* EVENTS src 0 */
	}
	xSemaphoreTake(s_sendLock, portMAX_DELAY);
	switch (s_active) {
	case UI_BLE:    err = bleSend((const uint8_t *)text, len); break;
	case UI_TELNET: err = telnetSend((const uint8_t *)text, len); break;
	case UI_WEB:    err = webSend((const uint8_t *)text, len); break;
	case UI_UART:   serWrite(SER_UART, text, len, true); err = ESP_OK; break;
	case UI_USB:    serWrite(SER_USB, text, len, true); err = ESP_OK; break;
	default:        err = ESP_OK; break;
	}
	xSemaphoreGive(s_sendLock);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "reply not sent (%s)", esp_err_to_name(err));
	}
}

esp_err_t uiSelect(uiIf_t ifc)
{
	esp_err_t err = ESP_OK;

	xSemaphoreTake(s_switchLock, portMAX_DELAY);
	if (ifc != s_active) {
		uiStop(s_active);
		s_active = UI_NONE;
		uiNewSession();

		err = uiStart(ifc);
		if (err == ESP_OK) {
			s_active = ifc;
		} else if (ifc != UI_BLE) {
			ESP_LOGE(TAG, "%s failed (%s) - falling back to BLE", uiName(ifc), esp_err_to_name(err));
			if (uiStart(UI_BLE) == ESP_OK) {
				s_active = UI_BLE;
			}
		}
		uiNewSession();
		uiPost(UI_EV_START);
		ESP_LOGI(TAG, "active: %s", uiName(s_active));
	}
	xSemaphoreGive(s_switchLock);
	return err;
}

void uiDisable(void)
{
	xSemaphoreTake(s_switchLock, portMAX_DELAY);
	uiStop(s_active);
	s_active = UI_NONE;
	uiNewSession();
	xSemaphoreGive(s_switchLock);
}

uiIf_t uiActive(void)
{
	return s_active;
}

void uiFmChanged(void)
{
	if (!s_client) {
		uiPost(UI_EV_START);			/* SD manager off: back to the Wait screen */
	}
}

bool uiRunMain(uiMainFn_t fn, int arg)
{
	uiChunk_t c    = { .session = s_session, .kind = UI_EV_CALL };
	uiCall_t  call = { .fn = fn, .arg = arg };
	memcpy(c.data, &call, sizeof(call));
	return xQueueSend(s_rxQueue, &c, 0) == pdTRUE;
}

/* ================= Internals ================= */

/* Queue bytes from src, if it is the active interface. Runs in the interface's task. */
static void uiFeed(uiIf_t src, const uint8_t *data, size_t len)
{
	if (src != s_active) {
		return;
	}

	uiChunk_t c = { .session = s_session };
	while (len > 0) {
		c.len = (len > UI_CHUNK) ? UI_CHUNK : len;
		memcpy(c.data, data, c.len);
		if (src == UI_TELNET || src == UI_UART || src == UI_USB) {
			/* back-pressure: the telnet / serial task waits (TCP or the driver's
			 * receive buffer holds the sender, e.g. pasting long scripts). Give
			 * up only if the interface stops being active. */
			while (xQueueSend(s_rxQueue, &c, pdMS_TO_TICKS(UI_FEED_WAIT_MS)) != pdTRUE) {
				if (s_active != src) {
					return;
				}
			}
		} else if (xQueueSend(s_rxQueue, &c, 0) != pdTRUE) {
			ESP_LOGW(TAG, "rx queue full, dropped %u bytes", (unsigned)len);
			return;
		}
		data += c.len;
		len  -= c.len;
	}
}

static void uiNewSession(void)
{
	s_session = s_session + 1;
}

static esp_err_t uiStart(uiIf_t ifc)
{
	esp_err_t err = ESP_OK;

	switch (ifc) {
	case UI_BLE:
		bleEnable();
		break;
	case UI_TELNET:
		s_link = false;
		err = wifiApStart(false);
		if (err == ESP_OK) {
			err = telnetStart(uiTelnetRx, uiTelnetConn);
			if (err != ESP_OK) {
				wifiApStop();
			}
		}
		break;
	case UI_WEB:
		s_link = false;
		err = wifiApStart(nvGetInt("web.captive") != 0);	/* DHCP portal URL only with the captive portal */
		if (err == ESP_OK) {
			err = webStart(uiWebRx, uiWebConn);
			if (err != ESP_OK) {
				wifiApStop();
			}
		}
		break;
	case UI_UART:
		err = serSetBaud(nvGetInt("uart.baud"));
		break;
	case UI_RESET:
		s_rstMode = RESET_NV;				/* each visit starts at NVRAM */
		break;
	default:
		break;
	}
	if (err == ESP_OK && uiSerPort(ifc) >= 0) {		/* fresh serial session */
		s_serConn   = false;
		s_serEcho   = nvGetInt("ser.echo");
		s_serLen    = 0;
		s_serLastCr = false;
		s_serEmpty  = 0;
		s_serEsc    = false;
	}
	logSetUiPort(err == ESP_OK ? uiSerPort(ifc) : -1);	/* logs to the other port(s) */
	return err;
}

static void uiStop(uiIf_t ifc)
{
	switch (ifc) {
	case UI_BLE:
		bleDisable();
		break;
	case UI_TELNET:
		telnetStop();
		wifiApStop();
		break;
	case UI_WEB:
		webStop();
		wifiApStop();
		break;
	case UI_UART:
		serSetBaud(SER_LOG_BAUD);			/* back to the log speed */
		break;
	default:
		break;
	}
	logSetUiPort(-1);					/* logs to both, debug off */
}

static const char *uiName(uiIf_t ifc)
{
	switch (ifc) {
	case UI_BLE:    return "BLE";
	case UI_TELNET: return "TELNET";
	case UI_WEB:    return "WEB";
	case UI_UART:   return "UART";
	case UI_USB:    return "USB";
	case UI_MSC:    return "MSC";
	case UI_RESET:  return "RESET!";
	default:        return "NONE";
	}
}

static void uiBleRx(const uint8_t *data, size_t len)
{
	uiFeed(UI_BLE, data, len);
}

static void uiBleConn(bool connected)
{
	if (s_active == UI_BLE) {
		uiNewSession();
		uiPost(connected ? UI_EV_CONNECT : UI_EV_START);
	}
}

static void uiTelnetRx(const uint8_t *data, size_t len)
{
	uiFeed(UI_TELNET, data, len);
}

static void uiTelnetConn(bool connected)
{
	if (s_active == UI_TELNET) {
		uiNewSession();
		uiPost(connected ? UI_EV_CONNECT : UI_EV_START);
	}
}

/* Web input: one whole line, never waits (the server task also delivers our
 * replies, so blocking here could deadlock). false = no room, page gets ERR busy. */
static bool uiWebRx(const uint8_t *data, size_t len)
{
	if (s_active != UI_WEB) {
		return true;
	}
	if (uxQueueSpacesAvailable(s_rxQueue) < (len + UI_CHUNK - 1) / UI_CHUNK) {
		return false;
	}
	uiFeed(UI_WEB, data, len);
	return true;
}

static void uiWebConn(bool connected)
{
	if (s_active == UI_WEB) {
		uiNewSession();
		uiPost(connected ? UI_EV_CONNECT : UI_EV_START);
	}
}

/* Wi-Fi link up/down (event task). Down = last station left: its telnet/web
 * connection will never close by itself, so drop it now (-> disconnect ->
 * details screen). */
static void uiWifiLink(bool up)
{
	s_link = up;
	if (!up) {
		switch (s_active) {
		case UI_TELNET: telnetDropClient(); break;
		case UI_WEB:    webDropClient(); break;
		default:        break;
		}
	}
	uiPost(UI_EV_LINK);
}

/* ================= Connection screens ================= */

/* Queue a screen event for the main task (any task; never blocks). */
static void uiPost(uint8_t kind)
{
	uiChunk_t c = { .session = s_session, .kind = kind };
	if (xQueueSend(s_rxQueue, &c, 0) != pdTRUE) {
		ESP_LOGW(TAG, "rx queue full, screen event %d dropped", kind);
	}
}

/* Main task: handle a screen event. */
static void uiEvent(uint8_t kind)
{
	switch (kind) {
	case UI_EV_START:
		if (s_client) {
			s_client = false;
			uiSend("<<UI DISCONNECT>>\n");	/* no client to see it: for scripts */
			scriptClientGone();		/* RUN script: wait (ON UI) or stop */
		}
		scriptWaitEnd();			/* UI switch during a Wait screen: onWait after */
		if (fmActive()) {
			uiFmScreen();			/* SD manager: its screen, no onWait */
		} else {
			uiStartScreen(true);		/* client gone / UI switch */
		}
		break;
	case UI_EV_CONNECT:
		esp_timer_stop(s_bootPoll);
		s_client = true;
		uiSend("<<UI CONNECT>>\n");		/* the client's first message */
		scriptWaitEnd();			/* onWait instead exits, after runs */
		if (fmActive()) {
			uiScrConnect();			/* SD manager: no onConnect script */
		} else {
			scriptConnect(uiName(s_active), uiScrConnect);	/* splash, or onConnect */
		}
		break;
	case UI_EV_FMSTOP:
		fmStop();
		break;
	case UI_EV_MSC:
		if (s_active == UI_MSC && usbDiskBegin() != NULL) {
			ESP_LOGW(TAG, "disk mode could not start");
		}
		break;
	case UI_EV_RSTTAP:
	case UI_EV_RSTARM:
	case UI_EV_RSTGO:
	case UI_EV_RSTCANCEL:
		uiResetEvent(kind);
		break;
	case UI_EV_BUTTON:
		uiScrPassword(s_pwdShown);
		break;
	case UI_EV_NEXT:
		uiNextIf();
		break;
	case UI_EV_LINK:
		uiSend(s_link ? "<<UI LINK UP>>\n" : "<<UI LINK DOWN>>\n");
		uiScrLink(s_link);
		break;
	default:
		break;
	}
}

/* Details screen for the active interface (after a client: fade to black first);
 * watch BOOT (long press: password, double press: next interface). */
static void uiStartScreen(bool afterClient)
{
	char details[200];

	esp_timer_stop(s_bootPoll);
	uiScrSetLink(s_link);				/* current state, drawn with the details */
	uiWaitDetails(details, sizeof(details));
	if (s_active == UI_RESET) {			/* no onWait: the reset is for when things are broken */
		sdLock();
		s_rstNoSd = (sdEnsureMounted() != ESP_OK);
		sdUnlock();
		if (s_rstNoSd) {
			s_rstMode = RESET_NV;
		}
		uiScrResetSet(s_rstMode, s_rstNoSd);
	} else if (scriptWaitBegin(details)) {
		return;					/* onWait 'instead' shows the Wait screen */
	}
	if (afterClient) {
		uiScrDisconnect(s_active);
	} else {
		uiScrStart(s_active);
	}
	if (uiScrWaiting()) {
		uiBootWatch();
	}
}

/* Start watching BOOT (Wait screen gestures, or the SD manager's exit). */
static void uiBootWatch(void)
{
	gpio_set_pull_mode(UI_BOOT_GPIO, GPIO_PULLUP_ONLY);
	s_bootLongMs  = nvGetInt("btn.long");
	s_bootDblMs   = nvGetInt("btn.dbl");
	s_bootDown    = false;
	s_bootLong    = false;
	s_bootRelease = 0;
	s_pwdShown    = false;
	s_rstArmed    = false;
	s_rstTap      = false;
	esp_timer_start_periodic(s_bootPoll, UI_BOOT_POLL_US);
}

/* SD manager on, no client: its screen; BOOT short press leaves it. */
static void uiFmScreen(void)
{
	char url[48];
	esp_timer_stop(s_bootPoll);
	snprintf(url, sizeof(url), "http://%s/fm", nvGet("wifi.ip"));
	uiScrFm(url);
	uiBootWatch();
}

/* onWait argument: the details fields of the active interface. */
static void uiWaitDetails(char *out, size_t n)
{
	const char *link = s_link ? "Active" : "Waiting";
	switch (s_active) {
	case UI_WEB:
	case UI_TELNET:
		snprintf(out, n, "UI:%s:%d|SSID:%s|Pwd:%s|IP:%s|Mask:%s|Link:%s", uiName(s_active),
		         nvGetInt(s_active == UI_WEB ? "web.port" : "telnet.port"), nvGet("wifi.ssid"), nvGet("wifi.pwd"),
		         nvGet("wifi.ip"), nvGet("wifi.cidr"), link);
		break;
	case UI_BLE:
		snprintf(out, n, "UI:BLE|Name:%s", nvGet("ble.name"));
		break;
	case UI_UART: {
		int tx, rx;
		serUartPins(&tx, &rx);
		snprintf(out, n, "UI:UART|Baud:%s|Pins:Tx:%d Rx:%d", nvGet("uart.baud"), tx, rx);
		break;
	}
	default:
		snprintf(out, n, "UI:%s", uiName(s_active));
		break;
	}
}

/* esp_timer task, every 50 ms (also the debounce): BOOT gestures.
 *   held >= btn.long          -> password shown until released
 *   two short presses, the second starting within btn.dbl of the first
 *   release                   -> next interface */
static void uiBootPoll(void *arg)
{
	gpio_input_enable(UI_BOOT_GPIO);		/* in case BTN 1 was switched off (pin reset) */
	bool    down = (gpio_get_level(UI_BOOT_GPIO) == 0);
	int64_t now  = esp_timer_get_time() / 1000;

	if (fmActive()) {				/* SD manager screen: a press leaves it */
		if (!down && s_bootDown) {
			s_bootDown = false;
			uiPost(UI_EV_FMSTOP);
		} else if (down) {
			s_bootDown = true;
		}
		return;
	}
	if (s_active == UI_RESET) {
		uiResetPoll(down, now);
		return;
	}

	if (down && !s_bootDown) {			/* press */
		s_bootDown  = true;
		s_bootLong  = false;
		s_bootPress = now;
	} else if (down && !s_bootLong && now - s_bootPress >= s_bootLongMs) {
		s_bootLong = true;			/* long press: show (MSC: start disk mode) */
		if (s_active == UI_MSC) {
			uiPost(UI_EV_MSC);
		} else {
			s_pwdShown = true;
			uiPost(UI_EV_BUTTON);
		}
	} else if (!down && s_bootDown) {		/* release */
		s_bootDown = false;
		if (s_bootLong) {
			s_bootRelease = 0;		/* never part of a double */
			if (s_active != UI_MSC) {
				s_pwdShown = false;	/* hide */
				uiPost(UI_EV_BUTTON);
			}
		} else if (s_bootRelease && s_bootPress - s_bootRelease <= s_bootDblMs) {
			s_bootRelease = 0;
			uiPost(UI_EV_NEXT);
		} else {
			s_bootRelease = now;
		}
	}
}

/* Main task: BOOT double press on the Wait screen: next interface (uiIfNext),
 * this session only (NV sys.ui unchanged). */
static void uiNextIf(void)
{
	uiIf_t next = uiIfNext(s_active);
	if (next == UI_NONE || s_switching || !uiScrWaiting()) {
		return;
	}
	s_switching = true;
	if (xTaskCreate(uiSwitchTask, "uiswitch", UI_SWITCH_STACK, (void *)(intptr_t)(next | UI_SWITCH_NOW),
	                UI_SWITCH_PRIO, NULL) != pdPASS) {	/* no client: no reply to wait for */
		s_switching = false;
	}
}

/* WEB -> TELNET -> BLE -> USB -> UART -> MSC -> RESET! -> WEB (BOOT double press,
 * UI NEXT; a client is never on MSC / RESET!, so UI NEXT stops at MSC). */
static uiIf_t uiIfNext(uiIf_t ifc)
{
	switch (ifc) {
	case UI_WEB:    return UI_TELNET;
	case UI_TELNET: return UI_BLE;
	case UI_BLE:    return UI_USB;
	case UI_USB:    return UI_UART;
	case UI_UART:   return UI_MSC;
	case UI_MSC:    return UI_RESET;
	case UI_RESET:  return UI_WEB;
	default:        return UI_NONE;
	}
}

/* NV sys.ui as an interface. */
static uiIf_t uiBootIf(void)
{
	const char *b = nvGet("sys.ui");
	if (UI_DEBUG_BOOT_TELNET || strcmp(b, "TELNET") == 0) {
		return UI_TELNET;
	}
	if (strcmp(b, "WEB") == 0) {
		return UI_WEB;
	}
	if (strcmp(b, "UART") == 0) {
		return UI_UART;
	}
	if (strcmp(b, "USB") == 0) {
		return UI_USB;
	}
	if (strcmp(b, "MSC") == 0) {
		return UI_MSC;
	}
	return UI_BLE;
}

/* "<<SRC ...": true if that source is muted by EVENTS. */
static bool uiEvMuted(const char *text)
{
	if (s_evMute == 0 || text[0] != '<' || text[1] != '<') {
		return false;
	}
	for (int i = 0; i < UI_EV_SOURCES; i++) {
		size_t n = strlen(s_evNames[i]);
		if (strncmp(text + 2, s_evNames[i], n) == 0 && (text[2 + n] == ' ' || text[2 + n] == '>')) {
			return (s_evMute >> i) & 1;
		}
	}
	return false;
}

/* arg: interface, | UI_SWITCH_NOW to skip the reply delay (BOOT double press) */
static void uiSwitchTask(void *arg)
{
	int a = (int)(intptr_t)arg;
	if (!(a & UI_SWITCH_NOW)) {
		vTaskDelay(pdMS_TO_TICKS(UI_SWITCH_DELAY_MS));
	}
	uiSelect((uiIf_t)(a & ~UI_SWITCH_NOW));
	s_switching = false;
	vTaskDelete(NULL);
}

/* ================= RESET! screen ================= */

/* esp_timer task: BOOT on the RESET! screen.
 *   tap               next mode, once btn.dbl has passed with no second tap
 *   two taps          next interface (as on every Wait screen)
 *   long press        arm ("Long press again...")
 *   long press, armed (within UI_RST_CONFIRM_MS of the first release) -> go
 *   tap, armed / time-out  cancel */
static void uiResetPoll(bool down, int64_t now)
{
	if (s_rstBusy) {
		return;
	}
	if (down && !s_bootDown) {			/* press */
		s_bootDown  = true;
		s_bootLong  = false;
		s_bootPress = now;
	} else if (down && !s_bootLong && now - s_bootPress >= s_bootLongMs) {
		s_bootLong = true;			/* long press */
		s_rstTap   = false;
		if (s_rstArmed) {
			s_rstArmed = false;
			uiPost(UI_EV_RSTGO);
		} else {
			s_rstArmed   = true;
			s_rstArmedAt = now;
			uiPost(UI_EV_RSTARM);
		}
	} else if (!down && s_bootDown) {		/* release */
		s_bootDown = false;
		if (s_bootLong) {
			if (s_rstArmed) {
				s_rstArmedAt = now;	/* the confirm window starts now */
			}
		} else if (s_rstArmed) {
			s_rstArmed = false;
			uiPost(UI_EV_RSTCANCEL);
		} else if (s_rstTap && s_bootPress - s_bootRelease <= s_bootDblMs) {
			s_rstTap = false;
			uiPost(UI_EV_NEXT);
		} else {
			s_rstTap      = true;
			s_bootRelease = now;
		}
	} else if (!down && s_rstTap && now - s_bootRelease > s_bootDblMs) {
		s_rstTap = false;			/* no second tap: a mode change */
		uiPost(UI_EV_RSTTAP);
	} else if (!down && s_rstArmed && now - s_rstArmedAt > UI_RST_CONFIRM_MS) {
		s_rstArmed = false;
		uiPost(UI_EV_RSTCANCEL);
	}
}

/* Main task: RESET! screen events. */
static void uiResetEvent(uint8_t kind)
{
	if (s_active != UI_RESET || !uiScrWaiting()) {
		return;
	}
	switch (kind) {
	case UI_EV_RSTTAP:
		if (!s_rstNoSd) {
			s_rstMode = (resetMode_t)((s_rstMode + 1) % RESET_MODES);
		}
		uiScrResetSet(s_rstMode, s_rstNoSd);
		uiScrResetMode();
		uiScrResetMsg("", "");
		break;
	case UI_EV_RSTARM:
		uiScrResetMsg("Long press again...", "");
		break;
	case UI_EV_RSTCANCEL:
		uiScrResetMsg("Cancelled", "");
		break;
	case UI_EV_RSTGO: {
		s_rstBusy = true;
		esp_timer_stop(s_bootPoll);
		ESP_LOGW(TAG, "RESET! mode %d%s", (int)s_rstMode, s_rstNoSd ? " (no SD)" : "");
		const char *e = resetRun(s_rstMode, s_rstNoSd, uiResetMsg);	/* NVRAM / FULL reboot */
		if (e) {
			char text[48];
			snprintf(text, sizeof(text), "ERR %s", e);
			uiScrResetMsg(text, "");
		} else {
			uiScrResetMsg("reset.s done", "");
		}
		s_rstBusy = false;
		uiBootWatch();
		break;
	}
	default:
		break;
	}
}

/* resetRun() progress: two message lines; NULL = redraw the RESET! screen. */
static void uiResetMsg(const char *a, const char *b)
{
	if (!a) {
		uiScrResetSet(s_rstMode, s_rstNoSd);
		uiScrDisconnect(UI_RESET);		/* fade to black, screen again */
		return;
	}
	uiScrResetMsg(a, b);
}

/* ================= Commands ================= */

static const char *uiCmd(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 0) {
		reply(uiName(s_active));
		reply("\n");
		return NULL;
	}

	uiIf_t target;
	if (strcasecmp(argv[0], "BLE") == 0) {
		target = UI_BLE;
	} else if (strcasecmp(argv[0], "TELNET") == 0) {
		target = UI_TELNET;
	} else if (strcasecmp(argv[0], "WEB") == 0) {
		target = UI_WEB;
	} else if (strcasecmp(argv[0], "UART") == 0) {
		target = UI_UART;
	} else if (strcasecmp(argv[0], "USB") == 0) {
		target = UI_USB;
	} else if (strcasecmp(argv[0], "MSC") == 0) {
		target = UI_MSC;			/* Wait screen only; hold BOOT for disk mode */
	} else if (strcasecmp(argv[0], "NEXT") == 0) {
		target = uiIfNext(s_active);
		if (target == UI_NONE) {
			return "no active interface";
		}
	} else {
		return "usage: UI [BLE|TELNET|WEB|UART|USB|MSC|NEXT]";
	}

	if (target == s_active) {
		return NULL;
	}
	if (s_switching) {
		return "switch in progress";
	}

	if (target == UI_TELNET) {
		char text[96];
		snprintf(text, sizeof(text), "SSID:%s\nPWD:%s\nIP:%s\nPORT:%d\n",
		         nvGet("wifi.ssid"), nvGet("wifi.pwd"), nvGet("wifi.ip"), nvGetInt("telnet.port"));
		reply(text);
	} else if (target == UI_WEB) {
		char text[112];
		int  port = nvGetInt("web.port");
		char pp[8] = "";
		if (port != 80) {
			snprintf(pp, sizeof(pp), ":%d", port);
		}
		snprintf(text, sizeof(text), "SSID:%s\nPWD:%s\nURL:http://%s%s/\n",
		         nvGet("wifi.ssid"), nvGet("wifi.pwd"), nvGet("wifi.ip"), pp);
		reply(text);
	} else if (target == UI_UART) {
		char text[48];
		int  tx, rx;
		serUartPins(&tx, &rx);
		snprintf(text, sizeof(text), "BAUD:%s\nPINS:Tx:%d Rx:%d\n", nvGet("uart.baud"), tx, rx);
		reply(text);
	}

	s_switching = true;
	if (xTaskCreate(uiSwitchTask, "uiswitch", UI_SWITCH_STACK, (void *)(intptr_t)target,
	                UI_SWITCH_PRIO, NULL) != pdPASS) {
		s_switching = false;
		return "switch failed";
	}
	return NULL;
}

/* ================= Serial UIs ================= */

/* Serial port of a UI, or -1. */
static int uiSerPort(uiIf_t ifc)
{
	return (ifc == UI_UART) ? SER_UART : (ifc == UI_USB) ? SER_USB : -1;
}

/* Serial task: Enter. The first line counts as the client connecting. */
static void uiSerEnter(serPort_t port)
{
	if (s_serEcho) {
		serWrite(port, "\r\n", 2, false);
	}
	s_serEsc = false;
	if (!s_serConn) {
		s_serConn = true;
		uiNewSession();
		uiPost(UI_EV_CONNECT);				/* splash */
	}
	if (s_serLen == 0) {
		if (++s_serEmpty >= UI_SER_BANNER_ENTERS) {
			s_serEmpty = 0;
			uiSerBanner(port);
		}
		return;
	}
	s_serEmpty = 0;
	s_serLine[s_serLen++] = '\n';
	uiFeed(port == SER_UART ? UI_UART : UI_USB, (const uint8_t *)s_serLine, s_serLen);
	s_serLen = 0;
}

/* Serial task, ~20 ms: "+++" / "---" alone on the line, then quiet for 1 s. */
static void uiSerTick(void)
{
	int port = uiSerPort(s_active);
	if (port < 0 || !s_serEsc || esp_timer_get_time() / 1000 - s_serEscAt < UI_SER_ESC_MS) {
		return;
	}
	bool on = (s_serLine[0] == '+');
	s_serEsc = false;
	s_serLen = 0;
	logSetDebug(on);
	const char *msg = on ? "\n[debug enabled, --- to stop]\n" : "\n[debug disabled]\n";
	xSemaphoreTake(s_sendLock, portMAX_DELAY);
	serWrite((serPort_t)port, msg, strlen(msg), true);
	xSemaphoreGive(s_sendLock);
}

static void uiSerBanner(serPort_t port)
{
	const char *msg = (port == SER_UART) ? "[Startup Complete - UART UI active]\n"
	                                     : "[Startup Complete - USB UI active]\n";
	xSemaphoreTake(s_sendLock, portMAX_DELAY);
	serWrite(port, msg, strlen(msg), true);
	xSemaphoreGive(s_sendLock);
}

/* EVENTS [src 0|1]: list / mute or unmute event sources for the client. */
static const char *uiCmdEvents(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 0) {
		char text[64];
		int  n = 0;
		for (int i = 0; i < UI_EV_SOURCES; i++) {
			n += snprintf(text + n, sizeof(text) - n, "%s%s %d", i ? " " : "", s_evNames[i],
			              !((s_evMute >> i) & 1));
		}
		snprintf(text + n, sizeof(text) - n, "\n");
		reply(text);
		return NULL;
	}
	int on;
	if (argc != 2 || !cmdParseInt(argv[1], 0, 1, &on)) {
		return "usage: EVENTS [BTN|IMU|UI|SCRIPT|ALL 0|1]";
	}
	uint8_t bits = 0;
	if (strcasecmp(argv[0], "ALL") == 0) {
		bits = (1u << UI_EV_SOURCES) - 1;
	} else {
		for (int i = 0; i < UI_EV_SOURCES; i++) {
			if (strcasecmp(argv[0], s_evNames[i]) == 0) {
				bits = 1u << i;
			}
		}
	}
	if (!bits) {
		return "usage: EVENTS [BTN|IMU|UI|SCRIPT|ALL 0|1]";
	}
	s_evMute = on ? (s_evMute & ~bits) : (s_evMute | bits);
	return NULL;
}
