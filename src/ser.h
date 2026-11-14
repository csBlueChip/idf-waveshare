#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * The two serial ports, used by the UART / USB user interfaces and by the
 * log output (log.c).
 *   SER_UART   UART0 on the header, 8N1; pins from NV uart.tx / uart.rx at
 *              boot (default TX GPIO43, RX GPIO44; the chip's own start-up
 *              messages always come out on GPIO43)
 *   SER_USB    the chip's USB-Serial/JTAG port (USB-C)
 *
 * Both drivers are installed by serInit() and stay installed; the console
 * (stdout, boot messages) keeps working through them. One task reads both
 * ports and hands the bytes to the receive callback.
 */

#define SER_LOG_BAUD		115200	/* UART speed when it is not the UI */

typedef enum {
	SER_UART = 0,
	SER_USB,
	SER_COUNT
} serPort_t;

/* Received bytes (len > 0), or a tick about every 20 ms (len == 0, data NULL)
 * for time-outs. Runs in the serial task. */
typedef void (*serRx_t)(serPort_t port, const uint8_t *data, size_t len);

/* Install both drivers, start the receive task. Call early (before logInit()),
 * after nvInit() (UART pins). */
esp_err_t serInit(serRx_t rx);

/* UART pins in use (NV uart.tx / uart.rx as read at boot). */
void serUartPins(int *tx, int *rx);

/* Write len bytes; with crlf, every '\n' goes out as "\r\n". The whole call is
 * one block on that port (calls from other tasks never land in the middle).
 * Bytes for an unplugged USB port are dropped. */
void serWrite(serPort_t port, const char *data, size_t len, bool crlf);

/* UART speed (UART UI: NV uart.baud; otherwise SER_LOG_BAUD). */
esp_err_t serSetBaud(uint32_t baud);
