#include "ble_serial.h"
#include "nv.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_attr.h"
#include "esp_bt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define BLE_ATT_MAX_VALUE   512   /* max ATT attribute length */
#define BLE_NOTIFY_RETRIES  20
#define BLE_NOTIFY_RETRY_MS 10

/* ---------- NUS UUIDs (little-endian byte order) ---------- */
static const ble_uuid128_t s_nusSvcUuid = BLE_UUID128_INIT(
	0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
	0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nusRxUuid = BLE_UUID128_INIT(
	0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
	0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nusTxUuid = BLE_UUID128_INIT(
	0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
	0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ---------- State ---------- */
static StreamBufferHandle_t s_rxStream;
static bleRxCb_t            s_rxCb;
static bleConnCb_t          s_connCb;
static uint8_t              s_ownAddrType;
static uint16_t             s_txHandle;
static volatile uint16_t    s_connHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool        s_connected;
static volatile bool        s_subscribed;
static volatile bool        s_synced;
static volatile bool        s_disabled;	/* bleDisable() called: no advertising until bleEnable() */

/* ---------- Forward decls ---------- */
static int  bleGattAccess(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  bleGapEvent(struct ble_gap_event *event, void *arg);
static void bleOnSync(void);
static void bleOnReset(int reason);
static void bleHostTask(void *param);

/* ---------- GATT table ---------- */
static const struct ble_gatt_svc_def s_gattSvcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &s_nusSvcUuid.u,
		.characteristics = (struct ble_gatt_chr_def[]) {
			{
				.uuid      = &s_nusRxUuid.u,
				.access_cb = bleGattAccess,
				.flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
			},
			{
				.uuid       = &s_nusTxUuid.u,
				.access_cb  = bleGattAccess,
				.flags      = BLE_GATT_CHR_F_NOTIFY,
				.val_handle = &s_txHandle,
			},
			{ 0 }
		},
	},
	{ 0 }
};

/* ================= Public API ================= */

esp_err_t bleInit(bleRxCb_t onRxCb, bleConnCb_t onConnCb)
{
	s_rxCb   = onRxCb;
	s_connCb = onConnCb;

	if (!s_rxCb) {
		s_rxStream = xStreamBufferCreate(BLE_RX_BUF_SIZE, 1);
		if (s_rxStream == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	esp_err_t err = nimble_port_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
		return err;
	}

	ble_hs_cfg.sync_cb  = bleOnSync;
	ble_hs_cfg.reset_cb = bleOnReset;

	ble_svc_gap_init();
	ble_svc_gatt_init();

	int rc = ble_gatts_count_cfg(s_gattSvcs);
	if (rc == 0) {
		rc = ble_gatts_add_svcs(s_gattSvcs);
	}
	if (rc != 0) {
		ESP_LOGE(TAG, "GATT register failed: %d", rc);
		return ESP_FAIL;
	}

	rc = ble_svc_gap_device_name_set(nvGet("ble.name"));
	if (rc != 0) {
		ESP_LOGE(TAG, "set device name failed: %d", rc);
		return ESP_FAIL;
	}

	nimble_port_freertos_init(bleHostTask);
	return ESP_OK;
}

esp_err_t bleStartAdvertising(void)
{
	if (!s_synced || s_disabled) {
		return ESP_ERR_INVALID_STATE;
	}

	/* Adv packet: flags + NUS UUID (name won't fit alongside a 128-bit UUID) */
	struct ble_hs_adv_fields adv = { 0 };
	adv.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	adv.uuids128             = &s_nusSvcUuid;
	adv.num_uuids128         = 1;
	adv.uuids128_is_complete = 1;

	int rc = ble_gap_adv_set_fields(&adv);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv set fields: %d", rc);
		return ESP_FAIL;
	}

	/* Scan response: device name */
	const char *name = ble_svc_gap_device_name();
	struct ble_hs_adv_fields rsp = { 0 };
	rsp.name             = (const uint8_t *)name;
	rsp.name_len         = strlen(name);
	rsp.name_is_complete = 1;

	rc = ble_gap_adv_rsp_set_fields(&rsp);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv rsp set fields: %d", rc);
		return ESP_FAIL;
	}

	struct ble_gap_adv_params params = { 0 };
	params.conn_mode = BLE_GAP_CONN_MODE_UND;
	params.disc_mode = BLE_GAP_DISC_MODE_GEN;

	rc = ble_gap_adv_start(s_ownAddrType, NULL, BLE_HS_FOREVER, &params, bleGapEvent, NULL);
	if (rc != 0 && rc != BLE_HS_EALREADY) {
		ESP_LOGE(TAG, "adv start: %d", rc);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "advertising as \"%s\"", name);
	return ESP_OK;
}

void bleDisable(void)
{
	s_disabled = true;
	ble_gap_adv_stop();
	if (s_connected) {
		ble_gap_terminate(s_connHandle, BLE_ERR_REM_USER_CONN_TERM);
	}
	ESP_LOGI(TAG, "disabled");
}

/* NV ble.pwr (dBm, already one of the steps) -> controller power level */
static void bleSetPower(void)
{
	static const int dbm[] = { -24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 20 };
	int want = nvGetInt("ble.pwr");
	int lvl  = ESP_PWR_LVL_P9;
	for (int i = 0; i < (int)(sizeof(dbm) / sizeof(dbm[0])); i++) {
		if (dbm[i] == want) {
			lvl = ESP_PWR_LVL_N24 + i;
		}
	}
	esp_err_t e1 = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, (esp_power_level_t)lvl);
	esp_err_t e2 = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)lvl);
	if (e1 != ESP_OK || e2 != ESP_OK) {
		ESP_LOGW(TAG, "TX power %d dBm not set", want);
	}
}

void bleEnable(void)
{
	s_disabled = false;
	bleSetPower();					/* NV ble.pwr, applied each time BLE starts */
	if (s_synced && !s_connected) {
		bleStartAdvertising();
	}
	ESP_LOGI(TAG, "enabled");
}

bool bleIsConnected(void)
{
	return s_connected;
}

esp_err_t bleSend(const uint8_t *data, size_t len)
{
	if (!s_connected || !s_subscribed) {
		return ESP_ERR_INVALID_STATE;
	}

	uint16_t conn = s_connHandle;
	uint16_t mtu  = ble_att_mtu(conn);
	if (mtu <= 3) {
		return ESP_ERR_INVALID_STATE;
	}
	size_t chunkMax = mtu - 3;

	while (len > 0) {
		size_t n = len < chunkMax ? len : chunkMax;
		int rc = BLE_HS_ENOMEM;

		for (int attempt = 0; attempt < BLE_NOTIFY_RETRIES; attempt++) {
			struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
			if (om != NULL) {
				rc = ble_gatts_notify_custom(conn, s_txHandle, om); /* consumes om */
				if (rc != BLE_HS_ENOMEM) {
					break;
				}
			}
			vTaskDelay(pdMS_TO_TICKS(BLE_NOTIFY_RETRY_MS)); /* wait for mbufs to free */
		}

		if (rc != 0) {
			ESP_LOGW(TAG, "notify failed: %d", rc);
			return ESP_FAIL;
		}
		data += n;
		len  -= n;
	}
	return ESP_OK;
}

size_t bleReceive(uint8_t *buf, size_t max, TickType_t timeout)
{
	if (!s_rxStream) {
		return 0;
	}
	return xStreamBufferReceive(s_rxStream, buf, max, timeout);
}

/* ================= Internals ================= */

static int bleGattAccess(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
		ble_uuid_cmp(ctxt->chr->uuid, &s_nusRxUuid.u) != 0) {
		return BLE_ATT_ERR_UNLIKELY;
	}

	static EXT_RAM_BSS_ATTR uint8_t tmp[BLE_ATT_MAX_VALUE]; /* host task only - not re-entrant */
	uint16_t len = 0;
	if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len) != 0) {
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	}

	if (s_rxCb) {
		s_rxCb(tmp, len);
	} else {
		size_t sent = xStreamBufferSend(s_rxStream, tmp, len, 0);
		if (sent < len) {
			ESP_LOGW(TAG, "rx overflow, dropped %u bytes", (unsigned)(len - sent));
		}
	}
	return 0;
}

static int bleGapEvent(struct ble_gap_event *event, void *arg)
{
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status == 0) {
			s_connHandle = event->connect.conn_handle;
			s_connected  = true;
			ESP_LOGI(TAG, "connected (handle %d)", s_connHandle);
			if (s_connCb) {
				s_connCb(true);
			}
		} else {
			ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
			bleStartAdvertising();
		}
		break;

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
		s_connected  = false;
		s_subscribed = false;
		s_connHandle = BLE_HS_CONN_HANDLE_NONE;
		if (s_connCb) {
			s_connCb(false);
		}
		bleStartAdvertising();
		break;

	case BLE_GAP_EVENT_ADV_COMPLETE:
		bleStartAdvertising();
		break;

	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle == s_txHandle) {
			s_subscribed = event->subscribe.cur_notify;
			ESP_LOGI(TAG, "TX notify %s", s_subscribed ? "on" : "off");
		}
		break;

	case BLE_GAP_EVENT_MTU:
		ESP_LOGI(TAG, "MTU %d", event->mtu.value);
		break;

	default:
		break;
	}
	return 0;
}

static void bleOnSync(void)
{
	int rc = ble_hs_util_ensure_addr(0);
	if (rc == 0) {
		rc = ble_hs_id_infer_auto(0, &s_ownAddrType);
	}
	if (rc != 0) {
		ESP_LOGE(TAG, "address setup failed: %d", rc);
		return;
	}
	s_synced = true;
	bleStartAdvertising();
}

static void bleOnReset(int reason)
{
	s_synced = false;
	ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void bleHostTask(void *param)
{
	nimble_port_run();              /* returns only on nimble_port_stop() */
	nimble_port_freertos_deinit();
}
