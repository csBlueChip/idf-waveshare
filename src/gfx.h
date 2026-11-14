#pragma once

#include <stdint.h>
#include "esp_err.h"

/*
 * 2D drawing on the LCD (current rotation, 0,0 = top-left).
 * Colours are RGB565 (lcdRgb()). Anything off-screen is clipped.
 *
 * Commands registered by gfxInit():
 *   PLOT  x y colour
 *   LINE  x1 y1 x2 y2 colour
 *   BOX   x y w h colour       FBOX  x y w h colour
 *   TRI   x1 y1 x2 y2 x3 y3 colour
 *   FTRI  x1 y1 x2 y2 x3 y3 colour
 *   CIRC  cx cy rad colour     FCIRC cx cy rad colour
 * colour = r,g,b, RRGGBB or @h,s,l (cmdParseColour)
 */

#define GFX_COORD_MAX	1000	/* coordinates -1000..1000 */
#define GFX_SIZE_MAX	1000	/* w, h 1..1000; radius 0..1000 */

/* Register drawing commands. Call after lcdInit() and cmdInit(). */
esp_err_t gfxInit(void);

esp_err_t gfxPlot(int x, int y, uint16_t colour);
esp_err_t gfxLine(int x0, int y0, int x1, int y1, uint16_t colour);
esp_err_t gfxBox(int x, int y, int w, int h, uint16_t colour);
esp_err_t gfxFillBox(int x, int y, int w, int h, uint16_t colour);
esp_err_t gfxTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t colour);
esp_err_t gfxFillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t colour);
esp_err_t gfxCircle(int cx, int cy, int rad, uint16_t colour);
esp_err_t gfxFillCircle(int cx, int cy, int rad, uint16_t colour);
