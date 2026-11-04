#pragma once

#include "esp_err.h"

/*
 * Battery voltage on GPIO1 (ADC1 channel 0). The board divides the battery
 * voltage by 3 (200K/100K: ADC sees Vb / 3). With no battery the charger
 * holds BAT at ~4.2 V, so "no battery" reads like "full".
 *
 * Command registered by batInit():
 *   BAT    "Vb: x.yy V" - replied and drawn under gZ
 *          (portrait row 9; landscape row 5, right half)
 */

/* Set up the ADC and calibration; register BAT. Call after cmdInit(). */
esp_err_t batInit(void);

/* Battery voltage in volts (average of 16 calibrated readings). */
esp_err_t batRead(float *volts);
