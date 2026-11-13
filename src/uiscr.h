#pragma once

#include <stdbool.h>
#include "ui.h"

/*
 * Connection screens on the LCD (call from the main task only).
 *
 * Sequences (fades are a firmware one-off; SCR TR/TRP are never touched):
 *   boot        SCR AUTO 1 ; CLS 0 0 0 ; BL scr.bl ; boot square ; wait
 *   wait        ROT 1|3 (even ROT + 1) ; SCR AUTO 0 ; details ; fade in 500 ms
 *   connect     SCR AUTO 0 ; ROT 0 ; CLS 0 0 0 ; fade 1000 ; splash ; fade 1000 ; SCR AUTO 1
 *   disconnect  SCR AUTO 1 ; CLS 0 0 0 ; fade 1000 ; wait   (also on a UI switch)
 *
 * Details screen ("onStart"), font 1, ink 220 110 20 on black, 10 px side margins;
 * the interface (after "UI  : ") in white:
 *   WEB/TELNET:  UI  : WEB:80 | UI  : TELNET:23
 *                SSID: <wifi.ssid>
 *                Pwd : (hold BOOT)      password shown while BOOT (GP0) is held
 *                IP  : <wifi.ip> <wifi.cidr>
 *                WiFi: Waiting... | Link Active (white: a station has an IP)
 *   BLE:         UI  : BLE
 *                Name: <ble.name>
 *   MSC:         UI  : MSC - Flash Drive
 *                BOOT: hold to start    (BOOT held: USB disk mode, as FILES)
 *   RESET!:      UI  : RESET!
 *                MODE: NVRAM | Script | FULL   (the selected mode only, in
 *                                               white; no SD card:
 *                                               NVRAM - no backup)
 *                BOOT: TAP=mode, LONG=OK
 *                (rows 3-4: messages in white, e.g. "Long press again...")
 *   Lines too long for the screen end in '~'.
 *
 * Splash: SD:/sys/splash.png at 0,0 ("sys/splash.png" printed on the top line if missing).
 */

/* SD manager on (fm.c): fade to black, then "SD CARD MANAGER", the URL and
 * "BOOT: press to exit" in landscape. Not a Wait screen. */
void uiScrFm(const char *url);

/* Boot sequence (before the first wait). */
void uiScrBoot(void);

/* Wait: details screen for interface ifc (BLE, TELNET, WEB); others: nothing.
 * Turns on IMU ORIENT L AUTO (details follow the device turned 180 deg). */
void uiScrStart(uiIf_t ifc);

/* Client gone (or UI switch): fade to black, then wait for ifc. */
void uiScrDisconnect(uiIf_t ifc);

/* Client connected: IMU ORIENT off (view flip off), splash. */
void uiScrConnect(void);

/* BOOT long press down (show) / up (hide) while a Wi-Fi details screen is up. */
void uiScrPassword(bool show);

/* Wi-Fi link state: set (no drawing; used by the next details screen), or
 * set and redraw the WiFi line if a Wi-Fi details screen is up. */
void uiScrSetLink(bool up);
void uiScrLink(bool up);

/* True while a WiFi details screen (with a password line) is up. */
bool uiScrWantsButton(void);

/* True while any Wait (details) screen is up. */
bool uiScrWaiting(void);

/* RESET! screen: mode (resetMode_t) and no-card state for the next drawing. */
void uiScrResetSet(int mode, bool noSd);

/* RESET! screen up: redraw the MODE line / the two message rows. */
void uiScrResetMode(void);
void uiScrResetMsg(const char *a, const char *b);
