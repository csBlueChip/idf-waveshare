#include "btn.h"
#include "nv.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/touch_sens.h"

#include "cmd.h"
#include "ui.h"

static const char *TAG = "btn";

#define BTN_TASK_STACK		4096
#define BTN_TASK_PRIO		6
#define BTN_QUEUE_DEPTH		32
#define BTN_TIME_MAX		5000
#define BTN_BOUNCE_MAX		200

#define BTN_TOUCH_INIT_SCANS	3

/* Button n -> GPIO. Button 1 must always be the BOOT pin (GPIO0): the Wait
 * screen and IMU SLEEP read BOOT directly, scripts use BTN 1 for it, and it is
 * TACT only (GPIO0 is a strapping pin, not a touch channel). Do not move it. */
static const gpio_num_t s_gpio[BTN_COUNT + 1] = { -1, 0, 2, 4, 5, 6, 7, 8, 9, 10, 11 };

typedef enum { BTN_OFF = 0, BTN_TACT, BTN_TOUCH } btnType_t;

typedef struct {
	int b, l, d, r;				/* ms */
} btnTiming_t;

typedef struct {
	btnType_t   type;
	bool        eShort, eLong, eDouble;
	btnTiming_t t;

	/* state (button task only) */
	bool        pressed;		/* debounced state */
	int8_t      raw;		/* latest raw state from a touch event, -1 = read GPIO */
	int64_t     bounceAt;		/* debounce deadline, 0 = none */
	int64_t     longAt;		/* long / next repeat deadline, 0 = none */
	int64_t     dblAt;		/* double-click window end, 0 = none */
	bool        longActive;
	bool        ignoreRelease;
	touch_channel_handle_t chan;
} btn_t;

/* raw edge/touch event from an ISR */
typedef struct {
	uint8_t n;
	int8_t  raw;			/* -1 GPIO edge, 0/1 touch inactive/active */
} btnEvent_t;

static btn_t                 s_btn[BTN_COUNT + 1];
static btnTiming_t           s_global;		/* from NV btn.deb/long/dbl/rpt at init */
static float                 s_touchRatio;	/* NV btn.touch: active when smooth > benchmark * (1 + ratio) */
static QueueHandle_t         s_queue;
static SemaphoreHandle_t     s_lock;
static bool                  s_isrService;
static touch_sensor_handle_t s_touch;
static bool                  s_touchRunning;

static void        btnTask(void *arg);
static void        btnIsr(void *arg);
static bool        btnTouchActive(touch_sensor_handle_t h, const touch_active_event_data_t *e, void *ctx);
static bool        btnTouchInactive(touch_sensor_handle_t h, const touch_inactive_event_data_t *e, void *ctx);
static void        btnEmit(int n, const char *what);
static void        btnOnPress(int n, int64_t now);
static void        btnOnRelease(int n, int64_t now);
static int64_t     btnNowMs(void);
static esp_err_t   btnEnable(int n, btnType_t type, bool s, bool l, bool d, cmdReply_t reply);
static void        btnDisable(int n);
static esp_err_t   btnTouchStop(void);
static esp_err_t   btnTouchStart(cmdReply_t reply);
static const char *btnSetTiming(btnTiming_t *t, const char *key, const char *val);
static const char *btnCmd(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t btnInit(void)
{
	s_global = (btnTiming_t) {			/* NV btn.deb/long/dbl/rpt */
		.b = nvGetInt("btn.deb"), .l = nvGetInt("btn.long"), .d = nvGetInt("btn.dbl"), .r = nvGetInt("btn.rpt"),
	};
	s_touchRatio = nvGetFloat("btn.touch");

	s_queue = xQueueCreate(BTN_QUEUE_DEPTH, sizeof(btnEvent_t));
	s_lock  = xSemaphoreCreateMutex();
	if (!s_queue || !s_lock) {
		return ESP_ERR_NO_MEM;
	}
	if (xTaskCreate(btnTask, "btn", BTN_TASK_STACK, NULL, BTN_TASK_PRIO, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	return cmdRegister("BTN", 0, 5, btnCmd,
	                   "BTN [n s l d [TACT|TOUCH]] | BTN [n] SET deb|long|dbl|rpt ms | BTN CAL");
}

/* ================= Event sources (ISR context) ================= */

static void IRAM_ATTR btnIsr(void *arg)
{
	btnEvent_t ev = { .n = (uint8_t)(intptr_t)arg, .raw = -1 };
	BaseType_t woken = pdFALSE;
	xQueueSendFromISR(s_queue, &ev, &woken);
	if (woken) {
		portYIELD_FROM_ISR();
	}
}

static bool btnTouchEvent(int chanId, int8_t active)
{
	for (int n = 2; n <= BTN_COUNT; n++) {		/* touch channel id == GPIO number */
		if (s_gpio[n] == chanId) {
			btnEvent_t ev = { .n = n, .raw = active };
			BaseType_t woken = pdFALSE;
			xQueueSendFromISR(s_queue, &ev, &woken);
			return woken == pdTRUE;
		}
	}
	return false;
}

static bool btnTouchActive(touch_sensor_handle_t h, const touch_active_event_data_t *e, void *ctx)
{
	return btnTouchEvent(e->chan_id, 1);
}

static bool btnTouchInactive(touch_sensor_handle_t h, const touch_inactive_event_data_t *e, void *ctx)
{
	return btnTouchEvent(e->chan_id, 0);
}

/* ================= Gesture task ================= */

static int64_t btnNowMs(void)
{
	return esp_timer_get_time() / 1000;
}

static void btnEmit(int n, const char *what)
{
	char msg[32];
	snprintf(msg, sizeof(msg), "<<BTN %d %s>>\n", n, what);
	uiSend(msg);
}

static void btnOnPress(int n, int64_t now)
{
	btn_t *b = &s_btn[n];

	if (b->eDouble && b->dblAt) {			/* second press inside the window */
		b->dblAt         = 0;
		b->ignoreRelease = true;
		btnEmit(n, "DOUBLE");
		return;
	}
	b->ignoreRelease = false;
	b->longActive    = false;
	b->longAt        = b->eLong ? now + b->t.l : 0;

	if (b->eShort && !b->eLong && !b->eDouble) {	/* short only: immediately */
		b->ignoreRelease = true;
		btnEmit(n, "SHORT");
		if (b->t.r > 0) {
			b->longAt = now + b->t.l;	/* typematic: repeat SHORT from l, every r */
		}
	}
}

static void btnOnRelease(int n, int64_t now)
{
	btn_t *b = &s_btn[n];

	b->longAt = 0;
	if (b->ignoreRelease) {
		b->ignoreRelease = false;
		return;
	}
	if (b->longActive) {
		b->longActive = false;
		if (b->t.r == 0) {
			btnEmit(n, "UP");
		}
		return;
	}
	if (b->eDouble) {
		b->dblAt = now + b->t.d;		/* wait for a second press */
	} else if (b->eShort) {
		btnEmit(n, "SHORT");
	}
}

/* Waits on the event queue with a timeout set by the nearest deadline - no polling. */
static void btnTask(void *arg)
{
	for (;;) {
		/* nearest deadline */
		int64_t now  = btnNowMs();
		int64_t next = INT64_MAX;
		xSemaphoreTake(s_lock, portMAX_DELAY);
		for (int n = 1; n <= BTN_COUNT; n++) {
			btn_t *b = &s_btn[n];
			if (b->type == BTN_OFF) {
				continue;
			}
			if (b->bounceAt && b->bounceAt < next) next = b->bounceAt;
			if (b->longAt   && b->longAt   < next) next = b->longAt;
			if (b->dblAt    && b->dblAt    < next) next = b->dblAt;
		}
		xSemaphoreGive(s_lock);

		TickType_t wait = portMAX_DELAY;
		if (next != INT64_MAX) {
			int64_t ms = next - now;
			if (ms <= 0) {
				wait = 0;
			} else {
				wait = pdMS_TO_TICKS(ms);
				if (pdTICKS_TO_MS(wait) < ms) {
					wait++;			/* round up: never wake early */
				}
			}
		}

		btnEvent_t ev;
		bool       got = xQueueReceive(s_queue, &ev, wait) == pdTRUE;

		xSemaphoreTake(s_lock, portMAX_DELAY);
		now = btnNowMs();

		if (got && ev.n >= 1 && ev.n <= BTN_COUNT && s_btn[ev.n].type != BTN_OFF) {
			btn_t *b    = &s_btn[ev.n];
			b->raw      = ev.raw;
			b->bounceAt = now + b->t.b;
			if (b->bounceAt == 0) {
				b->bounceAt = 1;
			}
		}

		for (int n = 1; n <= BTN_COUNT; n++) {
			btn_t *b = &s_btn[n];
			if (b->type == BTN_OFF) {
				continue;
			}

			if (b->bounceAt && now >= b->bounceAt) {	/* settled: read the state */
				b->bounceAt  = 0;
				bool pressed = (b->raw >= 0) ? (b->raw == 1) : (gpio_get_level(s_gpio[n]) == 0);
				if (pressed != b->pressed) {
					b->pressed = pressed;
					if (pressed) {
						btnOnPress(n, now);
					} else {
						btnOnRelease(n, now);
					}
				}
			}

			if (b->longAt && now >= b->longAt && b->pressed) {
				if (!b->eLong) {
					btnEmit(n, "SHORT");			/* typematic repeat */
				} else if (!b->longActive) {
					b->longActive = true;
					btnEmit(n, b->t.r ? "REPEAT" : "DOWN");
				} else {
					btnEmit(n, "REPEAT");
				}
				b->longAt = b->t.r ? b->longAt + b->t.r : 0;	/* from the deadline: no drift */
				if (b->longAt && b->longAt <= now) {
					b->longAt = now + b->t.r;		/* fell behind: don't burst */
				}
			}

			if (b->dblAt && now >= b->dblAt) {		/* no second press */
				b->dblAt = 0;
				if (b->eShort) {
					btnEmit(n, "SHORT");
				}
			}
		}
		xSemaphoreGive(s_lock);
	}
}

/* ================= Touch controller ================= */

/* Stop scanning so channels can be added/removed/recalibrated. */
static esp_err_t btnTouchStop(void)
{
	if (s_touch && s_touchRunning) {
		ESP_RETURN_ON_ERROR(touch_sensor_stop_continuous_scanning(s_touch), TAG, "stop");
		ESP_RETURN_ON_ERROR(touch_sensor_disable(s_touch), TAG, "disable");
		s_touchRunning = false;
	}
	return ESP_OK;
}

/* Calibrate every touch channel (benchmark -> threshold), then scan continuously.
 * Reports "BTN n CAL benchmark" per pad when reply is given. */
static esp_err_t btnTouchStart(cmdReply_t reply)
{
	bool any = false;
	for (int n = 2; n <= BTN_COUNT; n++) {
		any |= (s_btn[n].type == BTN_TOUCH);
	}
	if (!s_touch || !any) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "enable");
	for (int i = 0; i < BTN_TOUCH_INIT_SCANS; i++) {
		ESP_RETURN_ON_ERROR(touch_sensor_trigger_oneshot_scanning(s_touch, 2000), TAG, "scan");
	}
	ESP_RETURN_ON_ERROR(touch_sensor_disable(s_touch), TAG, "disable");

	for (int n = 2; n <= BTN_COUNT; n++) {
		btn_t *b = &s_btn[n];
		if (b->type != BTN_TOUCH) {
			continue;
		}
		uint32_t bm[TOUCH_SAMPLE_CFG_NUM] = { 0 };
		ESP_RETURN_ON_ERROR(touch_channel_read_data(b->chan, TOUCH_CHAN_DATA_TYPE_BENCHMARK, bm),
		                    TAG, "benchmark");

		touch_channel_config_t cfg = {
			.charge_speed     = TOUCH_CHARGE_SPEED_7,
			.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
		};
		for (int j = 0; j < TOUCH_SAMPLE_CFG_NUM; j++) {
			cfg.active_thresh[j] = (uint32_t)(bm[j] * s_touchRatio);
			if (cfg.active_thresh[j] == 0) {
				cfg.active_thresh[j] = 1;
			}
		}
		ESP_RETURN_ON_ERROR(touch_sensor_reconfig_channel(b->chan, &cfg), TAG, "threshold");

		b->pressed  = false;			/* start released */
		b->raw      = 0;
		b->bounceAt = b->longAt = b->dblAt = 0;
		if (reply) {
			char line[32];
			snprintf(line, sizeof(line), "BTN %d CAL %lu\n", n, (unsigned long)bm[0]);
			reply(line);
		}
	}

	ESP_RETURN_ON_ERROR(touch_sensor_enable(s_touch), TAG, "enable");
	ESP_RETURN_ON_ERROR(touch_sensor_start_continuous_scanning(s_touch), TAG, "start");
	s_touchRunning = true;
	return ESP_OK;
}

static esp_err_t btnTouchCreate(void)
{
	if (s_touch) {
		return ESP_OK;
	}
	static touch_sensor_sample_config_t sample[TOUCH_SAMPLE_CFG_NUM] = {
		TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
	};
	touch_sensor_config_t        sensCfg   = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample);
	touch_sensor_filter_config_t filterCfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
	touch_event_callbacks_t      cbs       = {
		.on_active   = btnTouchActive,
		.on_inactive = btnTouchInactive,
	};
	ESP_RETURN_ON_ERROR(touch_sensor_new_controller(&sensCfg, &s_touch), TAG, "controller");
	ESP_RETURN_ON_ERROR(touch_sensor_config_filter(s_touch, &filterCfg), TAG, "filter");
	ESP_RETURN_ON_ERROR(touch_sensor_register_callbacks(s_touch, &cbs, NULL), TAG, "callbacks");
	return ESP_OK;
}

/* ================= Enable / disable ================= */

static void btnDisable(int n)
{
	btn_t *b = &s_btn[n];

	if (b->type == BTN_TACT) {
		gpio_isr_handler_remove(s_gpio[n]);
		gpio_reset_pin(s_gpio[n]);
	} else if (b->type == BTN_TOUCH) {
		btnTouchStop();
		touch_sensor_del_channel(b->chan);
		b->chan = NULL;
		btnTouchStart(NULL);			/* restart for the remaining pads */
	}
	memset(b, 0, sizeof(*b));
}

static esp_err_t btnEnable(int n, btnType_t type, bool s, bool l, bool d, cmdReply_t reply)
{
	btn_t *b = &s_btn[n];

	if (b->type != BTN_OFF) {
		btnDisable(n);
	}

	*b = (btn_t) {
		.eShort = s, .eLong = l, .eDouble = d,
		.t      = s_global,			/* inherit global timings */
		.raw    = -1,
	};

	if (type == BTN_TACT) {
		if (!s_isrService) {
			esp_err_t err = gpio_install_isr_service(0);
			if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {	/* already installed is fine */
				return err;
			}
			s_isrService = true;
		}
		gpio_config_t io = {
			.pin_bit_mask = 1ULL << s_gpio[n],
			.mode         = GPIO_MODE_INPUT,
			.pull_up_en   = GPIO_PULLUP_ENABLE,
			.intr_type    = GPIO_INTR_ANYEDGE,
		};
		ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
		ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_gpio[n], btnIsr, (void *)(intptr_t)n), TAG, "isr");
		b->pressed = (gpio_get_level(s_gpio[n]) == 0);
		b->type    = BTN_TACT;
		return ESP_OK;
	}

	/* touch */
	ESP_RETURN_ON_ERROR(btnTouchCreate(), TAG, "touch");
	ESP_RETURN_ON_ERROR(btnTouchStop(), TAG, "touch stop");
	touch_channel_config_t cfg = {
		.active_thresh    = { 2000 },		/* replaced by calibration */
		.charge_speed     = TOUCH_CHARGE_SPEED_7,
		.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
	};
	ESP_RETURN_ON_ERROR(touch_sensor_new_channel(s_touch, s_gpio[n], &cfg, &b->chan), TAG, "channel");
	b->type = BTN_TOUCH;
	return btnTouchStart(reply);
}

/* ================= Commands ================= */

static const char *btnSetTiming(btnTiming_t *t, const char *key, const char *val)
{
	int v;

	if (strcasecmp(key, "deb") == 0) {
		if (!cmdParseInt(val, 0, BTN_BOUNCE_MAX, &v)) return "deb must be 0..200";
		t->b = v;
	} else if (strcasecmp(key, "long") == 0) {
		if (!cmdParseInt(val, 50, BTN_TIME_MAX, &v)) return "long must be 50..5000";
		t->l = v;
	} else if (strcasecmp(key, "dbl") == 0) {
		if (!cmdParseInt(val, 50, BTN_TIME_MAX, &v)) return "dbl must be 50..5000";
		t->d = v;
	} else if (strcasecmp(key, "rpt") == 0) {
		if (!cmdParseInt(val, 0, BTN_TIME_MAX, &v)) return "rpt must be 0..5000";
		t->r = v;
	} else {
		return "usage: BTN [n] SET deb|long|dbl|rpt ms";
	}
	return NULL;
}

static void btnList(cmdReply_t reply)
{
	char line[96];
	snprintf(line, sizeof(line), "global deb=%d long=%d dbl=%d rpt=%d\n",
	         s_global.b, s_global.l, s_global.d, s_global.r);
	reply(line);
	for (int n = 1; n <= BTN_COUNT; n++) {
		const btn_t *b = &s_btn[n];
		if (b->type == BTN_OFF) {
			snprintf(line, sizeof(line), "%d GP%d off\n", n, s_gpio[n]);
		} else {
			snprintf(line, sizeof(line), "%d GP%d %s %d %d %d deb=%d long=%d dbl=%d rpt=%d\n", n, s_gpio[n],
			         b->type == BTN_TACT ? "TACT" : "TOUCH", b->eShort, b->eLong, b->eDouble,
			         b->t.b, b->t.l, b->t.d, b->t.r);
		}
		reply(line);
	}
}

static const char *btnCmd(int argc, char *argv[], cmdReply_t reply)
{
	const char *err = NULL;
	int         n, s, l, d;

	xSemaphoreTake(s_lock, portMAX_DELAY);

	if (argc == 0) {
		btnList(reply);
	} else if (strcasecmp(argv[0], "CAL") == 0 && argc == 1) {
		if (btnTouchStop() != ESP_OK || btnTouchStart(reply) != ESP_OK) {
			err = "touch calibration failed";
		}
	} else if (strcasecmp(argv[0], "SET") == 0 && argc == 3) {
		err = btnSetTiming(&s_global, argv[1], argv[2]);
	} else if (!cmdParseInt(argv[0], 1, BTN_COUNT, &n)) {
		err = "button must be 1..10";
	} else if (argc == 4 && strcasecmp(argv[1], "SET") == 0) {
		err = (s_btn[n].type == BTN_OFF) ? "button not enabled" : btnSetTiming(&s_btn[n].t, argv[2], argv[3]);
	} else if (argc < 4) {
		err = "usage: BTN n short long double [TACT|TOUCH]";
	} else if (!cmdParseInt(argv[1], 0, 1, &s) || !cmdParseInt(argv[2], 0, 1, &l) ||
	           !cmdParseInt(argv[3], 0, 1, &d)) {
		err = "short long double must be 0|1";
	} else {
		btnType_t type = BTN_TACT;
		if (argc == 5) {
			if (strcasecmp(argv[4], "TOUCH") == 0) {
				type = BTN_TOUCH;
			} else if (strcasecmp(argv[4], "TACT") != 0) {
				err = "type must be TACT|TOUCH";
			}
		}
		if (!err && type == BTN_TOUCH && n == 1) {
			err = "button 1 is TACT only";
		}
		if (!err) {
			if (!s && !l && !d) {
				if (s_btn[n].type != BTN_OFF) {
					btnDisable(n);
				}
			} else if (btnEnable(n, type, s, l, d, reply) != ESP_OK) {
				btnDisable(n);
				err = "button setup failed";
			}
		}
	}

	xSemaphoreGive(s_lock);
	return err;
}
