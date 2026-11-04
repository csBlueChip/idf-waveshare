#include "bat.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "cmd.h"
#include "lcd.h"

static const char *TAG = "bat";

#define BAT_GPIO		1
#define BAT_SAMPLES		16
#define BAT_DIV_NUM		3	/* Vb = Vadc * 3 / 1: divider 200K/100K (measured: BAT 4.2 V, ADC 1.39 V) */
#define BAT_DIV_DEN		1

/* Screen layout (text grid), matching imu.c: under gZ */
#define BAT_FIELD		13
#define BAT_ROW_PORT		9
#define BAT_ROW_LAND		5

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_chan;

static const char *batCmd(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t batInit(void)
{
	adc_unit_t unit;
	ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BAT_GPIO, &unit, &s_chan), TAG, "io to channel");

	adc_oneshot_unit_init_cfg_t unitCfg = { .unit_id = unit };
	ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unitCfg, &s_adc), TAG, "adc unit");

	adc_oneshot_chan_cfg_t chanCfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
	ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chanCfg), TAG, "adc channel");

	adc_cali_curve_fitting_config_t caliCfg = {
		.unit_id  = unit,
		.chan     = s_chan,
		.atten    = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&caliCfg, &s_cali), TAG, "calibration");

	return cmdRegister("BAT", 0, 0, batCmd, "BAT  battery voltage");
}

esp_err_t batRead(float *volts)
{
	int sum = 0;
	for (int i = 0; i < BAT_SAMPLES; i++) {
		int raw, mv;
		ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, s_chan, &raw), TAG, "read");
		ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "cali");
		sum += mv;
	}
	*volts = (float)sum * BAT_DIV_NUM / (BAT_DIV_DEN * BAT_SAMPLES * 1000.0f);
	return ESP_OK;
}

/* ================= Command ================= */

static const char *batCmd(int argc, char *argv[], cmdReply_t reply)
{
	float v;
	if (batRead(&v) != ESP_OK) {
		return "ADC read failed";
	}

	char line[16], text[20];
	snprintf(line, sizeof(line), "Vb: %.2f V", v);

//	bool landscape = lcdWidth() > lcdHeight();
//	int  col = landscape ? lcdTextCols() / 2 : 0;
//	int  row = landscape ? BAT_ROW_LAND : BAT_ROW_PORT;
//	if (lcdPrintAt(col, row, BAT_FIELD, line) != ESP_OK) {
//		ESP_LOGW(TAG, "LCD row %d not drawn", row);
//	}

	snprintf(text, sizeof(text), "%s\n", line);
	reply(text);
	return NULL;
}
