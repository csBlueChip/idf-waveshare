#pragma once

#include "esp_err.h"

/*
 * Radio settings - a short-name front end to the rad.* NV keys (see nv.h).
 * Stored only; applied at the next boot.
 *
 *   RAD SET key value [key value ...]   all pairs are checked, then all stored (or none)
 *   RAD GET [key ...]                   show (all if no key)
 *
 *   keys: ssid pwd ble ip netmask pwr nogw maxcli
 */

/* Register RAD. Call after nvInit(). */
esp_err_t radInit(void);
