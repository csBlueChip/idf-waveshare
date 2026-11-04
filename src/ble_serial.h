#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/*
 * BLE Serial - Nordic UART Service (NUS) over NimBLE
 *   Service : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX      : 6E400002-...  (Write / Write-No-Rsp, client -> device)
 *   TX      : 6E400003-...  (Notify,              device -> client)
 */

#define BLE_RX_BUF_SIZE   1024   /* bytes buffered between BLE host and reader */

/* Callbacks run in the NimBLE host task - keep them short. */
typedef void (*bleRxCb_t)(const uint8_t *data, size_t len);
typedef void (*bleConnCb_t)(bool connected);

/* Init NimBLE, register NUS, start host task. Advertising starts automatically on sync.
 * onRxCb: received bytes go here (if NULL they are buffered for bleReceive()).
 * onConnCb: optional connect/disconnect notification. */
esp_err_t bleInit(bleRxCb_t onRxCb, bleConnCb_t onConnCb);

/* (Re)start advertising. Called internally on sync / disconnect. */
esp_err_t bleStartAdvertising(void);

/* Disconnect the client and stop advertising until bleEnable(). */
void bleDisable(void);

/* Allow advertising again (starts it now if the stack is ready). */
void bleEnable(void);

/* True while a client is connected. */
bool bleIsConnected(void);

/* Send bytes via TX notifications (split by MTU). Requires client subscribed to TX. */
esp_err_t bleSend(const uint8_t *data, size_t len);

/* Blocking read of received bytes (only when bleInit had no onRxCb). Returns bytes read (0 on timeout). */
size_t bleReceive(uint8_t *buf, size_t max, TickType_t timeout);
