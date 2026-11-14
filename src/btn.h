#pragma once

#include "esp_err.h"

/*
 * Buttons 1..n (n <= 10): tact switches (to GND, pull-up) or capacitive touch
 * pads. Button n -> GPIO from NV btn.gpio at boot (one GPIO per button, in
 * order; the first is always 0 = BOOT). Default:
 *
 *   button  1  2  3  4  5  6  7  8  9  10
 *   GPIO    0  2  4  5  6  7  8  9  10 11      (button 1 = BOOT, tact only)
 *
 * Allowed GPIOs: 0, 2..11, 43, 44 (not the UART pins, NV uart.tx / uart.rx).
 * TOUCH only on GPIO1..14 (43 / 44: TACT only). BTN n beyond the list: ERR.
 *
 * Commands registered by btnInit():
 *   BTN                               list buttons (n GPn ...) and global timings
 *   BTN n short long double [TACT|TOUCH]
 *                                     enable button n with these gestures (0|1);
 *                                     copies the global timings. BTN n 0 0 0 = off
 *   BTN SET deb|long|dbl|rpt ms       global timing (used by buttons enabled later)
 *   BTN n SET deb|long|dbl|rpt ms     timing for button n only
 *   BTN CAL                           re-calibrate all touch pads (hands off!)
 *
 * Timings (NV btn.deb/long/dbl/rpt at boot):
 *          deb debounce (30), long long-press hold (600), dbl double-click window (300),
 *          rpt repeat interval: with long on, 0 = DOWN/UP, >0 = REPEAT;
 *          with short only (long and double off), >0 = typematic SHORT repeat.
 *
 * Messages sent to the active UI, when they happen:
 *   <<BTN n SHORT>>  <<BTN n DOUBLE>>
 *   <<BTN n DOWN>>  <<BTN n UP>>               (long press, rpt = 0: DOWN when held
 *                                               for long, UP on release)
 *   <<BTN n REPEAT>>                           (long press, rpt > 0: at long, then
 *                                               every rpt ms while held)
 *
 * When SHORT is sent depends on what else is enabled:
 *   short only      -> on press, immediately; if the button's rpt > 0 it then
 *                      repeats (typematic) at long ms and every rpt ms while held
 *   + long          -> on release before long
 *   + double        -> dbl ms after release, if no second press
 */

#define BTN_COUNT	10

/* Register BTN and start the button task. Call after cmdInit() and uiInit(). */
esp_err_t btnInit(void);
