#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Onboard addressable RGB LED (WS2812-type) */
#define LED_GPIO   38

/* Init LED (off) and register the "LED" command. Call after cmdInit(). */
esp_err_t ledInit(void);

/* Set the colour (stored; shown unless hidden). Thread-safe. */
esp_err_t ledSetRgb(uint8_t r, uint8_t g, uint8_t b);

/* Same as ledSetRgb(0, 0, 0). */
esp_err_t ledOff(void);

/* Hide (LED dark; colour changes are only stored) or show the stored colour.
 * Used by IMU SLEEP. */
esp_err_t ledHide(bool hide);
