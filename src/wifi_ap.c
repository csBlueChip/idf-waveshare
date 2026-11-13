#include "wifi_ap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"

#include "nv.h"

static const char *TAG = "wifi";

static bool         s_initDone;
static esp_netif_t *s_netif;
static void       (*s_linkCb)(bool up);

static esp_err_t wifiApInitOnce(void);
static esp_err_t wifiApSetIp(bool portal);
static void      wifiApOnLeave(void *arg, esp_event_base_t base, int32_t id, void *data);
static void      wifiApOnIp(void *arg, esp_event_base_t base, int32_t id, void *data);
static void      wifiApOnEvent(void *arg, esp_event_base_t base, int32_t id, void *data);
static void      wifiApDump(void);

/* ================= Public API ================= */

esp_err_t wifiApStart(bool portal)
{
	ESP_RETURN_ON_ERROR(wifiApInitOnce(), TAG, "init");
	ESP_RETURN_ON_ERROR(wifiApSetIp(portal), TAG, "ip");

	wifi_config_t cfg = {
		.ap = {
			.channel        = WIFI_AP_CHANNEL,
			.authmode       = WIFI_AUTH_WPA2_PSK,
			.pmf_cfg        = { .required = false },
		},
	};

	const char *ssid = nvGet("wifi.ssid");
	strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
	cfg.ap.ssid_len = strlen(ssid);
	strlcpy((char *)cfg.ap.password, nvGet("wifi.pwd"), sizeof(cfg.ap.password));
	cfg.ap.max_connection = nvGetInt("wifi.maxconn");
	int8_t qdbm = (int8_t)lroundf(nvGetFloat("wifi.pwr") * 4.0f);	/* 0.25 dBm units */

	ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode");
	ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "config");
	ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
	ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(qdbm), TAG, "tx power");

	ESP_LOGI(TAG, "AP \"%s\" up at %s%s", ssid, nvGet("wifi.ip"), nvGet("wifi.cidr"));
	wifiApDump();
	return ESP_OK;
}

void wifiApOnLink(void (*cb)(bool up))
{
	s_linkCb = cb;
}

esp_err_t wifiApStop(void)
{
	if (!s_initDone) {
		return ESP_OK;
	}
	esp_err_t err = esp_wifi_stop();
	ESP_LOGI(TAG, "AP down");
	return err;
}

/* ================= Internals ================= */

static esp_err_t wifiApInitOnce(void)
{
	if (s_initDone) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");

	esp_err_t err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {	/* already created is fine */
		return err;
	}

	s_netif = esp_netif_create_default_wifi_ap();
	if (!s_netif) {
		return ESP_FAIL;
	}

	wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_RETURN_ON_ERROR(esp_wifi_init(&initCfg), TAG, "wifi init");
	ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");

	ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifiApOnLeave, NULL),
	                    TAG, "leave handler");
	ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, wifiApOnIp, NULL),
	                    TAG, "ip handler");

	/* DEBUG: log all Wi-Fi events, including probe requests (masked by default) */
	ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiApOnEvent, NULL),
	                    TAG, "event handler");
	esp_wifi_set_event_mask(WIFI_EVENT_MASK_NONE);

	s_initDone = true;
	return ESP_OK;
}

/* A station left: if none remain, tell the owner (its TCP clients are gone). */
static void wifiApOnLeave(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	wifi_sta_list_t list = { 0 };
	if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num == 0 && s_linkCb) {
		s_linkCb(false);
	}
}

/* DHCP gave a station an address: the link is usable. */
static void wifiApOnIp(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (s_linkCb) {
		s_linkCb(true);
	}
}

/* AP address and netmask from NV; with wifi.gw = 0 DHCP offers no router (gateway);
 * portal: DHCP also offers the captive-portal URL (NV web.port, if not 80). */
static esp_err_t wifiApSetIp(bool portal)
{
	static char uri[32];					/* DHCP server keeps the pointer */

	esp_netif_ip_info_t ip = { 0 };
	int bits = atoi(nvGet("wifi.cidr") + 1);

	ip.ip.addr      = ipaddr_addr(nvGet("wifi.ip"));
	ip.gw.addr      = ip.ip.addr;
	ip.netmask.addr = lwip_htonl(0xFFFFFFFFu << (32 - bits));

	uint8_t router = nvGetInt("wifi.gw") ? 1 : 0;

	esp_err_t err = esp_netif_dhcps_stop(s_netif);
	if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
		return err;
	}
	ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &ip), TAG, "set ip");
	ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
	                                           &router, sizeof(router)), TAG, "dhcps router");
	if (portal) {
		int port = nvGetInt("web.port");
		if (port == 80) {
			snprintf(uri, sizeof(uri), "http://%s/", nvGet("wifi.ip"));
		} else {
			snprintf(uri, sizeof(uri), "http://%s:%d/", nvGet("wifi.ip"), port);
		}
		ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
		                                           uri, strlen(uri)), TAG, "dhcps portal");
	}
	return esp_netif_dhcps_start(s_netif);
}

/* ================= DEBUG ================= */

static void wifiApOnEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	switch (id) {
	case WIFI_EVENT_AP_START:
		ESP_LOGI(TAG, "DBG event: AP_START");
		break;
	case WIFI_EVENT_AP_STOP:
		ESP_LOGI(TAG, "DBG event: AP_STOP");
		break;
	case WIFI_EVENT_AP_STACONNECTED: {
		wifi_event_ap_staconnected_t *e = data;
		ESP_LOGI(TAG, "DBG event: STA joined " MACSTR, MAC2STR(e->mac));
		break;
	}
	case WIFI_EVENT_AP_STADISCONNECTED: {
		wifi_event_ap_stadisconnected_t *e = data;
		ESP_LOGI(TAG, "DBG event: STA left " MACSTR " reason %d", MAC2STR(e->mac), e->reason);
		break;
	}
	case WIFI_EVENT_AP_PROBEREQRECVED: {
		wifi_event_ap_probe_req_rx_t *e = data;
		ESP_LOGI(TAG, "DBG event: probe from " MACSTR " rssi %d", MAC2STR(e->mac), e->rssi);
		break;
	}
	default:
		ESP_LOGI(TAG, "DBG event: id %ld", (long)id);
		break;
	}
}

/* Read back what the driver is actually running. */
static void wifiApDump(void)
{
	wifi_mode_t mode = WIFI_MODE_NULL;
	esp_wifi_get_mode(&mode);

	wifi_config_t cfg = { 0 };
	esp_wifi_get_config(WIFI_IF_AP, &cfg);

	uint8_t primary = 0;
	wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
	esp_wifi_get_channel(&primary, &second);

	wifi_country_t cc = { 0 };
	esp_wifi_get_country(&cc);

	int8_t power = 0;
	esp_wifi_get_max_tx_power(&power);

	uint8_t proto = 0;
	esp_wifi_get_protocol(WIFI_IF_AP, &proto);

	wifi_bandwidth_t bw = 0;
	esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);

	ESP_LOGI(TAG, "DBG mode %d (2=AP) ssid \"%.*s\" len %d hidden %d auth %d (3=WPA2) maxconn %d beacon %d",
	         mode, cfg.ap.ssid_len, (const char *)cfg.ap.ssid, cfg.ap.ssid_len, cfg.ap.ssid_hidden,
	         cfg.ap.authmode, cfg.ap.max_connection, cfg.ap.beacon_interval);
	ESP_LOGI(TAG, "DBG channel %d/%d country \"%.2s\" ch %d..%d policy %d",
	         primary, second, cc.cc, cc.schan, cc.schan + cc.nchan - 1, cc.policy);
	ESP_LOGI(TAG, "DBG tx power %d.%02d dBm proto 0x%02X bw %d heap %u internal %u",
	         power / 4, (power % 4) * 25, proto, bw,
	         (unsigned)esp_get_free_heap_size(),
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
