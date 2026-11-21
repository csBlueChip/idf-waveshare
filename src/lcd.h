#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "font.h"

/*
 * 1.47" ST7789T LCD, 172x320, SPI.
 * Colours are RGB565 - build them with lcdRgb() (each channel rounded to
 * the nearest 565 level).
 *
 * Double buffered (two frames in PSRAM): all drawing goes to the hidden
 * buffer; lcdUpdate() sends only the pixels that differ from what is on the
 * panel, then swaps. By default lcdUpdate() runs after every command.
 *
 * Commands registered by lcdInit():
 *   CLS colour  clear to colour (r,g,b, RRGGBB or @h,s,l), show dimensions
 *   ROT [n]     rotation n mod 4 (0/2 portrait, 1/3 landscape) for new drawing;
 *               alone: rot:n;
 *               screen content is left as it is; replies W/H
 *   BL pct      backlight 0..100
 *   SCR AUTO 0|1   update after every command (default 1)
 *   SCR UPD        update now
 *   SCR FLIP [P|L|PL|NONE]  upside-down mounting: P swaps ROT 0/2, L swaps ROT 1/3
 *                  (this boot only, off at boot: set it in /sys/onBoot.s; screen
 *                  re-sent). No arg: report
 *   SCR SHOT [name] [-f]   save the glass as SD:/scrshots/name.png (see img.h)
 *   SCR TR|TRP ... screen transitions (see scrtr.h)
 */

/* Init SPI, panel and backlight; clear to black; register commands. Call after cmdInit(). */
esp_err_t lcdInit(void);

/* Current dimensions (depend on rotation). */
uint16_t lcdWidth(void);
uint16_t lcdHeight(void);

/* Clear to colour r,g,b and make it the CLS colour (like CLS, no text). */
esp_err_t lcdCls(uint8_t r, uint8_t g, uint8_t b);

/* 0..3, each step 90 degrees. Software only: changes how new drawing maps
 * onto the (native portrait) frame buffer; existing content is untouched. */
esp_err_t lcdSetRotation(uint8_t rot);
uint8_t   lcdGetRotation(void);

/* View flip: the panel shows the whole frame turned 180 degrees (both axes
 * mirrored in the panel controller; screen re-sent). Drawing, ROT, text boxes
 * and SCR SHOT are unchanged. Not stored (IMU ORIENT ... AUTO uses it).
 * Call from the main task. */
esp_err_t lcdSetViewFlip(bool on);
bool      lcdGetViewFlip(void);

/* SCR FLIP for portrait (false) or landscape (true) ROTs. */
bool      lcdGetFlip(bool landscape);

/* Backlight 0..100 %. */
/* Backlight level 0..NV scr.bl.steps (0 = off): perceptual (CIE L*) steps. */
esp_err_t lcdBacklight(uint8_t level);
/* Backlight at lightness pct 0..100 % (CIE L*); lcdGetBacklight() unchanged. */
esp_err_t lcdBacklightLightness(uint8_t pct);
uint8_t   lcdGetBacklight(void);

/* Backlight pin off and held through deep sleep (released by lcdInit()). */
void lcdDeepSleepPrep(void);

/* 8-bit RGB -> RGB565. */
uint16_t lcdRgb(uint8_t r, uint8_t g, uint8_t b);

esp_err_t lcdFill(uint16_t colour);
esp_err_t lcdFillRect(int x, int y, int w, int h, uint16_t colour);

/* Draw a w x h block of RGB565 pixels (row-major, stride w) at x,y. Clipped to screen. */
esp_err_t lcdDrawPixels(int x, int y, int w, int h, const uint16_t *pixels);

/* Blend one row of w RGBA8888 pixels at x,y over what is in the back buffer:
 * out = src * a + screen * (1 - a); alpha 0 = not drawn, 255 = solid.
 * Clipped to screen. */
void lcdBlendRow(int x, int y, int w, const uint8_t *rgba);

/* Draw anti-aliased text in font f (NULL = font 1) at pixel x,y.
 * '\n' starts a new line at x. Characters not fully on screen are skipped. */
esp_err_t lcdDrawText(const font_t *f, int x, int y, const char *str, uint16_t fg, uint16_t bg);

/* Draw one character (width: fontCharWidth()). */
esp_err_t lcdDrawChar(const font_t *f, int x, int y, char c, uint16_t fg, uint16_t bg);

/* Same, with transparency: fgNone = the glyph shows what is under it (paper
 * drawn around it), bgNone = only the glyph is drawn, blended onto what is
 * there; both = nothing drawn. */
esp_err_t lcdDrawCharT(const font_t *f, int x, int y, char c, uint16_t fg, uint16_t bg, bool fgNone, bool bgNone);

/* Text grid (font 1 cells) inside a margin. Depends on rotation. */
int lcdTextCols(void);
int lcdTextRows(void);

/* Clear a field of fieldChars cells at grid (col,row) to the CLS colour and draw
 * single-line text in it (truncated to the field). ESP_ERR_INVALID_SIZE if off-grid. */
esp_err_t lcdPrintAt(int col, int row, int fieldChars, const char *text);

/* Move the contents of rect (x,y,w,h) up by dy pixels; fill the freed rows with 'fill'. */
esp_err_t lcdScrollUp(int x, int y, int w, int h, int dy, uint16_t fill);

/* Default text colours: background = last CLS colour, text = black/white for contrast. */
void lcdGetTextColours(uint16_t *fg, uint16_t *bg);

/* Read logical row y (current rotation) of what is on the glass: lcdWidth() pixels. */
void lcdReadRow(int y, uint16_t *out);

/* Boot marker: 10x10 square in a random bright colour at a random x on the
 * bottom edge of the glass (ROT 0, y = 310). Drawn only; call lcdUpdate*(). */
void lcdBootMarker(void);

/* Push changes from the hidden buffer to the panel, then swap buffers.
 * Runs the selected screen transition (scrtr), if any. */
esp_err_t lcdUpdate(void);

/* Same, but never runs a transition and leaves a one-shot armed (typing). */
esp_err_t lcdUpdateNoTr(void);

/* Automatic update after each command (on by default). */
void lcdSetAutoUpdate(bool on);
bool lcdGetAutoUpdate(void);

/* Web mirror. on: keep a copy of what is on the panel; notify (any task) is
 * called when there is something for lcdMirrorTake(). The first take after
 * turning it on sends the whole frame. off: the copy is freed. */
esp_err_t lcdMirrorSet(bool on, void (*notify)(void));

/* Next band of the changed area for the mirror, at most max bytes: header
 * 'M', rotation as mapped (0..3: ROT with SCR FLIP; the picture is upright
 * when turned by -90 deg x that), x y w h (u16 little-endian, native 172 x 320
 * panel pixels), then w x h RGB565 (u16 little-endian). w = h = 0: only the
 * rotation changed. Returns the bytes written, 0 = nothing to send. */
#define LCD_MIR_HDR	10
size_t lcdMirrorTake(uint8_t *out, size_t max);
