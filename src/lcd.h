#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "font.h"

/*
 * 1.47" ST7789T LCD, 172x320, SPI.
 * Colours are RGB565 - build them with lcdRgb().
 *
 * Double buffered (two frames in PSRAM): all drawing goes to the hidden
 * buffer; lcdUpdate() sends only the pixels that differ from what is on the
 * panel, then swaps. By default lcdUpdate() runs after every command.
 *
 * Commands registered by lcdInit():
 *   CLS r g b   clear to colour, show dimensions
 *   ROT n       rotation n mod 4 (0/2 portrait, 1/3 landscape) for new drawing;
 *               screen content is left as it is; replies W/H
 *   BL pct      backlight 0..100
 *   SCR AUTO 0|1   update after every command (default 1)
 *   SCR UPD        update now
 *   SCR FLIP [P|L|PL|NONE]  upside-down mounting: P swaps ROT 0/2, L swaps ROT 1/3
 *                  (stored in NV scr.pflip/scr.lflip; screen re-sent). No arg: report
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

/* Backlight 0..100 %. */
esp_err_t lcdBacklight(uint8_t pct);
uint8_t   lcdGetBacklight(void);

/* Backlight pin off and held through deep sleep (released by lcdInit()). */
void lcdDeepSleepPrep(void);

/* 8-bit RGB -> RGB565. */
uint16_t lcdRgb(uint8_t r, uint8_t g, uint8_t b);

esp_err_t lcdFill(uint16_t colour);
esp_err_t lcdFillRect(int x, int y, int w, int h, uint16_t colour);

/* Draw a w x h block of RGB565 pixels (row-major, stride w) at x,y. Clipped to screen. */
esp_err_t lcdDrawPixels(int x, int y, int w, int h, const uint16_t *pixels);

/* Draw anti-aliased text in font f (NULL = font 1) at pixel x,y.
 * '\n' starts a new line at x. Characters not fully on screen are skipped. */
esp_err_t lcdDrawText(const font_t *f, int x, int y, const char *str, uint16_t fg, uint16_t bg);

/* Draw one character (width: fontCharWidth()). */
esp_err_t lcdDrawChar(const font_t *f, int x, int y, char c, uint16_t fg, uint16_t bg);

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
