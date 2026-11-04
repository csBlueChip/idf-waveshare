#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cmd.h"

/*
 * Screen transitions: run inside lcdUpdate() in place of the normal diff update.
 *
 * Commands (handled for lcd's SCR command):
 *   SCR TR  name args | CUT | NONE    one-shot (next update)
 *   SCR TRP name args | CUT | NONE    persistent (NONE = CUT)
 *   SCR TR|TRP NONE NONE              disarm one-shot, persistent CUT
 *   SCR TR    reports "x > y" (x one-shot, y persistent); "x" if y is CUT; "> y" if x is NONE
 *   SCR TRP   reports y
 *
 *   CUT     no transition (instant). As a one-shot it overrides the persistent one.
 *   NONE    one-shot not armed: the persistent transition is used.
 *   ms      10..10000, duration of the whole transition
 *   PUSH    old image slides out while the new one slides in; T|B|L|R is the
 *           side the new image enters from (R: enters right, old pushed left),
 *           relative to the current ROT. Always full screen.
 *   FADE    every pixel blends from old to new colour. Dirty rectangle only.
 *   WIPE ms T|B|L|R   a hard edge sweeps in from that side, revealing the new
 *           image (neither image moves). Always full screen.
 *   DISSOLVE ms       pixels flip from old to new in a random order (a new
 *           order every run). Dirty rectangle only.
 *
 * A one-shot is used up only when a transition actually runs (screen changed,
 * not a typing update).
 *
 * ---------------------------------------------------------------------------
 * Adding a new transition
 * ---------------------------------------------------------------------------
 * 1. In scrtr.c write a frame function:
 *
 *        static void scrtrXxxFrame(const scrtrCtx_t *c, int y0, int rows, int p)
 *
 *    It renders one band of one frame: native rows y0 .. y0+rows-1, columns
 *    c->x0 .. c->x1-1, into c->out. Pixel (x,y) goes to
 *        c->out[(y - y0) * (c->x1 - c->x0) + (x - c->x0)]
 *    Read the old image from c->from and the new one from c->to; both are full
 *    native frames, pixel (x,y) at [y * c->w + x], RGB565.
 *    p is progress 0..1024. At p == 1024 the output MUST equal c->to exactly.
 *    The function is called for every band of every frame, so keep it fast and
 *    keep no state between calls (everything derives from c and p).
 *
 * 2. Add one line to s_table[] in scrtr.c:
 *
 *        { "XXX", SCRTR_ARGS_MS, false, scrtrXxxFrame },
 *
 *    name     command word (matched case-insensitively)
 *    args     SCRTR_ARGS_MS (ms) or SCRTR_ARGS_MS_DIR (ms T|B|L|R)
 *    full     true: whole screen every frame; false: dirty rectangle only
 *    frame    the function from step 1
 *
 * The framework does the rest: argument parsing, timing (p from elapsed time),
 * banding through the DMA buffer, the exact final frame, and the usage text.
 * For a direction, c->mx/c->my is the native unit vector the old image moves
 * in (already converted from T|B|L|R through the current ROT). c->seed is a
 * fresh random number for each run.
 */

typedef struct {
	const uint16_t *from;		/* on the panel now (native frame) */
	const uint16_t *to;		/* new frame (native) */
	uint16_t       *out;		/* band output, stride x1 - x0 */
	int             w, h;		/* native frame size */
	int             x0, y0, x1, y1;	/* area being drawn, x1/y1 exclusive */
	int             mx, my;		/* direction the old image moves (native unit vector) */
	uint32_t        seed;		/* random, new for every run */
} scrtrCtx_t;

/* What lcdUpdate() hands to scrtrRun(). */
typedef struct {
	const uint16_t *from, *to;
	uint16_t       *buf;		/* DMA staging buffer */
	int             bufPx;		/* its size in pixels */
	int             w, h;		/* native frame size */
	int             x0, y0, x1, y1;	/* dirty rectangle (native), x1/y1 exclusive */
	int             sx, sy;		/* native index step for logical +x / +y (current ROT) */
	esp_err_t     (*blit)(int x, int y, int w, int h);	/* send buf to panel, blocking */
} scrtrEnv_t;

/* Called by lcdUpdate() when the screen changed (not for typing updates).
 * Consumes an armed one-shot. Runs the transition and returns its result, or
 * returns ESP_ERR_NOT_FOUND for CUT: the caller then does a normal update. */
esp_err_t scrtrRun(const scrtrEnv_t *env);

/* Firmware's own one-off transition for the next scrtrRun(), e.g. ("FADE", 1000, 0).
 * Wins over SCR TR/TRP and leaves them untouched. dir = 'T'|'B'|'L'|'R' or 0.
 * name NULL cancels a pending one. false if name/ms/dir are invalid. */
bool scrtrOverride(const char *name, int ms, char dir);

/* Handle SCR TR / SCR TRP (args after TR/TRP). Returns NULL or error text. */
const char *scrtrCommand(bool persistent, int argc, char *argv[], cmdReply_t reply);
