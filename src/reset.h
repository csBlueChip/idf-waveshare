#pragma once

#include <stdbool.h>

/*
 * Resetting the device when no UI works.
 *
 * 1. SD:/sys/reset.nvram at boot (resetBootFile, from app_main right after
 *    sdInit, before the LCD, UI and most modules read NV):
 *      - the current settings are exported to SD:/sys/<timestamp>.nvram
 *        (always the b<boot>_<uptime> form: the clock is not set yet);
 *        if that fails it is logged and the reset goes ahead
 *      - the file is checked against the defaults; bad: nothing changed,
 *        renamed reset.bad
 *      - good: every key to its default (except sys.bootcnt), then the
 *        file's settings (an empty file = plain defaults); renamed
 *        reset.done (an old reset.done / reset.bad is replaced)
 *    Results go to the log.
 *
 * 2. The RESET! Wait screen (ui.c / uiscr.c; BOOT double press after MSC):
 *      NVRAM   export to SD:/sys/<timestamp>.nvram, reset NV (except
 *              sys.bootcnt), reboot. No card: "NVRAM - no backup" is the
 *              only mode (reset without export). Export fails: "No backup:
 *              <reason>" shown, reset anyway.
 *      Script  run SD:/sys/reset.s ($arg = SCRIPT) as a system script, then
 *              back to the RESET! screen
 *      FULL    export, reset NV, run reset.s ($arg = FULL), reboot
 *              (reset.s missing: error, nothing done)
 */

typedef enum {
	RESET_NV = 0,
	RESET_SCRIPT,
	RESET_FULL,
	RESET_MODES
} resetMode_t;

/* Boot: handle SD:/sys/reset.nvram if present. Call after sysInit(), sdInit(). */
void resetBootFile(void);

/* Main task, RESET! screen: carry out mode m (noSd: no card, NVRAM without a
 * backup). msg(a, b) shows progress (two lines); msg(NULL, NULL) = redraw the
 * RESET! screen (after reset.s, which may have drawn on it). Reboots for
 * NVRAM / FULL (a failed backup does not stop them); returns NULL after
 * Script, or an error: no reset.s (nothing done) / NV write failed. */
const char *resetRun(resetMode_t m, bool noSd, void (*msg)(const char *a, const char *b));
