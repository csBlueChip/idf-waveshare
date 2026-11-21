#pragma once

/*
 * ANSI colour for PRINT / DEBUG output (plain C: also built into the PC
 * script tester).
 *
 * Text escapes (after the backslash); a colour is a digit or a letter
 * (lower case only): 0/k black 1/r red 2/g green 3/y yellow 4/b blue
 * 5/m magenta 6/c cyan 7/w white 8/d default; 9/l before 0..7 = light:
 *   iN   ink N        i9N / ilN  light (bright) ink N
 *   pN   paper N      p9N / plN  light paper N
 *   i8 / id   ink back to the script's INK (SGR 39 when it is 8 / unset)
 *   p8 / pd   paper back to the script's PAPER (SGR 49 when 8 / unset)
 *   ip   both
 * They become SGR codes (ESC [ n m); ansiFrame() then finishes each line.
 *
 * Colour values (INK / PAPER, ansiFrame): 0..7, 90..97 (bright), 8 = none.
 */

#include <stdbool.h>
#include <stddef.h>

#define ANSI_NONE	8		/* no INK / PAPER set */
#define ANSI_ESC	'\x1b'
#define ANSI_SEQ_MAX	16		/* one escape's SGR output, with NUL */

/* p points just after a backslash at 'i' or 'p'. Writes the SGR codes to out
 * (ANSI_SEQ_MAX bytes) and returns the characters used from p (2 or 3), or 0
 * if p is not a colour escape at all (not 'i' / 'p'), or -1 if it is a bad
 * one (e.g. \ix, \i9 without a colour). */
int ansiEscape(const char *p, char *out);

/* INK / PAPER argument: a colour (0..8 / k r g y b m c w d) or 9|l + 0..7 /
 * k..w -> *c (0..8, 90..97). false = bad. */
bool ansiParseColour(const char *w, int *c);

/* Finish PRINT / DEBUG text line by line (lines end at '\n'): a line that
 * uses colour (an escape, or ink / paper set) starts with the ink / paper
 * codes, has SGR 39 / 49 (the \i8 \p8 escapes) turned into the ink / paper,
 * and ends with the ink / paper again, ESC[K (paper set: the rest of the line
 * in the paper colour), ESC[0m. Writes at most n bytes with a NUL (n 0 =
 * measure only); returns the full length (without the NUL). */
size_t ansiFrame(const char *in, int ink, int paper, char *out, size_t n);

/* Remove every ESC [ ... <letter> sequence from s (in place). Returns the
 * new length. */
size_t ansiStrip(char *s);

/* s holds an ESC character. */
bool ansiHas(const char *s);
