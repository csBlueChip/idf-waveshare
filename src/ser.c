#include "ser.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "nv.h"

static const char *TAG = "ser";

#define SER_UART_NUM		UART_NUM_0
#define SER_ROM_TX_GPIO		43	/* UART0 pins the chip boots with */
#define SER_ROM_RX_GPIO		44
#define SER_UART_RX_BUF		1024
#define SER_UART_TX_BUF		4096
#define SER_USB_RX_BUF		512
#define SER_USB_TX_BUF		4096
#define SER_READ_MS		10	/* per port per loop: ~20 ms tick */
#define SER_LOCK_MS		1000	/* give up a write if a port stays busy */
#define SER_USB_WRITE_MS	50	/* host not reading: drop instead of blocking */
#define SER_TASK_STACK		4096
#define SER_TASK_PRIO		5

static SemaphoreHandle_t s_lock[SER_COUNT];	/* recursive: a log line from inside a write */
static serRx_t           s_rx;
static int               s_txPin, s_rxPin;	/* NV uart.tx / uart.rx at boot */

static void serTask(void *arg);
static bool serPut(serPort_t port, const char *data, size_t len);

/* ================= Public API ================= */

esp_err_t serInit(serRx_t rx)
{
	s_rx = rx;
	for (int i = 0; i < SER_COUNT; i++) {
		s_lock[i] = xSemaphoreCreateRecursiveMutex();
		ESP_RETURN_ON_FALSE(s_lock[i], ESP_ERR_NO_MEM, TAG, "lock");
	}

	ESP_RETURN_ON_ERROR(uart_driver_install(SER_UART_NUM, SER_UART_RX_BUF, SER_UART_TX_BUF, 0, NULL, 0),
	                    TAG, "uart");
	s_txPin = nvGetInt("uart.tx");
	s_rxPin = nvGetInt("uart.rx");
	if (s_txPin != SER_ROM_TX_GPIO) {
		gpio_reset_pin(SER_ROM_TX_GPIO);		/* moved: free the ROM console pins */
	}
	if (s_rxPin != SER_ROM_RX_GPIO) {
		gpio_reset_pin(SER_ROM_RX_GPIO);
	}
	ESP_RETURN_ON_ERROR(uart_set_pin(SER_UART_NUM, s_txPin, s_rxPin,
	                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
	gpio_pullup_en(s_rxPin);				/* idle high when nothing is plugged in */
	uart_vfs_dev_use_driver(SER_UART_NUM);			/* console through the driver */

	usb_serial_jtag_driver_config_t usb = {
		.tx_buffer_size = SER_USB_TX_BUF,
		.rx_buffer_size = SER_USB_RX_BUF,
	};
	ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb), TAG, "usb");
	usb_serial_jtag_vfs_use_driver();

	ESP_RETURN_ON_FALSE(xTaskCreate(serTask, "ser", SER_TASK_STACK, NULL, SER_TASK_PRIO, NULL) == pdPASS,
	                    ESP_ERR_NO_MEM, TAG, "task");
	return ESP_OK;
}

void serUartPins(int *tx, int *rx)
{
	*tx = s_txPin;
	*rx = s_rxPin;
}

void serWrite(serPort_t port, const char *data, size_t len, bool crlf)
{
	if (port >= SER_COUNT || len == 0) {
		return;
	}
	if (port == SER_USB && !usb_serial_jtag_is_connected()) {
		return;						/* no host: drop */
	}
	if (xSemaphoreTakeRecursive(s_lock[port], pdMS_TO_TICKS(SER_LOCK_MS)) != pdTRUE) {
		return;
	}
	if (!crlf) {
		serPut(port, data, len);
	} else {
		const char *end = data + len;
		while (data < end) {
			const char *nl  = memchr(data, '\n', end - data);
			size_t      seg = (nl ? nl : end) - data;
			if (seg > 0 && !serPut(port, data, seg)) {
				break;
			}
			if (!nl) {
				break;
			}
			if (!serPut(port, "\r\n", 2)) {
				break;
			}
			data = nl + 1;
		}
	}
	xSemaphoreGiveRecursive(s_lock[port]);
}

esp_err_t serSetBaud(uint32_t baud)
{
	uart_wait_tx_done(SER_UART_NUM, pdMS_TO_TICKS(500));	/* let pending output go at the old speed */
	return uart_set_baudrate(SER_UART_NUM, baud);
}

/* ================= Internals ================= */

/* false = USB host not taking data (rest of the write dropped). */
static bool serPut(serPort_t port, const char *data, size_t len)
{
	if (port == SER_UART) {
		return uart_write_bytes(SER_UART_NUM, data, len) == (int)len;
	}
	return usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(SER_USB_WRITE_MS)) == (int)len;
}

static void serTask(void *arg)
{
	uint8_t buf[64];

	for (;;) {
		int n = uart_read_bytes(SER_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(SER_READ_MS));
		if (n > 0) {
			s_rx(SER_UART, buf, n);
		}
		n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(SER_READ_MS));
		if (n > 0) {
			s_rx(SER_USB, buf, n);
		}
		s_rx(SER_UART, NULL, 0);				/* tick */
	}
}
