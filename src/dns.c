#include "dns.h"

#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"

static const char *TAG = "dns";

#define DNS_PORT		53
#define DNS_PKT_MAX		512
#define DNS_HDR			12
#define DNS_TTL			60
#define DNS_TASK_STACK		3072
#define DNS_TASK_PRIO		5
#define DNS_POLL_MS		500	/* recv timeout: how often the task checks for stop */

static volatile bool     s_run;
static uint32_t          s_ip;
static SemaphoreHandle_t s_done;

static void dnsTask(void *arg);

/* ================= Public API ================= */

esp_err_t dnsStart(uint32_t ip)
{
	if (s_run) {
		return ESP_OK;
	}
	if (!s_done && !(s_done = xSemaphoreCreateBinary())) {
		return ESP_ERR_NO_MEM;
	}
	s_ip  = ip;
	s_run = true;
	if (xTaskCreate(dnsTask, "dns", DNS_TASK_STACK, NULL, DNS_TASK_PRIO, NULL) != pdPASS) {
		s_run = false;
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

void dnsStop(void)
{
	if (!s_run) {
		return;
	}
	s_run = false;
	xSemaphoreTake(s_done, portMAX_DELAY);
}

size_t dnsBuildReply(const uint8_t *q, size_t qlen, uint8_t *out, size_t outMax, uint32_t ip)
{
	if (qlen < DNS_HDR || qlen > outMax) {
		return 0;
	}
	if ((q[2] & 0x80) || (q[2] & 0x78) || q[4] != 0 || q[5] == 0) {
		return 0;					/* a response, not a standard query, or no question */
	}

	/* walk the first question name: labels up to a 0 byte (no compression in queries) */
	size_t p = DNS_HDR;
	while (p < qlen && q[p] != 0) {
		if (q[p] & 0xC0) {
			return 0;
		}
		p += q[p] + 1;
	}
	if (p + 5 > qlen) {
		return 0;					/* name + type + class must fit */
	}
	size_t   qend  = p + 5;
	uint16_t qtype = (q[p + 1] << 8) | q[p + 2];
	uint16_t qcls  = (q[p + 3] << 8) | q[p + 4];
	bool     answer = (qtype == 1 && qcls == 1);	/* A, IN */

	size_t len = qend + (answer ? 16 : 0);
	if (len > outMax) {
		return 0;
	}

	memcpy(out, q, qend);				/* header + first question only */
	out[2]  = 0x80 | (q[2] & 0x01);			/* QR, keep RD */
	out[3]  = 0x80;					/* RA, NOERROR */
	out[4]  = 0; out[5]  = 1;			/* QDCOUNT */
	out[6]  = 0; out[7]  = answer ? 1 : 0;		/* ANCOUNT */
	out[8]  = 0; out[9]  = 0;			/* NSCOUNT */
	out[10] = 0; out[11] = 0;			/* ARCOUNT */

	if (answer) {
		uint8_t *a = out + qend;
		const uint8_t *ipb = (const uint8_t *)&ip;	/* network order */
		a[0] = 0xC0; a[1] = DNS_HDR;			/* name: pointer to the question */
		a[2] = 0;    a[3] = 1;				/* type A */
		a[4] = 0;    a[5] = 1;				/* class IN */
		a[6] = 0;    a[7] = 0; a[8] = 0; a[9] = DNS_TTL;
		a[10] = 0;   a[11] = 4;				/* RDLENGTH */
		memcpy(a + 12, ipb, 4);
	}
	return len;
}

/* ================= Internals ================= */

static void dnsTask(void *arg)
{
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock >= 0) {
		struct sockaddr_in addr = {
			.sin_family      = AF_INET,
			.sin_port        = htons(DNS_PORT),
			.sin_addr.s_addr = htonl(INADDR_ANY),
		};
		struct timeval tv = { .tv_sec = 0, .tv_usec = DNS_POLL_MS * 1000 };
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
			ESP_LOGE(TAG, "bind failed");
			close(sock);
			sock = -1;
		}
	}
	if (sock < 0) {
		ESP_LOGE(TAG, "not started");
	} else {
		ESP_LOGI(TAG, "answering all names");
	}

	static EXT_RAM_BSS_ATTR uint8_t in[DNS_PKT_MAX], out[DNS_PKT_MAX];
	while (s_run && sock >= 0) {
		struct sockaddr_in from;
		socklen_t          fromLen = sizeof(from);
		int n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&from, &fromLen);
		if (n <= 0) {
			continue;				/* timeout: check s_run */
		}
		size_t len = dnsBuildReply(in, n, out, sizeof(out), s_ip);
		if (len) {
			sendto(sock, out, len, 0, (struct sockaddr *)&from, fromLen);
		}
	}

	if (sock >= 0) {
		close(sock);
	}
	if (!s_run) {
		xSemaphoreGive(s_done);
	} else {
		s_run = false;					/* failed to start: nobody waits */
	}
	vTaskDelete(NULL);
}
