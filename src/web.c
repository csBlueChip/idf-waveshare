#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "sdkconfig.h"

#if !CONFIG_HTTPD_WS_SUPPORT || !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "web.c needs CONFIG_HTTPD_WS_SUPPORT and CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT - delete sdkconfig and rebuild"
#endif

#include "dns.h"
#include "sd.h"
#include "webpath.h"
#include "nv.h"
#include "fm.h"

static const char *TAG = "web";

#define WEB_LINE_MAX		200	/* = CMD_LINE_MAX */
#define WEB_SEND_TIMEOUT_MS	2000
#define WEB_ROOT		SD_MOUNT_POINT "/www"
#define WEB_FILE_CHUNK		1436	/* one TCP segment */
#define WEB_CLOSE_REPLACED	4000	/* WebSocket close code: another page took over */
#define WEB_CLOSE_REBOOT	4001	/* WebSocket close code: device restarting */
#define WEB_CLOSE_WAIT_MS	200	/* restart: wait for the close frame to be sent */
#define WEB_CLOSE_FLUSH_MS	50	/* restart: then let TCP put it on the air */
#define WEB_PING		"<<PING>>"	/* page heartbeat, answered here */
#define WEB_PONG		"<<PONG>>\n"

/* DEBUG: 1 = log every connection open/close and every GET with its timing */
#define WEB_DEBUG		1
#if WEB_DEBUG
#define WEB_DBG(fmt, ...)	ESP_LOGI(TAG, "DBG " fmt, ##__VA_ARGS__)
#else
#define WEB_DBG(fmt, ...)	do { } while (0)
#endif

extern const char s_page[] asm("_binary_index_html_start");

typedef struct {
	int     fd;
	size_t  len;
	uint8_t data[];
} webMsg_t;

static httpd_handle_t    s_server;
static volatile int      s_fd = -1;		/* the page's WebSocket, -1 = none */
static webRxCb_t         s_rxCb;
static webConnCb_t       s_connCb;
static SemaphoreHandle_t s_sent;
static SemaphoreHandle_t s_closed;		/* restart: close frame sent */
static char              s_home[40];		/* http://<ip>/ */
static int               s_open;		/* DEBUG: open connections */

static esp_err_t webGetPage(httpd_req_t *req);
static esp_err_t webGet(httpd_req_t *req);
static bool      webSendFile(httpd_req_t *req);
static esp_err_t webGetCfg(httpd_req_t *req);
static esp_err_t webTime(httpd_req_t *req);
static esp_err_t webWs(httpd_req_t *req);
static esp_err_t webWsOpen(httpd_req_t *req);
static esp_err_t webRedirect(httpd_req_t *req, httpd_err_code_t err);
static void      webOnClose(httpd_handle_t hd, int fd);
static esp_err_t webOnOpen(httpd_handle_t hd, int fd);
static void      webDbgHost(httpd_req_t *req, char *host, size_t n);
static void      webDbgGet(httpd_req_t *req, const char *host, const char *what, int64_t t0);
static void      webSendWork(void *arg);
static esp_err_t webDrain(httpd_req_t *req, httpd_ws_frame_t *f);
static void      webDropAll(void *arg);
static void      webShutdown(void);
static void      webCloseWork(void *arg);

/* ================= Public API ================= */

esp_err_t webStart(webRxCb_t rxCb, webConnCb_t connCb)
{
	if (s_server) {
		return ESP_OK;
	}
	if (!s_sent && !(s_sent = xSemaphoreCreateBinary())) {
		return ESP_ERR_NO_MEM;
	}
	if (!s_closed && !(s_closed = xSemaphoreCreateBinary())) {
		return ESP_ERR_NO_MEM;
	}
	/* planned restarts (esp_restart, e.g. REBOOT): close the page's WebSocket first. Runs
	 * before Wi-Fi's own handler (registered earlier; handlers run newest first). */
	esp_err_t sh = esp_register_shutdown_handler(webShutdown);
	if (sh != ESP_OK && sh != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "no shutdown handler (%s)", esp_err_to_name(sh));
	}
	s_rxCb   = rxCb;
	s_connCb = connCb;
	s_fd     = -1;
	s_open   = 0;
	snprintf(s_home, sizeof(s_home), "http://%s/", nvGet("rad.ip"));

	esp_err_t err = dnsStart(inet_addr(nvGet("rad.ip")));
	if (err != ESP_OK) {
		return err;
	}

	httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
	cfg.server_port      = WEB_PORT;
	cfg.lru_purge_enable = true;			/* phones open many probe connections */
	cfg.close_fn         = webOnClose;
	cfg.open_fn          = webOnOpen;
	cfg.uri_match_fn     = httpd_uri_match_wildcard;	/* catch-all handler below; exact URIs match first */
	cfg.max_uri_handlers = 4 + FM_URI_COUNT + 2;		/* ours + the SD manager's + spare */
	err = httpd_start(&s_server, &cfg);
	if (err != ESP_OK) {
		dnsStop();
		s_server = NULL;
		return err;
	}

	const httpd_uri_t uris[] = {
		{ .uri = "/cfg", .method = HTTP_GET, .handler = webGetCfg },
		{ .uri = "/time", .method = HTTP_POST, .handler = webTime },
		{ .uri = "/ws",  .method = HTTP_GET, .handler = webWs, .is_websocket = true,
		  .ws_post_handshake_cb = webWsOpen },
		{ .uri = "/*",   .method = HTTP_GET, .handler = webGet },	/* last: everything else */
	};
	for (size_t i = 0; i < 3; i++) {
		httpd_register_uri_handler(s_server, &uris[i]);
	}
	fmRegister(s_server);					/* /fm ... before the catch-all */
	httpd_register_uri_handler(s_server, &uris[3]);
	httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, webRedirect);

	ESP_LOGI(TAG, "portal at %s", s_home);
	return ESP_OK;
}

esp_err_t webStop(void)
{
	if (!s_server) {
		return ESP_OK;
	}
	httpd_handle_t hd = s_server;
	s_server = NULL;
	s_fd     = -1;
	esp_err_t err = httpd_stop(hd);
	dnsStop();
	return err;
}

void webDropClient(void)
{
	if (s_server) {
		httpd_queue_work(s_server, webDropAll, NULL);	/* run in the server task */
	}
}

esp_err_t webSend(const uint8_t *data, size_t len)
{
	int fd = s_fd;
	if (!s_server || fd < 0) {
		return ESP_ERR_INVALID_STATE;
	}

	webMsg_t *m = malloc(sizeof(webMsg_t) + len);
	if (!m) {
		return ESP_ERR_NO_MEM;
	}
	m->fd  = fd;
	m->len = len;
	memcpy(m->data, data, len);

	xSemaphoreTake(s_sent, 0);			/* clear a late give from a timed-out send */
	esp_err_t err = httpd_queue_work(s_server, webSendWork, m);
	if (err != ESP_OK) {
		free(m);
		return err;
	}
	/* wait so output is paced by the network (and the order is kept) */
	if (xSemaphoreTake(s_sent, pdMS_TO_TICKS(WEB_SEND_TIMEOUT_MS)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	return ESP_OK;
}

/* ================= Internals (server task) ================= */

static esp_err_t webGetPage(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

/* Any other GET: SD:/www file if there is one, else the console page for "/",
 * else redirect to "/". */
static esp_err_t webGet(httpd_req_t *req)
{
	int64_t t0 = esp_timer_get_time();
	char    host[48];
	webDbgHost(req, host, sizeof(host));		/* before the reply: headers are gone after */
	if (webSendFile(req)) {
		webDbgGet(req, host, "file", t0);
		return ESP_OK;
	}
	const char *q = strpbrk(req->uri, "?#");
	size_t      n = q ? (size_t)(q - req->uri) : strlen(req->uri);
	if (n == 1 && req->uri[0] == '/') {
		esp_err_t err = webGetPage(req);
		webDbgGet(req, host, "page", t0);
		return err;
	}
	esp_err_t err = webRedirect(req, HTTPD_404_NOT_FOUND);
	webDbgGet(req, host, "redirect", t0);
	return err;
}

/* DEBUG: the request's Host header ("?" if none). */
static void webDbgHost(httpd_req_t *req, char *host, size_t n)
{
	if (httpd_req_get_hdr_value_str(req, "Host", host, n) != ESP_OK) {
		snprintf(host, n, "?");
	}
}

/* DEBUG: one line per GET: host, uri, socket, result, time taken. */
static void webDbgGet(httpd_req_t *req, const char *host, const char *what, int64_t t0)
{
	WEB_DBG("GET %s%s fd %d -> %s %d ms", host, req->uri, httpd_req_to_sockfd(req), what,
	        (int)((esp_timer_get_time() - t0) / 1000));
}

/* DEBUG: count connections. */
static esp_err_t webOnOpen(httpd_handle_t hd, int fd)
{
	s_open++;
	WEB_DBG("open fd %d (%d open)", fd, s_open);
	return ESP_OK;
}

/* Send SD:/www/<uri> in chunks. false = no such file (nothing sent). */
static bool webSendFile(httpd_req_t *req)
{
	static EXT_RAM_BSS_ATTR char buf[WEB_FILE_CHUNK];			/* server task only */
	char        path[sizeof(WEB_ROOT) + 260];
	struct stat st;

	if (!webPathMap(req->uri, WEB_ROOT, path, sizeof(path))) {
		return false;
	}

	sdLock();
	FILE *f = NULL;
	if (sdEnsureMounted() == ESP_OK && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		f = fopen(path, "rb");
	}
	if (!f) {
		sdUnlock();
		return false;
	}

	httpd_resp_set_type(req, webPathType(path));
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
			break;					/* client gone */
		}
	}
	fclose(f);
	sdUnlock();
	httpd_resp_send_chunk(req, NULL, 0);
	return true;
}

static esp_err_t webGetCfg(httpd_req_t *req)
{
	int64_t t0 = esp_timer_get_time();
	char    host[48];
	webDbgHost(req, host, sizeof(host));		/* before the reply: headers are gone after */
	char json[32];
	snprintf(json, sizeof(json), "{\"hist\":%d}", nvGetInt("web.hist"));
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
	webDbgGet(req, host, "cfg", t0);
	return err;
}

/* POST /time?t=<seconds>: set the clock from the browser's LOCAL time (the
 * device has no time zone, so FAT dates then match the PC's). Sent by the
 * console page on connect and by the SD manager page when it opens. */
static esp_err_t webTime(httpd_req_t *req)
{
	char q[48], v[24];
	char *end;
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
	    httpd_query_key_value(q, "t", v, sizeof(v)) != ESP_OK) {
		httpd_resp_set_status(req, "400 Bad Request");
		return httpd_resp_sendstr(req, "missing t");
	}
	long long t = strtoll(v, &end, 10);
	if (*end || t < 1577836800LL || t > 4102444800LL) {	/* 2020 .. 2100 */
		httpd_resp_set_status(req, "400 Bad Request");
		return httpd_resp_sendstr(req, "bad time");
	}
	struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
	settimeofday(&tv, NULL);
	WEB_DBG("clock set from the browser: %lld", t);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_sendstr(req, "OK");
}

/* Anything unknown (incl. OS connectivity checks) -> the console page. */
static esp_err_t webRedirect(httpd_req_t *req, httpd_err_code_t err)
{
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", s_home);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, NULL, 0);
}

/* WebSocket handshake done (the frame handler is not called for it): this is the page now. */
static esp_err_t webWsOpen(httpd_req_t *req)
{
	int fd  = httpd_req_to_sockfd(req);
	int old = s_fd;

	s_fd = fd;
	if (old >= 0 && old != fd) {				/* tell the old page it was replaced */
		uint8_t close[2 + 8] = { WEB_CLOSE_REPLACED >> 8, WEB_CLOSE_REPLACED & 0xFF,
		                         'r', 'e', 'p', 'l', 'a', 'c', 'e', 'd' };
		httpd_ws_frame_t c = { .type = HTTPD_WS_TYPE_CLOSE, .payload = close, .len = sizeof(close) };
		httpd_ws_send_frame_async(req->handle, old, &c);
		httpd_sess_trigger_close(req->handle, old);
	}
	ESP_LOGI(TAG, "page connected (fd %d)", fd);
	if (s_connCb) {
		s_connCb(true);
	}
	return ESP_OK;
}

/* One WebSocket frame from a page. */
static esp_err_t webWs(httpd_req_t *req)
{
	int             fd = httpd_req_to_sockfd(req);
	uint8_t         buf[WEB_LINE_MAX + 1];
	httpd_ws_frame_t f = { .payload = buf };
	esp_err_t err = httpd_ws_recv_frame(req, &f, 0);	/* get length */
	if (err != ESP_OK) {
		return err;
	}
	/* the payload must always be read, or the next frame header is misparsed */
	if (f.type != HTTPD_WS_TYPE_TEXT || fd != s_fd) {
		return webDrain(req, &f);			/* not for us: discard */
	}
	if (f.len > WEB_LINE_MAX) {
		static const char tooLong[] = "ERR line too long\n";
		httpd_ws_frame_t r = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)tooLong, .len = sizeof(tooLong) - 1 };
		err = webDrain(req, &f);
		return (err == ESP_OK) ? httpd_ws_send_frame(req, &r) : err;
	}
	if (f.len == 0) {
		return ESP_OK;					/* empty line */
	}
	err = httpd_ws_recv_frame(req, &f, f.len);
	if (err != ESP_OK) {
		return err;
	}
	if (f.len == sizeof(WEB_PING) - 1 && memcmp(buf, WEB_PING, f.len) == 0) {
		/* heartbeat: answered here, never queued, so it works while a command
		 * or script is busy. A frame of its own: cannot split other output. */
		httpd_ws_frame_t r = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)WEB_PONG,
		                       .len = sizeof(WEB_PONG) - 1 };
		return httpd_ws_send_frame(req, &r);
	}
	buf[f.len++] = '\n';				/* one frame = one line */
	if (s_rxCb && !s_rxCb(buf, f.len)) {
		static const char busy[] = "ERR busy\n";
		httpd_ws_frame_t r = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)busy, .len = sizeof(busy) - 1 };
		return httpd_ws_send_frame(req, &r);
	}
	return ESP_OK;
}

/* Read and discard the rest of frame f (header already read). */
static esp_err_t webDrain(httpd_req_t *req, httpd_ws_frame_t *f)
{
	uint8_t tmp[64];
	size_t  left = f->len;

	while (left > 0) {
		size_t n = (left > sizeof(tmp)) ? sizeof(tmp) : left;
		f->payload = tmp;
		esp_err_t err = httpd_ws_recv_frame_part(req, f, n);	/* payload only, in parts */
		if (err != ESP_OK) {
			return err;
		}
		left -= n;
	}
	return ESP_OK;
}

static void webOnClose(httpd_handle_t hd, int fd)
{
	s_open--;
	WEB_DBG("close fd %d (%d open)", fd, s_open);
	if (fd == s_fd) {
		s_fd = -1;
		ESP_LOGI(TAG, "page disconnected");
		if (s_connCb) {
			s_connCb(false);
		}
	}
	close(fd);
}

/* Server task: close every open connection - the page's WebSocket (-> webOnClose
 * -> connCb(false)) and any idle keep-alive connections left by vanished clients. */
static void webDropAll(void *arg)
{
	httpd_handle_t hd = s_server;
	int            fds[CONFIG_LWIP_MAX_SOCKETS];
	size_t         n = sizeof(fds) / sizeof(fds[0]);

	if (!hd || httpd_get_client_list(hd, &n, fds) != ESP_OK) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		httpd_sess_trigger_close(hd, fds[i]);
	}
	WEB_DBG("dropped %d connection(s)", (int)n);
}

/* esp_restart() shutdown handler (restarting task): have the server task send
 * the page a close frame (code 4001) and give it a moment to go out. */
static void webShutdown(void)
{
	if (!s_server || s_fd < 0) {
		return;
	}
	xSemaphoreTake(s_closed, 0);
	if (httpd_queue_work(s_server, webCloseWork, NULL) == ESP_OK &&
	    xSemaphoreTake(s_closed, pdMS_TO_TICKS(WEB_CLOSE_WAIT_MS)) == pdTRUE) {
		vTaskDelay(pdMS_TO_TICKS(WEB_CLOSE_FLUSH_MS));
	}
}

static void webCloseWork(void *arg)
{
	int fd = s_fd;
	if (s_server && fd >= 0) {
		uint8_t close[2 + 9] = { WEB_CLOSE_REBOOT >> 8, WEB_CLOSE_REBOOT & 0xFF,
		                         'r', 'e', 'b', 'o', 'o', 't', 'i', 'n', 'g' };
		httpd_ws_frame_t c = { .type = HTTPD_WS_TYPE_CLOSE, .payload = close, .len = sizeof(close) };
		httpd_ws_send_frame_async(s_server, fd, &c);
	}
	xSemaphoreGive(s_closed);
}

static void webSendWork(void *arg)
{
	webMsg_t *m = arg;
	if (s_server && m->fd == s_fd) {
		httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = m->data, .len = m->len };
		httpd_ws_send_frame_async(s_server, m->fd, &f);
	}
	free(m);
	xSemaphoreGive(s_sent);
}
