#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WEB_PORT	80

/*
 * Web UI: HTTP on port 80 + captive-portal DNS.
 *   /cfg   {"hist":n} - command history length from NV web.hist
 *   /fm... SD card manager (FILES WEB) - see fm.h
 *   POST /time?t=n  set the clock (n = the browser's local time in seconds;
 *          sent by both pages, so files written later get real FAT dates)
 *   /ws    WebSocket: each text frame from the page is one command line;
 *          everything sent with webSend() is pushed back as text frames
 *   other  the file SD:/www/<path> if it exists ("/" = /www/index.html; no
 *          caching, no folder listings, ".." etc. refused - see webpath.h);
 *          otherwise "/" is the built-in console page (assets/index.html)
 *          and anything else a 302 redirect to http://<rad.ip>/ (makes
 *          phones open the portal)
 * A text frame containing "<<GOTO /path>>" makes the console page open
 * that path (FILES WEB sends "<<GOTO /fm>>").
 * One client: a new WebSocket connection replaces the previous one; the old
 * page is closed with code 4000 ("replaced") and does not reconnect.
 * Heartbeat: the frame "<<PING>>" is answered "<<PONG>>" by the server task
 * (never passed to rxCb); the page sends it when idle to spot a dead link.
 * Planned restarts (any esp_restart(), e.g. REBOOT) close the page's
 * WebSocket with code 4001 ("rebooting") first.
 */

/* rxCb: one command line; return false if it could not be queued (client gets "ERR busy").
 * Both callbacks run in the HTTP server task. */
typedef bool (*webRxCb_t)(const uint8_t *data, size_t len);
typedef void (*webConnCb_t)(bool connected);

/* Start DNS and the HTTP server. Call after the AP is up. */
esp_err_t webStart(webRxCb_t rxCb, webConnCb_t connCb);

/* Stop the HTTP server and DNS. */
esp_err_t webStop(void);

/* Close all connections - the page's (connCb(false) is called) and any idle
 * ones. Any task. For clients that vanished without closing (left Wi-Fi). */
void webDropClient(void);

/* Send text to the connected page. Blocks until sent (max ~2 s).
 * ESP_ERR_INVALID_STATE if no page is connected. Not for use in the server task. */
esp_err_t webSend(const uint8_t *data, size_t len);
