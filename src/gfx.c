#include "gfx.h"

#include <stdlib.h>
#include "esp_check.h"

#include "lcd.h"
#include "cmd.h"

static const char *TAG = "gfx";

#define GFX_SWAP(a, b)	do { int t_ = (a); (a) = (b); (b) = t_; } while (0)

static esp_err_t   gfxHLine(int x0, int x1, int y, uint16_t colour);
static esp_err_t   gfxRun(int x0, int y0, int x1, int y1, uint16_t colour);
static const char *gfxParse(char *argv[], int nCoords, int *v, uint16_t *colour);

static const char *gfxCmdPlot(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdLine(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdBox(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdFillBox(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdTri(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdFillTri(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdCircle(int argc, char *argv[], cmdReply_t reply);
static const char *gfxCmdFillCircle(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t gfxInit(void)
{
	static const struct {
		const char   *name;
		int           nArgs;
		cmdHandler_t  handler;
		const char   *help;
	} cmds[] = {
		{ "PLOT",  5, gfxCmdPlot,       "PLOT x y r g b" },
		{ "LINE",  7, gfxCmdLine,       "LINE x1 y1 x2 y2 r g b" },
		{ "BOX",   7, gfxCmdBox,        "BOX x y w h r g b" },
		{ "FBOX",  7, gfxCmdFillBox,    "FBOX x y w h r g b" },
		{ "TRI",   9, gfxCmdTri,        "TRI x1 y1 x2 y2 x3 y3 r g b" },
		{ "FTRI",  9, gfxCmdFillTri,    "FTRI x1 y1 x2 y2 x3 y3 r g b" },
		{ "CIRC",  6, gfxCmdCircle,     "CIRC cx cy rad r g b" },
		{ "FCIRC", 6, gfxCmdFillCircle, "FCIRC cx cy rad r g b" },
	};

	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		ESP_RETURN_ON_ERROR(cmdRegister(cmds[i].name, cmds[i].nArgs, cmds[i].nArgs,
		                                cmds[i].handler, cmds[i].help),
		                    TAG, "register %s", cmds[i].name);
	}
	return ESP_OK;
}

esp_err_t gfxPlot(int x, int y, uint16_t colour)
{
	return lcdFillRect(x, y, 1, 1, colour);
}

/* Bresenham, emitted as straight runs (one SPI transfer per run, not per pixel). */
esp_err_t gfxLine(int x0, int y0, int x1, int y1, uint16_t colour)
{
	int  dx    = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
	int  dy    = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
	int  err   = dx + dy;
	bool steep = -dy > dx;

	int rx = x0, ry = y0;		/* start of current run */
	int px = x0, py = y0;		/* last pixel in current run */

	for (;;) {
		if (steep ? (x0 != rx) : (y0 != ry)) {
			ESP_RETURN_ON_ERROR(gfxRun(rx, ry, px, py, colour), TAG, "line");
			rx = x0;
			ry = y0;
		}
		px = x0;
		py = y0;
		if (x0 == x1 && y0 == y1) {
			break;
		}
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0  += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0  += sy;
		}
	}
	return gfxRun(rx, ry, px, py, colour);
}

esp_err_t gfxBox(int x, int y, int w, int h, uint16_t colour)
{
	if (w < 1 || h < 1) {
		return ESP_ERR_INVALID_ARG;
	}
	ESP_RETURN_ON_ERROR(lcdFillRect(x, y, w, 1, colour), TAG, "box");		/* top */
	ESP_RETURN_ON_ERROR(lcdFillRect(x, y + h - 1, w, 1, colour), TAG, "box");	/* bottom */
	ESP_RETURN_ON_ERROR(lcdFillRect(x, y, 1, h, colour), TAG, "box");		/* left */
	return lcdFillRect(x + w - 1, y, 1, h, colour);					/* right */
}

esp_err_t gfxFillBox(int x, int y, int w, int h, uint16_t colour)
{
	if (w < 1 || h < 1) {
		return ESP_ERR_INVALID_ARG;
	}
	return lcdFillRect(x, y, w, h, colour);
}

esp_err_t gfxTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t colour)
{
	ESP_RETURN_ON_ERROR(gfxLine(x0, y0, x1, y1, colour), TAG, "tri");
	ESP_RETURN_ON_ERROR(gfxLine(x1, y1, x2, y2, colour), TAG, "tri");
	return gfxLine(x2, y2, x0, y0, colour);
}

/* Scanline fill between the long edge (0-2) and the two short edges, then the
 * outline, so FTRI covers exactly the same edge pixels as TRI. */
esp_err_t gfxFillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t colour)
{
	int ox0 = x0, oy0 = y0, ox1 = x1, oy1 = y1, ox2 = x2, oy2 = y2;

	/* sort by y: y0 <= y1 <= y2 */
	if (y0 > y1) { GFX_SWAP(y0, y1); GFX_SWAP(x0, x1); }
	if (y1 > y2) { GFX_SWAP(y1, y2); GFX_SWAP(x1, x2); }
	if (y0 > y1) { GFX_SWAP(y0, y1); GFX_SWAP(x0, x1); }

	if (y0 != y2) {
		for (int y = y0; y <= y2; y++) {
			int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
			int xb;
			if (y < y1) {
				xb = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
			} else if (y2 != y1) {
				xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
			} else {
				xb = x1;
			}
			ESP_RETURN_ON_ERROR(gfxHLine(xa, xb, y, colour), TAG, "ftri");
		}
	}
	return gfxTri(ox0, oy0, ox1, oy1, ox2, oy2, colour);
}

/* Midpoint circle */
esp_err_t gfxCircle(int cx, int cy, int rad, uint16_t colour)
{
	if (rad < 0) {
		return ESP_ERR_INVALID_ARG;
	}

	int x = rad, y = 0, err = 1 - rad;
	while (x >= y) {
		const int pts[8][2] = {
			{ cx + x, cy + y }, { cx - x, cy + y }, { cx + x, cy - y }, { cx - x, cy - y },
			{ cx + y, cy + x }, { cx - y, cy + x }, { cx + y, cy - x }, { cx - y, cy - x },
		};
		for (int i = 0; i < 8; i++) {
			ESP_RETURN_ON_ERROR(gfxPlot(pts[i][0], pts[i][1], colour), TAG, "circ");
		}
		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
	return ESP_OK;
}

esp_err_t gfxFillCircle(int cx, int cy, int rad, uint16_t colour)
{
	if (rad < 0) {
		return ESP_ERR_INVALID_ARG;
	}

	int x = rad, y = 0, err = 1 - rad;
	while (x >= y) {
		ESP_RETURN_ON_ERROR(gfxHLine(cx - x, cx + x, cy + y, colour), TAG, "fcirc");
		ESP_RETURN_ON_ERROR(gfxHLine(cx - x, cx + x, cy - y, colour), TAG, "fcirc");
		ESP_RETURN_ON_ERROR(gfxHLine(cx - y, cx + y, cy + x, colour), TAG, "fcirc");
		ESP_RETURN_ON_ERROR(gfxHLine(cx - y, cx + y, cy - x, colour), TAG, "fcirc");
		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
	return ESP_OK;
}

/* ================= Internals ================= */

static esp_err_t gfxHLine(int x0, int x1, int y, uint16_t colour)
{
	if (x0 > x1) {
		GFX_SWAP(x0, x1);
	}
	return lcdFillRect(x0, y, x1 - x0 + 1, 1, colour);
}

/* Fill the rectangle spanned by two points (a horizontal or vertical run). */
static esp_err_t gfxRun(int x0, int y0, int x1, int y1, uint16_t colour)
{
	if (x0 > x1) {
		GFX_SWAP(x0, x1);
	}
	if (y0 > y1) {
		GFX_SWAP(y0, y1);
	}
	return lcdFillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, colour);
}

/* Parse nCoords ints (-GFX_COORD_MAX..GFX_COORD_MAX) into v[], then "r g b". */
static const char *gfxParse(char *argv[], int nCoords, int *v, uint16_t *colour)
{
	for (int i = 0; i < nCoords; i++) {
		if (!cmdParseInt(argv[i], -GFX_COORD_MAX, GFX_COORD_MAX, &v[i])) {
			return "values must be -1000..1000";
		}
	}

	uint8_t r, g, b;
	if (!cmdParseU8(argv[nCoords], &r) || !cmdParseU8(argv[nCoords + 1], &g) ||
	    !cmdParseU8(argv[nCoords + 2], &b)) {
		return "colour must be 0..255";
	}
	*colour = lcdRgb(r, g, b);
	return NULL;
}

#define GFX_DRAW(call)	(((call) == ESP_OK) ? NULL : "LCD write failed")

/* ================= Commands ================= */

static const char *gfxCmdPlot(int argc, char *argv[], cmdReply_t reply)
{
	int v[2];
	uint16_t c;
	const char *err = gfxParse(argv, 2, v, &c);
	return err ? err : GFX_DRAW(gfxPlot(v[0], v[1], c));
}

static const char *gfxCmdLine(int argc, char *argv[], cmdReply_t reply)
{
	int v[4];
	uint16_t c;
	const char *err = gfxParse(argv, 4, v, &c);
	return err ? err : GFX_DRAW(gfxLine(v[0], v[1], v[2], v[3], c));
}

static const char *gfxCmdBox(int argc, char *argv[], cmdReply_t reply)
{
	int v[4];
	uint16_t c;
	const char *err = gfxParse(argv, 4, v, &c);
	if (err) {
		return err;
	}
	if (v[2] < 1 || v[2] > GFX_SIZE_MAX || v[3] < 1 || v[3] > GFX_SIZE_MAX) {
		return "w and h must be 1..1000";
	}
	return GFX_DRAW(gfxBox(v[0], v[1], v[2], v[3], c));
}

static const char *gfxCmdFillBox(int argc, char *argv[], cmdReply_t reply)
{
	int v[4];
	uint16_t c;
	const char *err = gfxParse(argv, 4, v, &c);
	if (err) {
		return err;
	}
	if (v[2] < 1 || v[2] > GFX_SIZE_MAX || v[3] < 1 || v[3] > GFX_SIZE_MAX) {
		return "w and h must be 1..1000";
	}
	return GFX_DRAW(gfxFillBox(v[0], v[1], v[2], v[3], c));
}

static const char *gfxCmdTri(int argc, char *argv[], cmdReply_t reply)
{
	int v[6];
	uint16_t c;
	const char *err = gfxParse(argv, 6, v, &c);
	return err ? err : GFX_DRAW(gfxTri(v[0], v[1], v[2], v[3], v[4], v[5], c));
}

static const char *gfxCmdFillTri(int argc, char *argv[], cmdReply_t reply)
{
	int v[6];
	uint16_t c;
	const char *err = gfxParse(argv, 6, v, &c);
	return err ? err : GFX_DRAW(gfxFillTri(v[0], v[1], v[2], v[3], v[4], v[5], c));
}

static const char *gfxCmdCircle(int argc, char *argv[], cmdReply_t reply)
{
	int v[3];
	uint16_t c;
	const char *err = gfxParse(argv, 3, v, &c);
	if (err) {
		return err;
	}
	if (v[2] < 0 || v[2] > GFX_SIZE_MAX) {
		return "rad must be 0..1000";
	}
	return GFX_DRAW(gfxCircle(v[0], v[1], v[2], c));
}

static const char *gfxCmdFillCircle(int argc, char *argv[], cmdReply_t reply)
{
	int v[3];
	uint16_t c;
	const char *err = gfxParse(argv, 3, v, &c);
	if (err) {
		return err;
	}
	if (v[2] < 0 || v[2] > GFX_SIZE_MAX) {
		return "rad must be 0..1000";
	}
	return GFX_DRAW(gfxFillCircle(v[0], v[1], v[2], c));
}
