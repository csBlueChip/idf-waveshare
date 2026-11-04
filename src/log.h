#pragma once

#include <stdbool.h>
#include "ser.h"

/*
 * Log output routing. All ESP_LOGx messages and dbgf() lines go to the serial
 * ports (as "\r\n" lines):
 *   UI = UART        -> USB serial
 *   UI = USB serial  -> UART
 *   any other UI     -> both
 *   debug on (+++ in a serial UI) -> both, including the UI port
 * That is the AUTO mode. LOG UART|USB|BOTH fixes the port(s) instead (this
 * boot only; debug still adds the other port); LOG AUTO goes back.
 *
 * Command registered by logInit():
 *   LOG [UART|USB|BOTH|AUTO]   set / show the mode
 * Messages before logInit() (ROM, bootloader, early start-up) and crash
 * reports keep the build-time console (both ports).
 */

typedef enum { LOG_AUTO, LOG_UART, LOG_USB, LOG_BOTH } logMode_t;

/* Hook the log output, register LOG. Call after serInit() and cmdInit(). */
esp_err_t logInit(void);

/* Serial port used by the UI (SER_UART / SER_USB), or -1 for none. Also ends debug. */
void logSetUiPort(int port);

/* Debug: logs also go to the UI port. */
void logSetDebug(bool on);
bool logGetDebug(void);

/* Routing mode (LOG command). */
void      logSetMode(logMode_t mode);
logMode_t logGetMode(void);

/* printf-style log line; '\n' is added. Max 200 characters. Any task, not ISRs. */
void dbgf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
