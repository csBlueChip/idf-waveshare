#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Text boxes 1..TEXT_BOXES (32) on the LCD. Each box keeps the screen rotation
 * that was active when it was created (TBOX), so a box made after ROT 1 always
 * prints sideways, whatever the current rotation. Cursor is internal (not shown).
 *
 * Commands registered by textInit():
 *   TBOX n x y w lines [speed] [font] [lead]
 *        define box n: x,y,w in pixels; height = lines*font height + (lines-1)*lead.
 *        speed: typing effect, ms per character (0..1000, default 0 = off);
 *        font default 1 (0 = 1); lead defaults to the font's own.
 *        Cursor 0,0, nothing drawn, ink and paper AUTO, scroll off.
 *   TBOX n            report: n:1 rot:0 x:10 y:10 w:150 lines:2 speed:0 font:1
 *                     lead:2 h:50 cx:0 cy:1 scroll:0 ink:auto paper:auto
 *                     (cx pixels; cy = the cursor's line from 1, rounded down)
 *                     (':' splits words in scripts: GET $t = TBOX 1 -> $t[2k]
 *                     values; colours auto | none | RRGGBB widened from 565)
 *   TBOX n SET x|y|w|l|s|d|f value
 *        change one property, no redraw. x,y,w pixels; l lines (recomputes the
 *        height); s speed; d leading and f font (0 = 1) do NOT resize the box.
 *   TSCRL n 0|1       auto-scroll when text runs past the bottom (default 0)
 *   TYX n line x      move cursor to line 1..lines (top of the line at
 *                     (line-1) * (font height + lead)), x pixels from the left
 *   TEXT n text       print at the cursor; spaces kept. Text rules: see textUnquote().
 *                     Escapes: \n new line, \\ backslash, \# hash, \$ dollar,
 *                     \1..\9 font (this TEXT only)
 *   TLEN n text       reply the width in pixels TEXT n text would take on an
 *                     endless line (widest line if \n); nothing drawn
 *   TINK n colour|AUTO|NONE    text colour. AUTO (default): black/white
 *                     contrasting with the CLS colour. NONE: the glyph shows
 *                     what is under it (paper drawn around it)
 *   TPAPER n colour|AUTO|NONE  background. AUTO (default): the CLS colour.
 *                     NONE: only the glyph is drawn, blended onto what is
 *                     there. Ink and paper both NONE: nothing drawn. No redraw.
 *   TCLS n [colour|AUTO]  set the paper (if given), fill the box with it,
 *                     cursor 0,0. Paper NONE: filled with the AUTO colour
 *                     (paper stays NONE). TCLS n NONE is an error.
 *   colour = r,g,b, RRGGBB or @h,s,l (cmdParseColour)
 *
 * Typing effect (speed > 0): after each drawn character (including spaces) the
 * screen is updated and the next waits 'speed' ms (FreeRTOS tick resolution);
 * new lines and discarded characters are not delayed. TEXT replies when done.
 * SCR AUTO 0 disables the effect (box prints instantly).
 *
 * The box must be at least as wide as its font's widest character.
 * Word wrap: a word that doesn't fit the rest of the line moves to the next
 * line (unless already at line start); a word wider than the box breaks by
 * character. Past the bottom: scroll on moves the box content up just enough;
 * scroll off discards the rest until the cursor is moved (TYX/TCLS) or
 * scrolling is switched on. Scrolling fills the new line with the paper (the
 * AUTO colour when paper is NONE).
 */

#define TEXT_BOXES	32	/* text boxes 1..TEXT_BOXES */
#define TEXT_LEAD_MAX	100
#define TEXT_LINES_MAX	100
#define TEXT_SPEED_MAX	1000

/* Register text commands. Call after cmdInit() and lcdInit(). */
esp_err_t textInit(void);

/*
 * Text argument rules shared by TEXT, TLEN, PRINT, DEBUG, FLOG and FWRITE.
 * textUnquote(): if the first non-space character is " or ', the text is the
 * quoted string up to the matching quote (\" or \' inside = that quote; only
 * blanks may follow the closing quote). A leading \" or \' = not quoted, starts
 * with a quote (the backslash is dropped). Otherwise the text is kept as is,
 * leading spaces included. Other escapes are left for the next step.
 * Returns NULL or an error message. out: at least strlen(in) + 1 bytes.
 */
const char *textUnquote(const char *in, char *out, size_t n);

/* Escapes for PRINT / DEBUG / FLOG: \n new line, \\ \# \$ literal,
 * \1..\9 dropped, any other \x kept as is. out may be the same buffer as in. */
void textUnescape(const char *in, char *out, size_t n);
