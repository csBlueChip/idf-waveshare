#include "img.h"

#include <setjmp.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "png.h"

#include "cmd.h"
#include "lcd.h"
#include "sd.h"
#include "sys.h"

static const char *TAG = "img";

#define IMG_EXT		".png"
#define IMG_MAX_FILES	64
#define IMG_STRIP_ROWS	16	/* fully opaque rows sent to the LCD in one go */
#define IMG_ALPHA_MIN	128	/* pixels below this alpha are not drawn */
#define IMG_COORD_MAX	1000
#define IMG_DIR		SD_MOUNT_POINT "/images"
#define IMG_SHOT_DIR	SD_MOUNT_POINT "/scrshots"

typedef char imgName_t[SD_NAME_MAX];

/* Decoder buffers, kept in one struct so they survive libpng's longjmp. */
typedef struct {
	uint8_t  *rgba;		/* one decoded row, RGBA8888 */
	uint16_t *strip;	/* IMG_STRIP_ROWS rows of RGB565 */
} imgBufs_t;

/* In-memory PNG source */
typedef struct {
	const uint8_t *data;
	size_t         len;
	size_t         pos;
} imgMem_t;

static void        imgMemRead(png_structp png, png_bytep out, size_t n);
static esp_err_t   imgRun(FILE *f, imgMem_t *mem, int x, int y);
static int         imgBaseLen(const char *fileName);
static int         imgList(imgName_t *names);
static bool        imgNameValid(const char *base);
static esp_err_t   imgDecode(png_structp png, png_infop info, imgBufs_t *b, int x, int y);
static const char *imgCmd(int argc, char *argv[], cmdReply_t reply);
static esp_err_t   imgShotWrite(FILE *f);

/* ================= Public API ================= */

esp_err_t imgInit(void)
{
	return cmdRegister("IMG", 0, 3, imgCmd, "IMG [name|n [x y]]  list / draw PNG");
}

esp_err_t imgShowPng(const char *path, int x, int y)
{
	sdLock();
	FILE *f = fopen(path, "rb");
	if (!f) {
		sdUnlock();
		return ESP_ERR_NOT_FOUND;
	}

	esp_err_t result = imgRun(f, NULL, x, y);
	if (result == ESP_ERR_INVALID_RESPONSE) {
		ESP_LOGW(TAG, "decode error: %s", path);
	}
	fclose(f);
	sdUnlock();
	return result;
}

esp_err_t imgShowPngMem(const uint8_t *data, size_t len, int x, int y)
{
	imgMem_t mem = { .data = data, .len = len, .pos = 0 };
	return imgRun(NULL, &mem, x, y);
}

esp_err_t imgPngSize(const uint8_t *data, size_t len, int *w, int *h)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	/* signature, then IHDR: length(4) "IHDR"(4) width(4, BE) height(4, BE) */
	if (len < 24 || memcmp(data, sig, sizeof(sig)) != 0 || memcmp(data + 12, "IHDR", 4) != 0) {
		return ESP_ERR_INVALID_RESPONSE;
	}
	*w = (int)((data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19]);
	*h = (int)((data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23]);
	return ESP_OK;
}

/* ================= Internals ================= */

/* libpng read callback for an in-memory PNG. */
static void imgMemRead(png_structp png, png_bytep out, size_t n)
{
	imgMem_t *m = png_get_io_ptr(png);
	if (m->pos + n > m->len) {
		png_error(png, "read past end");
	}
	memcpy(out, m->data + m->pos, n);
	m->pos += n;
}

/* Decode from a file (f) or memory (mem) and draw at x,y. */
static esp_err_t imgRun(FILE *f, imgMem_t *mem, int x, int y)
{
	esp_err_t   result = ESP_ERR_INVALID_RESPONSE;
	imgBufs_t   bufs   = { 0 };
	png_structp png    = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop   info   = png ? png_create_info_struct(png) : NULL;

	if (png && info) {
		if (setjmp(png_jmpbuf(png)) == 0) {
			if (f) {
				png_init_io(png, f);
			} else {
				png_set_read_fn(png, mem, imgMemRead);
			}
			result = imgDecode(png, info, &bufs, x, y);
		} else {
			result = ESP_ERR_INVALID_RESPONSE;
		}
	}

	png_destroy_read_struct(png ? &png : NULL, info ? &info : NULL, NULL);
	free(bufs.rgba);
	free(bufs.strip);
	return result;
}

/* Read header, normalise every PNG type to RGBA8888, then stream rows to the LCD.
 * Runs inside the setjmp guard. */
static esp_err_t imgDecode(png_structp png, png_infop info, imgBufs_t *b, int x, int y)
{
	png_read_info(png, info);

	png_uint_32 w, h;
	int depth, type, interlace;
	png_get_IHDR(png, info, &w, &h, &depth, &type, &interlace, NULL, NULL);

	if (interlace != PNG_INTERLACE_NONE) {
		return ESP_ERR_NOT_SUPPORTED;
	}

	bool hasTrns = png_get_valid(png, info, PNG_INFO_tRNS);
	if (type == PNG_COLOR_TYPE_PALETTE) {
		png_set_palette_to_rgb(png);
	}
	if (type == PNG_COLOR_TYPE_GRAY && depth < 8) {
		png_set_expand_gray_1_2_4_to_8(png);
	}
	if (hasTrns) {
		png_set_tRNS_to_alpha(png);
	}
	if (depth == 16) {
		png_set_strip_16(png);
	}
	if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png);
	}
	if (!(type & PNG_COLOR_MASK_ALPHA) && !hasTrns) {
		png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
	}
	png_read_update_info(png, info);

	if (png_get_rowbytes(png, info) != w * 4) {
		return ESP_ERR_INVALID_RESPONSE;
	}

	b->rgba  = malloc(w * 4);
	b->strip = malloc(w * IMG_STRIP_ROWS * sizeof(uint16_t));
	if (!b->rgba || !b->strip) {
		return ESP_ERR_NO_MEM;
	}

	int stripRows  = 0;	/* opaque rows waiting in strip */
	int stripStart = 0;	/* image row of strip[0] */
	int screenH    = lcdHeight();

	for (int r = 0; r < (int)h; r++) {
		if (y + r >= screenH) {
			break;				/* rest is below the screen */
		}
		png_read_row(png, b->rgba, NULL);
		if (y + r < 0) {
			continue;			/* above the screen */
		}

		uint16_t *out    = b->strip + stripRows * w;
		bool      opaque = true;
		for (png_uint_32 i = 0; i < w; i++) {
			const uint8_t *p = b->rgba + i * 4;
			out[i] = lcdRgb(p[0], p[1], p[2]);
			if (p[3] < IMG_ALPHA_MIN) {
				opaque = false;
			}
		}

		if (opaque) {
			if (stripRows == 0) {
				stripStart = r;
			}
			if (++stripRows == IMG_STRIP_ROWS) {
				if (lcdDrawPixels(x, y + stripStart, w, stripRows, b->strip) != ESP_OK) {
					return ESP_FAIL;
				}
				stripRows = 0;
			}
			continue;
		}

		/* row with transparency: flush pending opaque rows, then draw opaque runs */
		if (stripRows) {
			if (lcdDrawPixels(x, y + stripStart, w, stripRows, b->strip) != ESP_OK) {
				return ESP_FAIL;
			}
			memcpy(b->strip, out, w * sizeof(uint16_t));
			out       = b->strip;
			stripRows = 0;
		}
		for (png_uint_32 i = 0; i < w; ) {
			if (b->rgba[i * 4 + 3] < IMG_ALPHA_MIN) {
				i++;
				continue;
			}
			png_uint_32 start = i;
			while (i < w && b->rgba[i * 4 + 3] >= IMG_ALPHA_MIN) {
				i++;
			}
			if (lcdDrawPixels(x + start, y + r, i - start, 1, out + start) != ESP_OK) {
				return ESP_FAIL;
			}
		}
	}

	if (stripRows && lcdDrawPixels(x, y + stripStart, w, stripRows, b->strip) != ESP_OK) {
		return ESP_FAIL;
	}
	return ESP_OK;
}

/* Valid base name: non-empty, no spaces, not all digits. */
static bool imgNameValid(const char *base)
{
	bool allDigits = true;
	if (!*base) {
		return false;
	}
	for (const char *p = base; *p; p++) {
		if (*p == ' ') {
			return false;
		}
		if (*p < '0' || *p > '9') {
			allDigits = false;
		}
	}
	return !allDigits;
}

/* Length of the name without its extension. */
static int imgBaseLen(const char *fileName)
{
	return (int)(strlen(fileName) - strlen(IMG_EXT));
}

/* Sorted list of valid PNG file names (full on-card names). -1 if no card. */
static int imgList(imgName_t *names)
{
	int n = sdListFiles(IMG_DIR, IMG_EXT, names, IMG_MAX_FILES);
	if (n < 0) {
		return -1;
	}

	int kept = 0;
	for (int i = 0; i < n; i++) {
		char base[SD_NAME_MAX];
		snprintf(base, sizeof(base), "%.*s", imgBaseLen(names[i]), names[i]);
		if (imgNameValid(base)) {
			if (kept != i) {
				memcpy(names[kept], names[i], SD_NAME_MAX);
			}
			kept++;
		}
	}
	return kept;
}

/* ================= Commands ================= */

static const char *imgCmd(int argc, char *argv[], cmdReply_t reply)
{
	if (argc == 2) {
		return "usage: IMG [name|n [x y]]";		/* x without y */
	}

	int x = 0, y = 0;
	if (argc == 3 && (!cmdParseInt(argv[1], -IMG_COORD_MAX, IMG_COORD_MAX, &x) ||
	                  !cmdParseInt(argv[2], -IMG_COORD_MAX, IMG_COORD_MAX, &y))) {
		return "x and y must be -1000..1000";
	}

	imgName_t *names = malloc(IMG_MAX_FILES * sizeof(imgName_t));
	if (!names) {
		return "out of memory";
	}

	sdLock();
	const char *err = NULL;
	if (sdEnsureMounted() != ESP_OK) {
		err = "no SD card";
		goto done;
	}

	int n = imgList(names);
	if (n < 0) {
		err = "no SD card";
		goto done;
	}

	/* ---- list ---- */
	if (argc == 0) {
		char line[SD_NAME_MAX + 8];
		for (int i = 0; i < n; i++) {
			snprintf(line, sizeof(line), "%d %.*s\n", i + 1, imgBaseLen(names[i]), names[i]);
			reply(line);
		}
		goto done;
	}

	/* ---- resolve name or number ---- */
	int idx = -1;
	if (cmdParseInt(argv[0], 1, 1000000, &idx)) {
		idx = (idx <= n) ? idx - 1 : -1;
	} else {
		char want[SD_NAME_MAX];
		strlcpy(want, argv[0], sizeof(want));
		size_t len = strlen(want), extLen = strlen(IMG_EXT);
		if (len > extLen && strcasecmp(want + len - extLen, IMG_EXT) == 0) {
			want[len - extLen] = '\0';
		}
		for (int i = 0; i < n; i++) {
			if (imgBaseLen(names[i]) == (int)strlen(want) &&
			    strncasecmp(names[i], want, strlen(want)) == 0) {
				idx = i;
				break;
			}
		}
	}
	if (idx < 0) {
		err = "not found";
		goto done;
	}

	/* ---- draw ---- */
	char path[sizeof(IMG_DIR) + SD_NAME_MAX + 1];
	snprintf(path, sizeof(path), "%s/%s", IMG_DIR, names[idx]);

	switch (imgShowPng(path, x, y)) {
	case ESP_OK:                break;
	case ESP_ERR_NOT_FOUND:     err = "not found"; break;
	case ESP_ERR_NOT_SUPPORTED: err = "interlaced PNG not supported"; break;
	case ESP_ERR_NO_MEM:        err = "out of memory"; break;
	case ESP_FAIL:              err = "LCD write failed"; break;
	default:                    err = "bad PNG"; break;
	}

done:
	sdUnlock();
	free(names);
	return err;
}

/* ================= Screenshot ================= */

const char *imgShotCmd(int argc, char *argv[], cmdReply_t reply)
{
	const char *name  = NULL;
	bool        force = false;

	for (int i = 0; i < argc; i++) {
		if (strcasecmp(argv[i], "-f") == 0) {
			force = true;
		} else if (!name) {
			name = argv[i];
		} else {
			return "usage: SCR SHOT [name] [-f]";
		}
	}

	char file[SD_NAME_MAX];
	if (name) {
		size_t len   = strlen(name);
		bool   hasExt = len >= 4 && strcasecmp(name + len - 4, IMG_EXT) == 0;
		if (len == 0 || strpbrk(name, "/\\:") || len + (hasExt ? 0 : 4) >= sizeof(file)) {
			return "bad file name";
		}
		snprintf(file, sizeof(file), "%s%s", name, hasExt ? "" : IMG_EXT);
	}

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	if (mkdir(IMG_SHOT_DIR, 0777) != 0 && errno != EEXIST) {
		sdUnlock();
		return "cannot create /scrshots";
	}
	if (!name) {
		const char *e = sysTimestampFile(IMG_SHOT_DIR, IMG_EXT, file, sizeof(file));
		if (e) {
			sdUnlock();
			return e;
		}
	}

	char        path[sizeof(IMG_SHOT_DIR) + SD_NAME_MAX + 1];
	struct stat st;
	snprintf(path, sizeof(path), "%s/%s", IMG_SHOT_DIR, file);
	if (!force && stat(path, &st) == 0) {
		sdUnlock();
		return "file exists";
	}

	FILE *f = fopen(path, "wb");
	if (!f) {
		sdUnlock();
		return "cannot write file";
	}
	esp_err_t err = imgShotWrite(f);
	if (fclose(f) != 0 && err == ESP_OK) {
		err = ESP_FAIL;
	}
	if (err != ESP_OK) {
		remove(path);
	}
	sdUnlock();
	if (err != ESP_OK) {
		return (err == ESP_ERR_NO_MEM) ? "out of memory" : "cannot write file";
	}

	char text[SD_NAME_MAX + 2];
	snprintf(text, sizeof(text), "%s\n", file);
	reply(text);
	return NULL;
}

/* Encode the glass (current rotation) as an 8-bit RGB PNG. */
static esp_err_t imgShotWrite(FILE *f)
{
	int        w    = lcdWidth(), h = lcdHeight();
	uint16_t  *px   = malloc(w * sizeof(uint16_t));
	uint8_t   *rgb  = malloc(w * 3);
	esp_err_t  result = ESP_ERR_NO_MEM;
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop   info = png ? png_create_info_struct(png) : NULL;

	if (px && rgb && png && info) {
		if (setjmp(png_jmpbuf(png)) == 0) {
			png_init_io(png, f);
			png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
			             PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
			png_write_info(png, info);
			for (int y = 0; y < h; y++) {
				lcdReadRow(y, px);
				for (int x = 0; x < w; x++) {
					uint16_t c = px[x];
					uint8_t  r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
					rgb[x * 3 + 0] = (r << 3) | (r >> 2);
					rgb[x * 3 + 1] = (g << 2) | (g >> 4);
					rgb[x * 3 + 2] = (b << 3) | (b >> 2);
				}
				png_write_row(png, rgb);
			}
			png_write_end(png, NULL);
			result = ESP_OK;
		} else {
			result = ESP_FAIL;
		}
	}

	png_destroy_write_struct(png ? &png : NULL, info ? &info : NULL);
	free(px);
	free(rgb);
	return result;
}
