#include "lcd.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"

#include "cmd.h"
#include "font.h"
#include "scrtr.h"
#include "img.h"
#include "nv.h"

static const char *TAG = "lcd";

/* ---------- Hardware ---------- */
#define LCD_HOST		SPI2_HOST
#define LCD_PIN_MOSI		45
#define LCD_PIN_SCLK		40
#define LCD_PIN_CS		42
#define LCD_PIN_DC		41
#define LCD_PIN_RST		39
#define LCD_PIN_BL		46
#define LCD_PCLK_HZ		(40 * 1000 * 1000)

#define LCD_NATIVE_W		172
#define LCD_NATIVE_H		320
#define LCD_GAP			34	/* panel RAM is 240 wide; visible area is centred */

#define LCD_BUF_PX		(LCD_NATIVE_H * 20)	/* DMA staging buffer: 20 lines at max width */
#define LCD_FB_PX		(LCD_NATIVE_W * LCD_NATIVE_H)	/* one full frame */

#define LCD_BL_MODE		LEDC_LOW_SPEED_MODE
#define LCD_BL_TIMER		LEDC_TIMER_0
#define LCD_BL_CHANNEL		LEDC_CHANNEL_0
#define LCD_BL_RES		LEDC_TIMER_10_BIT
#define LCD_BL_MAX		1023
#define LCD_BL_FREQ_HZ		5000

/* ---------- Screen layout ---------- */
#define LCD_TEXT_MARGIN		6

/* ---------- State ---------- */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static SemaphoreHandle_t         s_txDone;
static uint16_t                 *s_buf;		/* internal DMA staging buffer */
static uint16_t                 *s_fb[2];		/* frame buffers in PSRAM */
static int                       s_front;		/* s_fb[s_front] is on the panel */
static bool                      s_auto = true;	/* lcdUpdate() after each command */
static bool                      s_forceFull;	/* next update sends the dirty area unconditionally */
static bool                      s_dirty;
static int                       s_dx0, s_dy0, s_dx1, s_dy1;	/* dirty rect, x1/y1 exclusive */
static uint16_t                  s_w = LCD_NATIVE_W;
static uint16_t                  s_h = LCD_NATIVE_H;
static uint8_t                   s_rot;			/* requested (logical) rotation */
static uint8_t                   s_blPct;		/* backlight as last set */
static bool                      s_pflip, s_lflip;	/* upside-down mounting: 0<->2, 1<->3 */
static bool                      s_vflip;		/* view flip: panel shows the frame turned 180 */
static int                       s_base, s_sx, s_sy;	/* logical (x,y) -> native index: base + x*sx + y*sy */
static uint8_t                   s_bgR, s_bgG, s_bgB;	/* last CLS colour (text/background) */

/* Waveshare power/gamma settings for this panel */
static const struct {
	uint8_t cmd;
	uint8_t len;
	uint8_t data[14];
} s_vendorInit[] = {
	{ 0xB2,  5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 } },	/* porch */
	{ 0xB7,  1, { 0x75 } },				/* gate control */
	{ 0xBB,  1, { 0x1A } },				/* VCOM */
	{ 0xC0,  1, { 0x80 } },				/* LCM control */
	{ 0xC2,  2, { 0x01, 0xFF } },			/* VDV/VRH enable */
	{ 0xC3,  1, { 0x13 } },				/* VRH */
	{ 0xC4,  1, { 0x20 } },				/* VDV */
	{ 0xC6,  1, { 0x0F } },				/* frame rate */
	{ 0xD0,  2, { 0xA4, 0xA1 } },			/* power control */
	{ 0xE0, 14, { 0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30 } },
	{ 0xE1, 14, { 0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E } },
};

/* Rotation is done in software: frame buffers and the panel stay in native
 * portrait (172x320). Each rotation maps logical (x,y) to a native index
 * base + x*sx + y*sy:
 *   0: ( x,        y      )      2: ( W-1-x,  H-1-y )
 *   1: ( W-1-y,    x      )      3: ( y,      H-1-x )
 * So ROT changes how new drawing lands; existing content stays on the glass. */
#define LCD_NW	LCD_NATIVE_W
#define LCD_NH	LCD_NATIVE_H
static const struct {
	int base, sx, sy;
} s_xform[4] = {
	{ 0,                         1,       LCD_NW },
	{ LCD_NW - 1,                LCD_NW, -1      },
	{ LCD_NW * LCD_NH - 1,      -1,      -LCD_NW },
	{ (LCD_NH - 1) * LCD_NW,    -LCD_NW,  1      },
};

/* ---------- Forward decls ---------- */
static bool        lcdOnTxDone(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx);
static esp_err_t   lcdUpdateEx(bool allowTr);
static bool        lcdChanged(void);
static esp_err_t   lcdSendDiff(void);
static void        lcdSyncSwap(void);
static esp_err_t   lcdBlit(int x, int y, int w, int h);
static esp_err_t   lcdSendBand(int x0, int x1, int y0, int y1);
static void        lcdMarkDirty(int x, int y, int w, int h);
static void        lcdAfterCommand(void);
static esp_err_t   lcdPanelOrient(void);
static esp_err_t   lcdBacklightInit(void);
static void        lcdTextColours(uint16_t *fg, uint16_t *bg);
static esp_err_t   lcdShowScreen(cmdReply_t reply);
static const char *lcdCmdCls(int argc, char *argv[], cmdReply_t reply);
static const char *lcdCmdRot(int argc, char *argv[], cmdReply_t reply);
static const char *lcdCmdBl(int argc, char *argv[], cmdReply_t reply);
static const char *lcdCmdScr(int argc, char *argv[], cmdReply_t reply);
static const char *lcdCmdFlip(int argc, char *argv[], cmdReply_t reply);
static esp_err_t   lcdResend(void);

#define LCD_BACK	(s_fb[s_front ^ 1])
#define LCD_FRONT	(s_fb[s_front])

/* ================= Public API ================= */

esp_err_t lcdInit(void)
{
	s_txDone = xSemaphoreCreateBinary();
	s_buf    = heap_caps_malloc(LCD_BUF_PX * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	s_fb[0]  = heap_caps_calloc(LCD_FB_PX, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	s_fb[1]  = heap_caps_calloc(LCD_FB_PX, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	if (!s_txDone || !s_buf || !s_fb[0] || !s_fb[1]) {
		ESP_LOGE(TAG, "out of memory (frame buffers need PSRAM)");
		return ESP_ERR_NO_MEM;
	}

	ESP_RETURN_ON_ERROR(lcdBacklightInit(), TAG, "backlight init");

	spi_bus_config_t bus = {
		.mosi_io_num     = LCD_PIN_MOSI,
		.miso_io_num     = -1,
		.sclk_io_num     = LCD_PIN_SCLK,
		.quadwp_io_num   = -1,
		.quadhd_io_num   = -1,
		.max_transfer_sz = LCD_BUF_PX * sizeof(uint16_t),
	};
	ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

	esp_lcd_panel_io_spi_config_t ioCfg = {
		.dc_gpio_num         = LCD_PIN_DC,
		.cs_gpio_num         = LCD_PIN_CS,
		.pclk_hz             = LCD_PCLK_HZ,
		.lcd_cmd_bits        = 8,
		.lcd_param_bits      = 8,
		.spi_mode            = 0,
		.trans_queue_depth   = 10,
		.on_color_trans_done = lcdOnTxDone,
	};
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioCfg, &s_io),
	                    TAG, "panel io");

	esp_lcd_panel_dev_config_t panelCfg = {
		.reset_gpio_num = LCD_PIN_RST,
		.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
		.data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,	/* send uint16_t as-is, no byte swap */
		.bits_per_pixel = 16,
	};
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panelCfg, &s_panel), TAG, "panel");

	ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

	for (size_t i = 0; i < sizeof(s_vendorInit) / sizeof(s_vendorInit[0]); i++) {
		ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, s_vendorInit[i].cmd,
		                                              s_vendorInit[i].data, s_vendorInit[i].len),
		                    TAG, "vendor init 0x%02X", s_vendorInit[i].cmd);
	}

	ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
	ESP_RETURN_ON_ERROR(lcdPanelOrient(), TAG, "orientation");
	s_pflip = nvGetInt("scr.pflip");
	s_lflip = nvGetInt("scr.lflip");
	ESP_RETURN_ON_ERROR(lcdSetRotation(0), TAG, "rotation");
	ESP_RETURN_ON_ERROR(lcdFill(lcdRgb(0, 0, 0)), TAG, "clear");
	s_forceFull = true;					/* panel RAM is random: send everything */
	ESP_RETURN_ON_ERROR(lcdUpdate(), TAG, "first update");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");
	ESP_RETURN_ON_ERROR(lcdBacklight(nvGetInt("scr.bl")), TAG, "backlight");

	ESP_RETURN_ON_ERROR(cmdRegister("CLS", 3, 3, lcdCmdCls, "CLS r g b  clear screen (0..255)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("ROT", 1, 1, lcdCmdRot, "ROT n  drawing rotation (n mod 4)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("BL", 1, 1, lcdCmdBl, "BL pct  backlight (0..100)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("SCR", 1, 4, lcdCmdScr, "SCR AUTO 0|1 | UPD | SHOT [name] [-f] | TR|TRP [name args|CUT|NONE] | FLIP [P|L|PL|NONE]"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdSetPostHook(lcdAfterCommand), TAG, "hook");

	ESP_LOGI(TAG, "ready %ux%u", s_w, s_h);
	return ESP_OK;
}

uint16_t lcdWidth(void)
{
	return s_w;
}

uint16_t lcdHeight(void)
{
	return s_h;
}

esp_err_t lcdSetRotation(uint8_t rot)
{
	if (rot > 3) {
		return ESP_ERR_INVALID_ARG;
	}
	bool    landscape = rot & 1;
	uint8_t phys      = rot;
	if (landscape ? s_lflip : s_pflip) {
		phys ^= 2;				/* flip: turn 180 degrees */
	}

	s_rot  = rot;
	s_w    = landscape ? LCD_NATIVE_H : LCD_NATIVE_W;
	s_h    = landscape ? LCD_NATIVE_W : LCD_NATIVE_H;
	s_base = s_xform[phys].base;
	s_sx   = s_xform[phys].sx;
	s_sy   = s_xform[phys].sy;
	return ESP_OK;					/* buffers and panel untouched */
}

uint8_t lcdGetRotation(void)
{
	return s_rot;
}

esp_err_t lcdBacklight(uint8_t pct)
{
	if (pct > 100) {
		return ESP_ERR_INVALID_ARG;
	}
	uint32_t duty = (uint32_t)pct * LCD_BL_MAX / 100;
	ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_MODE, LCD_BL_CHANNEL, duty), TAG, "bl duty");
	s_blPct = pct;
	return ledc_update_duty(LCD_BL_MODE, LCD_BL_CHANNEL);
}

esp_err_t lcdSetViewFlip(bool on)
{
	if (on == s_vflip) {
		return ESP_OK;
	}
	s_vflip = on;
	ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, !s_vflip, s_vflip), TAG, "mirror");
	return lcdResend();				/* panel RAM is not re-mapped by itself */
}

bool lcdGetViewFlip(void)
{
	return s_vflip;
}

uint8_t lcdGetBacklight(void)
{
	return s_blPct;
}

void lcdDeepSleepPrep(void)
{
	ledc_stop(LCD_BL_MODE, LCD_BL_CHANNEL, 0);
	gpio_reset_pin(LCD_PIN_BL);
	gpio_set_direction(LCD_PIN_BL, GPIO_MODE_OUTPUT);
	gpio_set_level(LCD_PIN_BL, 0);
	gpio_hold_en(LCD_PIN_BL);			/* stays off through deep sleep */
	gpio_deep_sleep_hold_en();
}

uint16_t lcdRgb(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

esp_err_t lcdFill(uint16_t colour)
{
	return lcdFillRect(0, 0, s_w, s_h, colour);
}

esp_err_t lcdFillRect(int x, int y, int w, int h, uint16_t colour)
{
	/* clip */
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > s_w) { w = s_w - x; }
	if (y + h > s_h) { h = s_h - y; }
	if (w <= 0 || h <= 0) {
		return ESP_OK;
	}

	uint16_t *back = LCD_BACK;
	for (int row = y; row < y + h; row++) {
		uint16_t *p = back + s_base + x * s_sx + row * s_sy;
		for (int i = 0; i < w; i++, p += s_sx) {
			*p = colour;
		}
	}
	lcdMarkDirty(x, y, w, h);
	return ESP_OK;
}

esp_err_t lcdDrawPixels(int x, int y, int w, int h, const uint16_t *pixels)
{
	int stride = w;

	/* clip, moving the source pointer to match */
	if (x < 0) { pixels -= x;          w += x; x = 0; }
	if (y < 0) { pixels -= y * stride; h += y; y = 0; }
	if (x + w > s_w) { w = s_w - x; }
	if (y + h > s_h) { h = s_h - y; }
	if (w <= 0 || h <= 0) {
		return ESP_OK;
	}

	uint16_t *back = LCD_BACK;
	for (int row = 0; row < h; row++) {
		uint16_t       *d   = back + s_base + x * s_sx + (y + row) * s_sy;
		const uint16_t *src = pixels + row * stride;
		if (s_sx == 1) {
			memcpy(d, src, w * sizeof(uint16_t));
		} else {
			for (int i = 0; i < w; i++, d += s_sx) {
				*d = src[i];
			}
		}
	}
	lcdMarkDirty(x, y, w, h);
	return ESP_OK;
}

/* 16-step blend between bg (alpha 0) and fg (alpha 15). */
static void lcdBlendPalette(uint16_t fg, uint16_t bg, uint16_t pal[16])
{
	int fr = fg >> 11, fgn = (fg >> 5) & 0x3F, fb = fg & 0x1F;
	int br = bg >> 11, bgn = (bg >> 5) & 0x3F, bb = bg & 0x1F;
	for (int a = 0; a < 16; a++) {
		int r = br + (fr - br) * a / 15;
		int g = bgn + (fgn - bgn) * a / 15;
		int b = bb + (fb - bb) * a / 15;
		pal[a] = (uint16_t)((r << 11) | (g << 5) | b);
	}
}

/* Draw one glyph with a ready-made palette; skipped if not fully on screen. */
static int lcdPutGlyph(const font_t *f, int x, int y, char c, const uint16_t pal[16])
{
	int            w;
	const uint8_t *glyph = fontGlyph(f, c, &w);

	if (x < 0 || y < 0 || x + w > s_w || y + f->height > s_h) {
		return w;					/* off-screen: skip */
	}
	uint16_t *back = LCD_BACK;
	for (int py = 0; py < f->height; py++) {
		uint16_t *d = back + s_base + x * s_sx + (y + py) * s_sy;
		for (int px = 0; px < w; px++, d += s_sx) {
			*d = pal[fontAlpha(glyph, w, px, py)];
		}
	}
	lcdMarkDirty(x, y, w, f->height);
	return w;
}

esp_err_t lcdDrawChar(const font_t *f, int x, int y, char c, uint16_t fg, uint16_t bg)
{
	uint16_t pal[16];
	if (!f) {
		f = fontGet(FONT_DEFAULT);
	}
	lcdBlendPalette(fg, bg, pal);
	lcdPutGlyph(f, x, y, c, pal);
	return ESP_OK;
}

esp_err_t lcdDrawText(const font_t *f, int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
	uint16_t pal[16];
	if (!f) {
		f = fontGet(FONT_DEFAULT);
	}
	lcdBlendPalette(fg, bg, pal);

	int cx = x;
	for (const char *p = str; *p; p++) {
		if (*p == '\n') {
			cx = x;
			y += f->height;
			continue;
		}
		cx += lcdPutGlyph(f, cx, y, *p, pal);
	}
	return ESP_OK;
}

int lcdTextCols(void)
{
	return (s_w - LCD_TEXT_MARGIN) / fontGet(FONT_DEFAULT)->maxWidth;
}

int lcdTextRows(void)
{
	return (s_h - LCD_TEXT_MARGIN) / fontGet(FONT_DEFAULT)->height;
}

esp_err_t lcdPrintAt(int col, int row, int fieldChars, const char *text)
{
	if (col < 0 || row < 0 || fieldChars < 1 ||
	    col + fieldChars > lcdTextCols() || row >= lcdTextRows()) {
		return ESP_ERR_INVALID_SIZE;
	}

	const font_t *f = fontGet(FONT_DEFAULT);
	int x = LCD_TEXT_MARGIN + col * f->maxWidth;
	int y = LCD_TEXT_MARGIN + row * f->height;

	char buf[64];
	int  n = (fieldChars < (int)sizeof(buf) - 1) ? fieldChars : (int)sizeof(buf) - 1;
	snprintf(buf, n + 1, "%s", text);	/* truncate to field */

	uint16_t fg, bg;
	lcdTextColours(&fg, &bg);

	ESP_RETURN_ON_ERROR(lcdFillRect(x, y, fieldChars * f->maxWidth, f->height, bg), TAG, "clear field");
	return lcdDrawText(f, x, y, buf, fg, bg);
}

esp_err_t lcdScrollUp(int x, int y, int w, int h, int dy, uint16_t fill)
{
	/* clip to the screen */
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > s_w) { w = s_w - x; }
	if (y + h > s_h) { h = s_h - y; }
	if (w <= 0 || h <= 0 || dy <= 0) {
		return ESP_OK;
	}
	if (dy >= h) {
		return lcdFillRect(x, y, w, h, fill);
	}

	uint16_t *back = LCD_BACK;
	for (int row = y; row < y + h - dy; row++) {
		uint16_t       *d = back + s_base + x * s_sx + row * s_sy;
		const uint16_t *s = back + s_base + x * s_sx + (row + dy) * s_sy;
		for (int i = 0; i < w; i++, d += s_sx, s += s_sx) {
			*d = *s;
		}
	}
	lcdMarkDirty(x, y, w, h - dy);
	return lcdFillRect(x, y + h - dy, w, dy, fill);
}

void lcdGetTextColours(uint16_t *fg, uint16_t *bg)
{
	lcdTextColours(fg, bg);
}

esp_err_t lcdUpdate(void)
{
	return lcdUpdateEx(true);
}

esp_err_t lcdUpdateNoTr(void)
{
	return lcdUpdateEx(false);
}

/* allowTr: run the selected screen transition (scrtr) instead of the diff update. */
static esp_err_t lcdUpdateEx(bool allowTr)
{
	if (!s_dirty) {
		return ESP_OK;
	}

	if (allowTr && !s_forceFull && lcdChanged()) {
		scrtrEnv_t env = {
			.from  = LCD_FRONT,
			.to    = LCD_BACK,
			.buf   = s_buf,
			.bufPx = LCD_BUF_PX,
			.w     = LCD_NW,
			.h     = LCD_NH,
			.x0    = s_dx0,
			.y0    = s_dy0,
			.x1    = s_dx1,
			.y1    = s_dy1,
			.sx    = s_sx,
			.sy    = s_sy,
			.blit  = lcdBlit,
		};
		esp_err_t err = scrtrRun(&env);
		if (err == ESP_OK) {
			lcdSyncSwap();
			return ESP_OK;
		}
		if (err != ESP_ERR_NOT_FOUND) {
			return err;
		}
	}

	ESP_RETURN_ON_ERROR(lcdSendDiff(), TAG, "diff");
	lcdSyncSwap();
	return ESP_OK;
}

/* True if any pixel in the dirty rectangle differs from what is on the panel. */
static bool lcdChanged(void)
{
	const uint16_t *back  = LCD_BACK;
	const uint16_t *front = LCD_FRONT;
	int             n     = (s_dx1 - s_dx0) * sizeof(uint16_t);

	for (int y = s_dy0; y < s_dy1; y++) {
		if (memcmp(back + y * LCD_NW + s_dx0, front + y * LCD_NW + s_dx0, n) != 0) {
			return true;
		}
	}
	return false;
}

/* Send the changed pixels of the dirty rectangle (all of it if s_forceFull). */
static esp_err_t lcdSendDiff(void)
{
	const uint16_t *back  = LCD_BACK;
	const uint16_t *front = LCD_FRONT;
	int bandY0 = -1, bandX0 = 0, bandX1 = 0;

	/* Find changed pixels row by row; group consecutive changed rows into bands
	 * that fit the DMA staging buffer. */
	for (int y = s_dy0; y <= s_dy1; y++) {
		int x0 = -1, x1 = -1;			/* changed span on this row, x1 exclusive */

		if (y < s_dy1) {
			const uint16_t *b = back + y * LCD_NW;
			const uint16_t *f = front + y * LCD_NW;
			if (s_forceFull) {
				x0 = s_dx0;
				x1 = s_dx1;
			} else {
				for (int x = s_dx0; x < s_dx1; x++) {
					if (b[x] != f[x]) {
						x0 = x;
						break;
					}
				}
				if (x0 >= 0) {
					for (int x = s_dx1 - 1; x >= x0; x--) {
						if (b[x] != f[x]) {
							x1 = x + 1;
							break;
						}
					}
				}
			}
		}

		if (bandY0 >= 0) {
			int nx0 = (x0 >= 0 && x0 < bandX0) ? x0 : bandX0;
			int nx1 = (x1 > bandX1) ? x1 : bandX1;
			bool fits = (nx1 - nx0) * (y - bandY0 + 1) <= LCD_BUF_PX;
			if (x0 < 0 || !fits) {			/* row unchanged, end of area, or band full */
				ESP_RETURN_ON_ERROR(lcdSendBand(bandX0, bandX1, bandY0, y), TAG, "band");
				bandY0 = -1;
			} else {
				bandX0 = nx0;
				bandX1 = nx1;
				continue;
			}
		}
		if (x0 >= 0) {
			bandY0 = y;
			bandX0 = x0;
			bandX1 = x1;
		}
	}
	return ESP_OK;
}

/* Bring the other buffer up to date so drawing continues on current content, then swap. */
static void lcdSyncSwap(void)
{
	const uint16_t *back = LCD_BACK;
	uint16_t       *dst  = LCD_FRONT;

	for (int y = s_dy0; y < s_dy1; y++) {
		memcpy(dst + y * LCD_NW + s_dx0, back + y * LCD_NW + s_dx0, (s_dx1 - s_dx0) * sizeof(uint16_t));
	}
	s_front ^= 1;

	s_dirty     = false;
	s_forceFull = false;
}

void lcdReadRow(int y, uint16_t *out)
{
	const uint16_t *f   = LCD_FRONT;
	int             idx = s_base + y * s_sy;

	for (int x = 0; x < s_w; x++, idx += s_sx) {
		out[x] = f[idx];
	}
}

esp_err_t lcdCls(uint8_t r, uint8_t g, uint8_t b)
{
	s_bgR = r;
	s_bgG = g;
	s_bgB = b;
	return lcdFill(lcdRgb(r, g, b));
}

/* Send the whole frame that is on the panel (front buffer) again. */
static esp_err_t lcdResend(void)
{
	const uint16_t *front = LCD_FRONT;
	int             rows  = LCD_BUF_PX / LCD_NW;

	for (int y = 0; y < LCD_NH; y += rows) {
		int n = (LCD_NH - y < rows) ? LCD_NH - y : rows;
		memcpy(s_buf, front + y * LCD_NW, n * LCD_NW * sizeof(uint16_t));
		ESP_RETURN_ON_ERROR(lcdBlit(0, y, LCD_NW, n), TAG, "resend");
	}
	return ESP_OK;
}

void lcdSetAutoUpdate(bool on)
{
	s_auto = on;
}

bool lcdGetAutoUpdate(void)
{
	return s_auto;
}

/* ================= Internals ================= */

static bool lcdOnTxDone(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
	BaseType_t woken = pdFALSE;
	xSemaphoreGiveFromISR(s_txDone, &woken);
	return woken == pdTRUE;
}

/* Send s_buf to (x,y,w,h) and block until DMA is done, so s_buf can be reused. */
static esp_err_t lcdBlit(int x, int y, int w, int h)
{
	esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, s_buf);
	if (err == ESP_OK) {
		xSemaphoreTake(s_txDone, portMAX_DELAY);
	}
	return err;
}

/* Copy back-buffer rows y0..y1-1, columns x0..x1-1 into the DMA buffer and send. */
static esp_err_t lcdSendBand(int x0, int x1, int y0, int y1)
{
	const uint16_t *back = LCD_BACK;
	int             w    = x1 - x0;

	for (int y = y0; y < y1; y++) {
		memcpy(s_buf + (y - y0) * w, back + y * LCD_NW + x0, w * sizeof(uint16_t));
	}
	return lcdBlit(x0, y0, w, y1 - y0);
}

/* Grow the native dirty rectangle by logical rect (x,y,w,h), already clipped. */
static void lcdMarkDirty(int lx, int ly, int lw, int lh)
{
	int a  = s_base + lx * s_sx + ly * s_sy;			/* native index of two corners */
	int b  = s_base + (lx + lw - 1) * s_sx + (ly + lh - 1) * s_sy;
	int ax = a % LCD_NW, ay = a / LCD_NW;
	int bx = b % LCD_NW, by = b / LCD_NW;
	int x  = (ax < bx) ? ax : bx;
	int y  = (ay < by) ? ay : by;
	int w  = ((ax > bx) ? ax : bx) - x + 1;
	int h  = ((ay > by) ? ay : by) - y + 1;

	if (!s_dirty) {
		s_dx0 = x;
		s_dy0 = y;
		s_dx1 = x + w;
		s_dy1 = y + h;
		s_dirty = true;
		return;
	}
	if (x < s_dx0)     s_dx0 = x;
	if (y < s_dy0)     s_dy0 = y;
	if (x + w > s_dx1) s_dx1 = x + w;
	if (y + h > s_dy1) s_dy1 = y + h;
}

/* Fixed native portrait orientation (matches the Waveshare demo). Set once.
 * The view flip mirrors both axes instead (= 180 degrees); the 34 px gap is
 * the same on both sides of the 240-wide panel RAM, so it still fits. */
static esp_err_t lcdPanelOrient(void)
{
	ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "swap");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, !s_vflip, s_vflip), TAG, "mirror");
	return esp_lcd_panel_set_gap(s_panel, LCD_GAP, 0);
}

/* 10x10 square, random place and bright random colour: shows the board just booted. */
void lcdBootMarker(void)
{
	uint8_t r, g, b;
	uint8_t rot = s_rot;
	do {
		uint32_t rnd = esp_random();
		r = rnd;
		g = rnd >> 8;
		b = rnd >> 16;
	} while ((r * 299u + g * 587u + b * 114u) / 1000u < 128);	/* luma >= ~50% */

	lcdSetRotation(0);				/* bottom edge of the glass */
	int x = esp_random() % (s_w - 10 + 1);
	lcdFillRect(x, LCD_NATIVE_H - 10, 10, 10, lcdRgb(r, g, b));
	lcdSetRotation(rot);
}

/* Runs after every command (cmd post-hook). */
static void lcdAfterCommand(void)
{
	if (s_auto) {
		lcdUpdate();
	}
}

static esp_err_t lcdBacklightInit(void)
{
	gpio_hold_dis(LCD_PIN_BL);			/* held off by IMU SLEEP CPU */
	gpio_deep_sleep_hold_dis();
	ledc_timer_config_t timer = {
		.speed_mode      = LCD_BL_MODE,
		.duty_resolution = LCD_BL_RES,
		.timer_num       = LCD_BL_TIMER,
		.freq_hz         = LCD_BL_FREQ_HZ,
		.clk_cfg         = LEDC_AUTO_CLK,
	};
	ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

	ledc_channel_config_t chan = {
		.gpio_num   = LCD_PIN_BL,
		.speed_mode = LCD_BL_MODE,
		.channel    = LCD_BL_CHANNEL,
		.timer_sel  = LCD_BL_TIMER,
		.duty       = 0,		/* off until panel is ready */
		.hpoint     = 0,
	};
	return ledc_channel_config(&chan);
}

/* Background = last CLS colour; text = black or white, whichever contrasts. */
static void lcdTextColours(uint16_t *fg, uint16_t *bg)
{
	uint32_t luma = (s_bgR * 299u + s_bgG * 587u + s_bgB * 114u) / 1000u;
	*bg = lcdRgb(s_bgR, s_bgG, s_bgB);
	*fg = (luma > 128) ? lcdRgb(0, 0, 0) : lcdRgb(255, 255, 255);
}

/* Clear to last CLS colour, draw dimensions top-left, echo them to the client. */
static esp_err_t lcdShowScreen(cmdReply_t reply)
{
	uint16_t fg, bg;
	lcdTextColours(&fg, &bg);

	char text[32];
	snprintf(text, sizeof(text), "W:%u\nH:%u\n", s_w, s_h);

	ESP_RETURN_ON_ERROR(lcdFill(bg), TAG, "fill");
//	ESP_RETURN_ON_ERROR(lcdDrawText(NULL, LCD_TEXT_MARGIN, LCD_TEXT_MARGIN, text, fg, bg), TAG, "text");
	if (reply) {
		reply(text);
	}
	return ESP_OK;
}

/* ================= Commands ================= */

static const char *lcdCmdCls(int argc, char *argv[], cmdReply_t reply)
{
	uint8_t r, g, b;
	if (!cmdParseU8(argv[0], &r) || !cmdParseU8(argv[1], &g) || !cmdParseU8(argv[2], &b)) {
		return "values must be 0..255";
	}
	s_bgR = r;
	s_bgG = g;
	s_bgB = b;
	return (lcdShowScreen(reply) == ESP_OK) ? NULL : "LCD write failed";
}

/* SCR FLIP [P|L|PL|NONE]: report, or store in NV, apply and re-send the screen. */
static const char *lcdCmdFlip(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 0) {
		reply(s_pflip ? (s_lflip ? "PL\n" : "P\n") : (s_lflip ? "L\n" : "NONE\n"));
		return NULL;
	}
	bool p, l;
	if (strcasecmp(argv[0], "NONE") == 0)    { p = false; l = false; }
	else if (strcasecmp(argv[0], "P") == 0)  { p = true;  l = false; }
	else if (strcasecmp(argv[0], "L") == 0)  { p = false; l = true;  }
	else if (strcasecmp(argv[0], "PL") == 0 || strcasecmp(argv[0], "LP") == 0) { p = true; l = true; }
	else {
		return "usage: SCR FLIP [P|L|PL|NONE]";
	}

	const char *keys[] = { "scr.pflip", "scr.lflip" };
	const char *vals[] = { p ? "1" : "0", l ? "1" : "0" };
	const char *err = nvSetMany(2, keys, vals, NULL);
	if (err) {
		return err;
	}
	s_pflip = p;
	s_lflip = l;
	lcdSetRotation(s_rot);				/* re-map the current rotation */
	return (lcdResend() == ESP_OK) ? NULL : "LCD write failed";
}

static const char *lcdCmdRot(int argc, char *argv[], cmdReply_t reply)
{
	uint8_t rot;
	if (!cmdParseU8(argv[0], &rot)) {
		return "value must be 0..255 (used modulo 4)";
	}
	if (lcdSetRotation(rot % 4) != ESP_OK) {
		return "rotation failed";
	}

	char text[32];
	snprintf(text, sizeof(text), "W:%u\nH:%u\n", s_w, s_h);
	reply(text);					/* screen is left as it is */
	return NULL;
}

static const char *lcdCmdBl(int argc, char *argv[], cmdReply_t reply)
{
	uint8_t pct;
	if (!cmdParseU8(argv[0], &pct) || pct > 100) {
		return "value must be 0..100";
	}
	return (lcdBacklight(pct) == ESP_OK) ? NULL : "backlight failed";
}

static const char *lcdCmdScr(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 1 && strcasecmp(argv[0], "UPD") == 0) {
		return (lcdUpdate() == ESP_OK) ? NULL : "LCD write failed";
	}
	if (argc == 2 && strcasecmp(argv[0], "AUTO") == 0) {
		uint8_t on;
		if (!cmdParseU8(argv[1], &on) || on > 1) {
			return "value must be 0|1";
		}
		lcdSetAutoUpdate(on);
		return NULL;
	}
	if (strcasecmp(argv[0], "FLIP") == 0 && argc <= 2) {
		return lcdCmdFlip(argc - 1, argv + 1, reply);
	}
	if (strcasecmp(argv[0], "SHOT") == 0) {
		return imgShotCmd(argc - 1, argv + 1, reply);
	}
	if (argc >= 1 && strcasecmp(argv[0], "TR") == 0) {
		return scrtrCommand(false, argc - 1, argv + 1, reply);
	}
	if (argc >= 1 && strcasecmp(argv[0], "TRP") == 0) {
		return scrtrCommand(true, argc - 1, argv + 1, reply);
	}
	return "usage: SCR AUTO 0|1 | UPD | SHOT [name] [-f] | TR|TRP [transition] | FLIP [P|L|PL|NONE]";
}
