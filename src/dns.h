#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Captive-portal DNS server (UDP 53): every A query is answered with one
 * address (the AP's own), so any name a client looks up lands on the board.
 * Other query types get an empty NOERROR answer.
 */

/* Start answering with ip (network byte order). */
esp_err_t dnsStart(uint32_t ip);

/* Stop the server; waits for its task to exit. */
void dnsStop(void);

/* Build the reply to query q (qlen bytes) into out. Returns the reply length,
 * or 0 if the packet should be ignored. Pure function (host-testable). */
size_t dnsBuildReply(const uint8_t *q, size_t qlen, uint8_t *out, size_t outMax, uint32_t ip);
