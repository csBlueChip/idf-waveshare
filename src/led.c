#include "led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "cmd.h"

static const char *TAG = "led";

#define LED_RMT_RES_HZ  (10 * 1000 * 1000)

static led_strip_handle_t s_strip;
static SemaphoreHandle_t  s_lock;
static uint8_t            s_rgb[3];	/* colour as set */
static bool               s_hidden;	/* dark until shown again (IMU SLEEP) */

static esp_err_t ledShow(void);

static const char *ledCmd(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t ledInit(void)
{
	led_strip_config_t stripCfg = {
		.strip_gpio_num         = LED_GPIO,
		.max_leds               = 1,
		.led_model              = LED_MODEL_WS2812,
		.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,   /* this board's LED is RGB, not GRB */
	};
	led_strip_rmt_config_t rmtCfg = {
		.clk_src       = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = LED_RMT_RES_HZ,
	};

	s_lock = xSemaphoreCreateMutex();
	if (!s_lock) {
		return ESP_ERR_NO_MEM;
	}
	esp_err_t err = led_strip_new_rmt_device(&stripCfg, &rmtCfg, &s_strip);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
		return err;
	}

	err = ledOff();
	if (err != ESP_OK) {
		return err;
	}

	return cmdRegister("LED", 3, 3, ledCmd, "LED r g b  set LED colour (0..255)");
}

esp_err_t ledSetRgb(uint8_t r, uint8_t g, uint8_t b)
{
	if (!s_strip) {
		return ESP_ERR_INVALID_STATE;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	s_rgb[0] = r;
	s_rgb[1] = g;
	s_rgb[2] = b;
	esp_err_t err = ledShow();
	xSemaphoreGive(s_lock);
	return err;
}

esp_err_t ledOff(void)
{
	return ledSetRgb(0, 0, 0);
}

esp_err_t ledHide(bool hide)
{
	if (!s_strip) {
		return ESP_ERR_INVALID_STATE;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	s_hidden = hide;
	esp_err_t err = ledShow();
	xSemaphoreGive(s_lock);
	return err;
}

/* ================= Internals ================= */

/* Put the stored colour (or dark while hidden) on the LED. Caller holds s_lock. */
static esp_err_t ledShow(void)
{
	if (s_hidden || (s_rgb[0] | s_rgb[1] | s_rgb[2]) == 0) {
		return led_strip_clear(s_strip);
	}
	esp_err_t err = led_strip_set_pixel(s_strip, 0, s_rgb[0], s_rgb[1], s_rgb[2]);
	if (err == ESP_OK) {
		err = led_strip_refresh(s_strip);
	}
	return err;
}

/* ================= Commands ================= */

static const char *ledCmd(int argc, char *argv[], cmdReply_t reply)
{
	uint8_t r, g, b;
	if (!cmdParseU8(argv[0], &r) || !cmdParseU8(argv[1], &g) || !cmdParseU8(argv[2], &b)) {
		return "values must be 0..255";
	}
	if (ledSetRgb(r, g, b) != ESP_OK) {
		return "LED write failed";
	}
	return NULL;
}
