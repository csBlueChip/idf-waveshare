#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Text boxes 1..9 on the LCD. Each box keeps the screen rotation that was
 * active when it was created (TBOX), so a box made after ROT 1 always prints
 * sideways, whatever the current rotation. Cursor is internal (not shown).
 *
 * Commands registered by textInit():
 *   TBOX n x y w lines [speed] [lead] [font]
 *        define box n: x,y,w in pixels; height = lines*font height + (lines-1)*lead.
 *        speed: typing effect, ms per character (0..1000, default 0 = off);
 *        lead defaults to the font's own, font to 1. Cursor 0,0, nothing drawn.
 *   TBOX n SET x|y|w|h|s|l|f value
 *        change one property, no redraw. x,y,w pixels; h lines (recomputes the
 *        height); s speed; l leading and f font do NOT resize the box.
 *   TSCRL n 0|1       auto-scroll when text runs past the bottom (default 0)
 *   TYX n y x         move cursor to pixel y,x from the box top-left
 *   ECHO n text       print at the cursor; spaces kept. Text rules: see textUnquote().
 *                     Escapes: \n new line, \\ backslash, \# hash, \$ dollar,
 *                     \1..\9 font (this ECHO only)
 *   TINK n r g b      text colour (default: black/white contrast with CLS colour)
 *   TCLS n r g b      set box background, clear box, cursor 0,0 (default: CLS colour)
 *
 * Typing effect (speed > 0): after each drawn character (including spaces) the
 * screen is updated and the next waits 'speed' ms (FreeRTOS tick resolution);
 * new lines and discarded characters are not delayed. ECHO replies when done.
 * SCR AUTO 0 disables the effect (box prints instantly).
 *
 * The box must be at least as wide as its font's widest character.
 * Word wrap: a word that doesn't fit the rest of the line moves to the next
 * line (unless already at line start); a word wider than the box breaks by
 * character. Past the bottom: scroll on moves the box content up just enough;
 * scroll off discards the rest until the cursor is moved (TYX/TCLS) or
 * scrolling is switched on.
 */

#define TEXT_BOXES	9
#define TEXT_LEAD_MAX	100
#define TEXT_LINES_MAX	100
#define TEXT_SPEED_MAX	1000

/* Register text commands. Call after cmdInit() and lcdInit(). */
esp_err_t textInit(void);

/*
 * Text argument rules shared by ECHO, PRINT, DEBUG and APPEND.
 * textUnquote(): if the first non-space character is " or ', the text is the
 * quoted string up to the matching quote (\" or \' inside = that quote; only
 * blanks may follow the closing quote). A leading \" or \' = not quoted, starts
 * with a quote (the backslash is dropped). Otherwise the text is kept as is,
 * leading spaces included. Other escapes are left for the next step.
 * Returns NULL or an error message. out: at least strlen(in) + 1 bytes.
 */
const char *textUnquote(const char *in, char *out, size_t n);

/* Escapes for PRINT / DEBUG / APPEND: \n new line, \\ \# \$ literal,
 * \1..\9 dropped, any other \x kept as is. out may be the same buffer as in. */
void textUnescape(const char *in, char *out, size_t n);
