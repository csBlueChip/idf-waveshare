#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ser.h"

/*
 * User interface manager. Exactly one interface is active at a time;
 * the device boots on the one in NV sys.ui (default BLE; MSC = the MSC
 * Wait screen).
 * The browser pages send their local time (POST /time), setting the clock.
 *
 *   BLE      Nordic UART Service (ble_serial)
 *   TELNET   Wi-Fi access point (wifi_ap) + telnet on NV telnet.port (telnet)
 *   WEB      Wi-Fi access point + web console on NV web.port; captive portal
 *            (DNS + DHCP URI) unless NV web.captive 0 (web)
 *   UART     UART0 on the header (TX GPIO43, RX GPIO44), 8N1, NV uart.baud
 *   USB      the chip's USB-Serial/JTAG port (USB-C)
 *
 * Serial UIs (UART, USB): replies end in "\r\n"; input lines end on CR, LF
 * or CR+LF; typed characters are echoed (NV ser.echo) and Backspace erases.
 * The first line received counts as the client connecting (splash); the
 * session lasts until the UI changes or the board reboots. Three empty lines
 * in a row show "[Startup Complete - UART|USB UI active]" again.
 * "+++" or "---" alone on a line, then 1 s with no input: debug on / off
 * (logs also on the UI port, see log.h).
 *
 * Received bytes from the active interface are assembled into lines here.
 * Each connect, disconnect or interface switch starts a new session, and any
 * partial line from the previous session is discarded.
 *
 * Command registered by uiInit():
 *   UI          report active interface
 *   UI BLE      switch to BLE
 *   UI TELNET   send SSID/PWD/IP/PORT (from NV), then switch to telnet
 *   UI WEB      send SSID/PWD/URL (from NV), then switch to the web console
 *   UI UART     send BAUD (from NV), then switch to the UART console
 *   UI USB      switch to the USB serial console
 *   UI MSC      MSC Wait screen (no console): BOOT held starts USB disk mode
 *   UI NEXT     switch to the next one (WEB -> TELNET -> BLE -> USB -> UART -> MSC -> WEB)
 *   EVENTS [src 0|1]  which <<src ...>> events the client receives, src =
 *               BTN IMU UI SCRIPT ALL (this boot only; all on at boot)
 *
 * Events sent: <<UI CONNECT>> (the client's first message), <<UI DISCONNECT>>,
 * <<UI LINK UP>> (a Wi-Fi station got an IP, every join), <<UI LINK DOWN>>
 * (none left; the telnet/web session is dropped, so only scripts see it).
 *
 * BOOT on the Wait screen: long press (NV btn.long) shows the Wi-Fi password
 * until released (MSC screen: starts USB disk mode, as FILES); double press
 * (second press within NV btn.dbl) switches to the next interface,
 * WEB -> TELNET -> BLE -> USB -> UART -> MSC -> RESET! -> WEB (this session
 * only).
 *
 * RESET! (double press only: not a sys.ui value, no UI command; no
 * interface runs, onWait is not run). Screen:
 *   UI  : RESET!
 *   MODE: NVRAM | Script | FULL   (the selected mode only, in white; no SD
 *                                  card: "NVRAM - no backup", the only mode)
 *   BOOT: TAP=mode, LONG=OK
 * Tap = next mode (taken once btn.dbl has passed with no second press, so a
 * double press still moves on to WEB). Long press: "Long press
 * again..."; a second long press within UI_RST_CONFIRM_MS of releasing the
 * first carries it out (see reset.h); a tap or the time-out cancels.
 * Mode starts at NVRAM each time the screen is entered.
 */

/* DEBUG: 1 = boot into TELNET whatever NV sys.ui says. */
#define UI_DEBUG_BOOT_TELNET	0

typedef enum {
	UI_NONE = 0,
	UI_BLE,
	UI_TELNET,
	UI_WEB,
	UI_UART,
	UI_USB,
	UI_MSC,		/* Wait screen only: hold BOOT to start USB disk mode (FILES) */
	UI_RESET,	/* Wait screen only: RESET! (BOOT double press after MSC) */
} uiIf_t;

/* Init the BLE stack (not advertising), boot square; register UI, EVENTS. No
 * interface is active until uiBegin(). Call after cmdInit(). */
esp_err_t uiInit(void);

/* Serial receive callback (pass to serInit()). */
void uiSerRx(serPort_t port, const uint8_t *data, size_t len);

/* End of boot: start the boot interface (NV sys.ui; BLE if it fails), show
 * its Wait screen; a serial UI then gets "[Startup Complete - UART|USB UI
 * active]" as the last start-up line. Before this no interface runs. */
void uiBegin(void);

/* Block until a complete line arrives from the active interface (no CR/LF). */
void uiReadLine(char *line, size_t max);

/* Send text to the active interface (dropped if none). Thread-safe: each call
 * goes out whole, so command replies and button events never interleave. */
void uiSend(const char *text);

/* Same, without passing <<...>> events to a running script (script output). */
void uiSendText(const char *text);

/* A client is connected (main task view). */
bool uiHasClient(void);

/* Switch interface now (stops the current one first). */
esp_err_t uiSelect(uiIf_t ifc);

/* Stop the active interface; nothing is active afterwards (e.g. disk mode). */
void uiDisable(void);

uiIf_t uiActive(void);

/* Run fn(arg) in the main (command) task, between commands - for work that
 * must not race with command handlers (e.g. ROT from the IMU task). Never
 * blocks; false if the queue is full. Dropped if a client connects or
 * disconnects before it runs. */
typedef void (*uiMainFn_t)(int arg);
bool uiRunMain(uiMainFn_t fn, int arg);

/* SD manager (fm.c) turned off: back to the Wait screen if no client. Any task. */
void uiFmChanged(void);
