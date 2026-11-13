#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * PNG images from the SD card folder /images.
 *
 * File name convention (formal constraint):
 *   - extension .png (any case)
 *   - no spaces
 *   - name (without .png) must not be all digits
 *   - case-insensitive: names must not differ only by case
 *   Files breaking the convention are not listed and cannot be loaded.
 *
 * Command registered by imgInit():
 *   IMG                list PNGs as "n name" (alphabetical, no extension)
 *   IMG name|n [x y]   draw image with top-left at x,y (-1000..1000, default 0 0)
 *
 * Only the image's own pixels are drawn; pixels with alpha < 50% are skipped.
 * Off-screen parts are cropped. Interlaced PNGs are not supported.
 */

/* Register IMG. Call after cmdInit(), lcdInit() and sdInit(). */
esp_err_t imgInit(void);

/* Draw a PNG file at x,y.
 *   ESP_ERR_NOT_FOUND      file can't be opened
 *   ESP_ERR_NOT_SUPPORTED  interlaced PNG
 *   ESP_ERR_INVALID_RESPONSE  decode error
 *   ESP_FAIL               LCD write failed */
esp_err_t imgShowPng(const char *path, int x, int y);

/* Same, from a PNG held in memory (e.g. embedded in the firmware). */
esp_err_t imgShowPngMem(const uint8_t *data, size_t len, int x, int y);

/* Width and height from a PNG header in memory. */
esp_err_t imgPngSize(const uint8_t *data, size_t len, int *w, int *h);

/* SCR SHOT [name] [-f]: save the glass (current ROT) as SD:/scrshots/name.png.
 * No name = timestamp (sysTimestamp), e.g. b002A_001523.png (-1, -2 ... if taken). An
 * existing file is an error unless -f. Replies the file name. Called by lcd's
 * SCR command with the arguments after SHOT. */
const char *imgShotCmd(int argc, char *argv[], void (*reply)(const char *text));
