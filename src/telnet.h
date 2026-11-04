#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Minimal telnet server: one client at a time on TELNET_PORT.
 *   - a second connection is closed immediately (rejected)
 *   - telnet IAC negotiation is stripped from input, NULs dropped
 *   - "\n" in output is sent as "\r\n"
 * The server task blocks in select() - no polling.
 */

#define TELNET_PORT	23

typedef void (*telnetRxCb_t)(const uint8_t *data, size_t len);	/* runs in telnet task */
typedef void (*telnetConnCb_t)(bool connected);

/* Start listening. rxCb receives cleaned input; connCb reports client connect/disconnect. */
esp_err_t telnetStart(telnetRxCb_t rxCb, telnetConnCb_t connCb);

/* Close the client and the listener; waits for the server task to exit. */
esp_err_t telnetStop(void);

/* Close the connected client as if it had disconnected (connCb(false) is
 * called). Any task. For clients that vanished without closing (left Wi-Fi). */
void telnetDropClient(void);

/* Send text to the connected client. ESP_ERR_INVALID_STATE if none. */
esp_err_t telnetSend(const uint8_t *data, size_t len);
