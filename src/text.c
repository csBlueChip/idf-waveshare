#include "text.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"

#include "cmd.h"
#include "font.h"
#include "lcd.h"

static const char *TAG = "text";

#define TEXT_COORD_MAX	1000

typedef struct {
	bool     used;
	uint8_t  rot;		/* rotation captured at TBOX */
	int      x, y, w;	/* pixels, in that rotation */
	int      lines;		/* height in lines (as last set) */
	int      h;		/* height in pixels */
	int      lead;
	int      speed;		/* typing delay per character, ms (0 = off) */
	int      font;		/* base font id */
	int      cx, cy;	/* cursor, relative to box */
	bool     scroll;
	bool     full;		/* scroll off and a new line didn't fit: discard until cursor moves */
	bool     inkSet, paperSet;
	uint16_t ink, paper;
} textBox_t;

/* One character to print, with the font it uses */
typedef struct {
	char          c;
	const font_t *f;
} textChar_t;

/* Drawing context for one ECHO */
typedef struct {
	textBox_t *b;
	int        lineH;	/* base font height + lead */
	int        fontH;	/* base font height */
	uint16_t   ink, paper;
} textCtx_t;

static EXT_RAM_BSS_ATTR textBox_t s_box[TEXT_BOXES + 1];	/* index 1..9 */
static uint8_t   s_savedRot;
static char      s_err[40];			/* for formatted error replies */

static textBox_t  *textGet(int n);
static void        textEnter(const textBox_t *b);
static void        textLeave(void);
static void        textColours(const textBox_t *b, uint16_t *ink, uint16_t *paper);
static int         textHeight(int lines, int lead, const font_t *f);
static void        textClampCursor(textBox_t *b);
static void        textLineBreak(textCtx_t *t);
static esp_err_t   textPut(textCtx_t *t, const textChar_t *ch);
static esp_err_t   textPrint(textBox_t *b, const textChar_t *chars, int n);
static bool        textParseRgb(char *argv[], uint16_t *colour);
static const char *textCmdBox(int argc, char *argv[], cmdReply_t reply);
static const char *textCmdSet(textBox_t *b, const char *key, const char *val);
static const char *textCmdScrl(int argc, char *argv[], cmdReply_t reply);
static const char *textCmdYx(int argc, char *argv[], cmdReply_t reply);
static const char *textCmdInk(int argc, char *argv[], cmdReply_t reply);
static const char *textCmdCls(int argc, char *argv[], cmdReply_t reply);
static const char *textCmdEcho(const char *args, cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t textInit(void)
{
	ESP_RETURN_ON_ERROR(cmdRegister("TBOX", 4, 8, textCmdBox,
	                    "TBOX n x y w lines [speed] [lead] [font] | TBOX n SET x|y|w|h|s|l|f v"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("TSCRL", 2, 2, textCmdScrl, "TSCRL n 0|1  auto-scroll"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("TYX", 3, 3, textCmdYx, "TYX n y x  move cursor (pixels)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegisterRaw("ECHO", textCmdEcho, "ECHO n text  {\\n, \\\\, \\#, \\$, \\1..9=font}"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("TINK", 4, 4, textCmdInk, "TINK n r g b  text colour"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("TCLS", 4, 4, textCmdCls, "TCLS n r g b  clear box"), TAG, "cmd");
	return ESP_OK;
}

/* ================= Internals ================= */

static textBox_t *textGet(int n)
{
	return (n >= 1 && n <= TEXT_BOXES && s_box[n].used) ? &s_box[n] : NULL;
}

/* Draw in the box's own rotation; restore afterwards. */
static void textEnter(const textBox_t *b)
{
	s_savedRot = lcdGetRotation();
	lcdSetRotation(b->rot);
}

static void textLeave(void)
{
	lcdSetRotation(s_savedRot);
}

/* Box colours, falling back to the CLS defaults. */
static void textColours(const textBox_t *b, uint16_t *ink, uint16_t *paper)
{
	uint16_t fg, bg;
	lcdGetTextColours(&fg, &bg);
	*ink   = b->inkSet   ? b->ink   : fg;
	*paper = b->paperSet ? b->paper : bg;
}

static int textHeight(int lines, int lead, const font_t *f)
{
	return lines * f->height + (lines - 1) * lead;
}

static void textClampCursor(textBox_t *b)
{
	if (b->cx > b->w - 1) b->cx = b->w - 1;
	if (b->cy > b->h - 1) b->cy = b->h - 1;
}

/* Explicit or wrap new line. */
static void textLineBreak(textCtx_t *t)
{
	textBox_t *b = t->b;

	if (b->full) {
		if (b->scroll) {			/* take the pending new line */
			b->full = false;
			b->cx   = 0;
			b->cy  += t->lineH;
		}
		return;
	}
	if (!b->scroll && b->cy + t->lineH + t->fontH > b->h) {
		b->full = true;				/* won't fit: discard from here */
		return;
	}
	b->cx  = 0;
	b->cy += t->lineH;
}

/* Draw one character at the cursor (no wrapping here). */
static esp_err_t textPut(textCtx_t *t, const textChar_t *ch)
{
	textBox_t *b = t->b;

	if (b->full) {
		if (!b->scroll) {
			return ESP_OK;			/* discard */
		}
		b->full = false;			/* scroll now on: take the pending new line */
		b->cx   = 0;
		b->cy  += t->lineH;
	}

	if (b->cy + t->fontH > b->h) {			/* past the bottom */
		if (!b->scroll) {
			b->full = true;
			return ESP_OK;
		}
		int dy = b->cy + t->fontH - b->h;
		ESP_RETURN_ON_ERROR(lcdScrollUp(b->x, b->y, b->w, b->h, dy, t->paper), TAG, "scroll");
		b->cy -= dy;
	}

	ESP_RETURN_ON_ERROR(lcdDrawChar(ch->f, b->x + b->cx, b->y + b->cy, ch->c, t->ink, t->paper),
	                    TAG, "char");
	b->cx += fontCharWidth(ch->f, ch->c);

	/* typing effect: show each character, then pause. SCR AUTO 0 disables it. */
	if (b->speed > 0 && lcdGetAutoUpdate()) {
		ESP_RETURN_ON_ERROR(lcdUpdateNoTr(), TAG, "update");
		TickType_t ticks = pdMS_TO_TICKS(b->speed);
		vTaskDelay(ticks ? ticks : 1);
	}
	return ESP_OK;
}

/* Word-wrapped printing of pre-parsed characters. */
static esp_err_t textPrint(textBox_t *b, const textChar_t *chars, int n)
{
	textCtx_t t = { .b = b };
	t.fontH = fontGet(b->font)->height;
	t.lineH = t.fontH + b->lead;
	textColours(b, &t.ink, &t.paper);

	esp_err_t err = ESP_OK;
	textEnter(b);

	for (int i = 0; i < n && err == ESP_OK; ) {
		const textChar_t *ch = &chars[i];

		if (ch->c == '\n') {
			textLineBreak(&t);
			i++;
			continue;
		}

		if (ch->c == ' ') {
			/* a space that doesn't fit ends the line and is dropped */
			if (!b->full && b->cx > 0 && b->cx + fontCharWidth(ch->f, ' ') > b->w) {
				textLineBreak(&t);
			} else {
				err = textPut(&t, ch);
			}
			i++;
			continue;
		}

		/* a word: run of non-space characters, measured in their own fonts */
		int end = i, wordW = 0;
		while (end < n && chars[end].c != ' ' && chars[end].c != '\n') {
			wordW += fontCharWidth(chars[end].f, chars[end].c);
			end++;
		}
		if (!b->full && b->cx > 0 && b->cx + wordW > b->w) {
			textLineBreak(&t);			/* word to next line (not at line start) */
		}
		for (; i < end && err == ESP_OK; i++) {		/* longer than the box: break by character */
			if (!b->full && b->cx > 0 && b->cx + fontCharWidth(chars[i].f, chars[i].c) > b->w) {
				textLineBreak(&t);
			}
			err = textPut(&t, &chars[i]);
		}
	}

	textLeave();
	return err;
}

static bool textParseRgb(char *argv[], uint16_t *colour)
{
	uint8_t r, g, b;
	if (!cmdParseU8(argv[0], &r) || !cmdParseU8(argv[1], &g) || !cmdParseU8(argv[2], &b)) {
		return false;
	}
	*colour = lcdRgb(r, g, b);
	return true;
}

/* ================= Commands ================= */

#define TEXT_BOX_ERR	"box must be 1..9"
#define TEXT_UNDEF_ERR	"box not defined (TBOX)"

static const char *textCmdBox(int argc, char *argv[], cmdReply_t reply)
{
	int n;
	if (!cmdParseInt(argv[0], 1, TEXT_BOXES, &n)) {
		return TEXT_BOX_ERR;
	}

	/* TBOX n SET key value */
	if (strcasecmp(argv[1], "SET") == 0) {
		if (argc != 4) {
			return "usage: TBOX n SET x|y|w|h|s|l|f value";
		}
		textBox_t *b = textGet(n);
		return b ? textCmdSet(b, argv[2], argv[3]) : TEXT_UNDEF_ERR;
	}

	/* TBOX n x y w lines [lead] [font] */
	if (argc < 5) {
		return "usage: TBOX n x y w lines [speed] [lead] [font]";
	}
	int x, y, w, lines, lead, speed = 0, font = FONT_DEFAULT;
	if (!cmdParseInt(argv[1], -TEXT_COORD_MAX, TEXT_COORD_MAX, &x) ||
	    !cmdParseInt(argv[2], -TEXT_COORD_MAX, TEXT_COORD_MAX, &y)) {
		return "x and y must be -1000..1000";
	}
	if (argc >= 8 && !cmdParseInt(argv[7], 1, fontCount(), &font)) {
		snprintf(s_err, sizeof(s_err), "font must be 1..%d", fontCount());
		return s_err;
	}
	const font_t *f = fontGet(font);
	if (!cmdParseInt(argv[3], 1, TEXT_COORD_MAX, &w) || w < f->maxWidth) {
		snprintf(s_err, sizeof(s_err), "w must be at least %u for font %d", f->maxWidth, font);
		return s_err;
	}
	if (!cmdParseInt(argv[4], 1, TEXT_LINES_MAX, &lines)) {
		return "lines must be 1..100";
	}
	if (argc >= 6 && !cmdParseInt(argv[5], 0, TEXT_SPEED_MAX, &speed)) {
		return "speed must be 0..1000";
	}
	lead = f->lead;
	if (argc >= 7 && !cmdParseInt(argv[6], 0, TEXT_LEAD_MAX, &lead)) {
		return "lead must be 0..100";
	}

	s_box[n] = (textBox_t) {
		.used = true, .rot = lcdGetRotation(),
		.x = x, .y = y, .w = w,
		.lines = lines, .lead = lead, .speed = speed, .font = font,
		.h = textHeight(lines, lead, f),
	};
	return NULL;
}

static const char *textCmdSet(textBox_t *b, const char *key, const char *val)
{
	int v;
	char k = (key[0] && !key[1]) ? (char)(key[0] | 0x20) : 0;	/* single letter, lower case */

	switch (k) {
	case 'x':
	case 'y':
		if (!cmdParseInt(val, -TEXT_COORD_MAX, TEXT_COORD_MAX, &v)) {
			return "value must be -1000..1000";
		}
		*(k == 'x' ? &b->x : &b->y) = v;
		break;
	case 'w':
		if (!cmdParseInt(val, 1, TEXT_COORD_MAX, &v) || v < fontGet(b->font)->maxWidth) {
			snprintf(s_err, sizeof(s_err), "w must be at least %u for font %d",
			         fontGet(b->font)->maxWidth, b->font);
			return s_err;
		}
		b->w = v;
		break;
	case 'h':
		if (!cmdParseInt(val, 1, TEXT_LINES_MAX, &v)) {
			return "lines must be 1..100";
		}
		b->lines = v;
		b->h     = textHeight(v, b->lead, fontGet(b->font));
		break;
	case 's':
		if (!cmdParseInt(val, 0, TEXT_SPEED_MAX, &v)) {
			return "speed must be 0..1000";
		}
		b->speed = v;
		break;
	case 'l':
		if (!cmdParseInt(val, 0, TEXT_LEAD_MAX, &v)) {
			return "lead must be 0..100";
		}
		b->lead = v;				/* box size unchanged */
		break;
	case 'f':
		if (!cmdParseInt(val, 1, FONT_MAX, &v) || !fontGet(v)) {
			snprintf(s_err, sizeof(s_err), "font %s not available", val);
			return s_err;
		}
		if (fontGet(v)->maxWidth > b->w) {
			snprintf(s_err, sizeof(s_err), "font %d too wide for box", v);
			return s_err;
		}
		b->font = v;				/* box size unchanged */
		break;
	default:
		return "usage: TBOX n SET x|y|w|h|s|l|f value";
	}

	textClampCursor(b);
	return NULL;
}

static const char *textCmdScrl(int argc, char *argv[], cmdReply_t reply)
{
	int n, on;
	if (!cmdParseInt(argv[0], 1, TEXT_BOXES, &n)) {
		return TEXT_BOX_ERR;
	}
	if (!cmdParseInt(argv[1], 0, 1, &on)) {
		return "value must be 0|1";
	}
	textBox_t *b = textGet(n);
	if (!b) {
		return TEXT_UNDEF_ERR;
	}
	b->scroll = on;
	return NULL;
}

static const char *textCmdYx(int argc, char *argv[], cmdReply_t reply)
{
	int n, y, x;
	if (!cmdParseInt(argv[0], 1, TEXT_BOXES, &n)) {
		return TEXT_BOX_ERR;
	}
	textBox_t *b = textGet(n);
	if (!b) {
		return TEXT_UNDEF_ERR;
	}
	if (!cmdParseInt(argv[1], 0, b->h - 1, &y) || !cmdParseInt(argv[2], 0, b->w - 1, &x)) {
		return "y x must be inside the box";
	}
	b->cx   = x;
	b->cy   = y;
	b->full = false;
	return NULL;
}

static const char *textCmdInk(int argc, char *argv[], cmdReply_t reply)
{
	int      n;
	uint16_t c;
	if (!cmdParseInt(argv[0], 1, TEXT_BOXES, &n)) {
		return TEXT_BOX_ERR;
	}
	if (!textParseRgb(&argv[1], &c)) {
		return "colour must be 0..255";
	}
	textBox_t *b = textGet(n);
	if (!b) {
		return TEXT_UNDEF_ERR;
	}
	b->ink    = c;
	b->inkSet = true;
	return NULL;
}

static const char *textCmdCls(int argc, char *argv[], cmdReply_t reply)
{
	int      n;
	uint16_t c;
	if (!cmdParseInt(argv[0], 1, TEXT_BOXES, &n)) {
		return TEXT_BOX_ERR;
	}
	if (!textParseRgb(&argv[1], &c)) {
		return "colour must be 0..255";
	}
	textBox_t *b = textGet(n);
	if (!b) {
		return TEXT_UNDEF_ERR;
	}
	b->paper    = c;
	b->paperSet = true;
	b->cx       = 0;
	b->cy       = 0;
	b->full     = false;

	textEnter(b);
	esp_err_t err = lcdFillRect(b->x, b->y, b->w, b->h, c);
	textLeave();
	return (err == ESP_OK) ? NULL : "LCD write failed";
}

/* ECHO n text: text rules as textUnquote(), then the escapes \n new line,
 * \\ backslash, \# hash, \$ dollar, \1..\9 font for the rest of this ECHO;
 * any other \x is kept literally. A bad font is reported but printing goes on
 * in the current font (formatting lost, not information). */
static const char *textCmdEcho(const char *args, cmdReply_t reply)
{
	while (*args == ' ' || *args == '\t') {
		args++;
	}
	if (args[0] < '1' || args[0] > '9' || (args[1] != ' ' && args[1] != '\t' && args[1] != '\0')) {
		return "usage: ECHO n text  (n 1..9)";
	}
	int        n = args[0] - '0';
	textBox_t *b = textGet(n);
	if (!b) {
		return TEXT_UNDEF_ERR;
	}

	static EXT_RAM_BSS_ATTR char text[CMD_LINE_MAX + 1];
	static EXT_RAM_BSS_ATTR textChar_t chars[CMD_LINE_MAX];
	const char       *err = textUnquote(args[1] ? args + 2 : args + 1, text, sizeof(text));
	if (err) {
		return err;
	}
	int           count = 0;
	const font_t *cur   = fontGet(b->font);

	for (const char *p = text; *p && count < CMD_LINE_MAX; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			chars[count++] = (textChar_t) { '\n', cur };
			p++;
		} else if (p[0] == '\\' && (p[1] == '\\' || p[1] == '#' || p[1] == '$')) {
			chars[count++] = (textChar_t) { p[1], cur };
			p++;
		} else if (p[0] == '\\' && p[1] >= '1' && p[1] <= '9') {
			int           id = p[1] - '0';
			const font_t *f  = fontGet(id);
			if (!f) {
				if (!err) {
					snprintf(s_err, sizeof(s_err), "font %d not available", id);
					err = s_err;
				}
			} else if (f->maxWidth > b->w) {
				if (!err) {
					snprintf(s_err, sizeof(s_err), "font %d too wide for box", id);
					err = s_err;
				}
			} else {
				cur = f;
			}
			p++;
		} else {
			chars[count++] = (textChar_t) { *p, cur };
		}
	}

	if (textPrint(b, chars, count) != ESP_OK) {
		return "LCD write failed";
	}
	return err;
}

/* ================= Text argument rules ================= */

const char *textUnquote(const char *in, char *out, size_t n)
{
	const char *p = in;
	size_t      o = 0;

	if (n == 0) {
		return "text too long";
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '"' || *p == '\'') {
		char q = *p++;
		for (;; p++) {
			if (*p == '\0') {
				return "missing closing quote";
			}
			if (*p == q) {
				break;
			}
			if (o + 2 >= n) {
				return "text too long";
			}
			if (p[0] == '\\' && p[1] == q) {
				out[o++] = q;
				p++;
			} else if (p[0] == '\\' && p[1] != '\0') {
				out[o++] = *p++;		/* keep the pair for the escape step */
				out[o++] = *p;
			} else {
				out[o++] = *p;
			}
		}
		for (p++; *p == ' ' || *p == '\t'; p++) {
		}
		if (*p != '\0') {
			return "text after closing quote";
		}
	} else {
		const char *skip = (p[0] == '\\' && (p[1] == '"' || p[1] == '\'')) ? p : NULL;
		for (p = in; *p; p++) {
			if (p == skip) {
				continue;			/* leading \" or \' */
			}
			if (o + 1 >= n) {
				return "text too long";
			}
			out[o++] = *p;
		}
	}
	out[o] = '\0';
	return NULL;
}

void textUnescape(const char *in, char *out, size_t n)
{
	size_t o = 0;

	if (n == 0) {
		return;
	}
	for (const char *p = in; *p && o + 1 < n; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			out[o++] = '\n';
			p++;
		} else if (p[0] == '\\' && (p[1] == '\\' || p[1] == '#' || p[1] == '$')) {
			out[o++] = p[1];
			p++;
		} else if (p[0] == '\\' && p[1] >= '1' && p[1] <= '9') {
			p++;					/* font: ECHO only */
		} else {
			out[o++] = *p;
		}
	}
	out[o] = '\0';
}
