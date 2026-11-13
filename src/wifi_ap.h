#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Wi-Fi soft access point (the ESP is the network).
 * SSID, password, IP, netmask, TX power, gateway and max clients come from NV (wifi.*)
 * at first start. Full power (20 dBm) is unreliable with this board's chip
 * antenna; the default 8.5 dBm gives a few metres, which is plenty. */
#define WIFI_AP_CHANNEL		6

/* Bring up the access point (initialises Wi-Fi on first use). portal: DHCP
 * also offers http://<wifi.ip>[:port]/ (NV web.port) as captive-portal URL (option 114); once
 * offered it stays until reboot (the DHCP server cannot clear it). */
esp_err_t wifiApStart(bool portal);

/* Link callback (event task): up = a station has been given an IP by DHCP,
 * down = the last station has left. */
void wifiApOnLink(void (*cb)(bool up));

/* Take the access point down (radio off). */
esp_err_t wifiApStop(void);
