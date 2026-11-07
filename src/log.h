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
 * Volume per device: 0 off, 1 errors, 2 + warnings, 3 + info, 4 + "DBG ..."
 * lines (full, the default). Devices are groups of log tags:
 *   BLE (ble NimBLE BLE_INIT BT_* ...)  WIFI (wifi pp net80211 phy* esp_netif* ...)
 *   WEB (web httpd* dns)  SD (sd sdmmc* diskio* vfs_fat* usbdisk tusb*)
 *   SCRIPT  IMU  SYS (every other tag). Lines that are not esp_log lines
 *   (dbgf, DEBUG text) always show.
 * Set at boot from SD:/sys/debug.cfg ("DEVICE=n" lines, '#' comments; no
 * file = all 4; a device missing from it stays 4). Messages before
 * logLoadCfg() are not filtered.
 *
 * Command registered by logInit():
 *   LOG [UART|USB|BOTH|AUTO]   set / show the mode
 *   LOG DEBUG [device]         show the volume of all devices / one
 *   LOG DEBUG device n [SAVE]  set it (this boot; SAVE: also in debug.cfg)
 *   LOG DEBUG ALL [n [SAVE]]   all devices: list / set (SAVE: as LOG DEBUG SAVE)
 *   LOG DEBUG SAVE             show the old debug.cfg (UI and log), then
 *                              rewrite it: header + every current volume
 * Messages before logInit() (ROM, bootloader, early start-up) and crash
 * reports keep the build-time console (both ports).
 */

typedef enum { LOG_AUTO, LOG_UART, LOG_USB, LOG_BOTH } logMode_t;

/* Hook the log output, register LOG. Call after serInit() and cmdInit(). */
esp_err_t logInit(void);

/* Read SD:/sys/debug.cfg into the device volumes. Call after sdInit(). */
void logLoadCfg(void);

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
