#include "telnet.h"
#include "nv.h"

#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_eventfd.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "telnet";

#define TELNET_TASK_STACK	4096
#define TELNET_TASK_PRIO	5
#define TELNET_RX_BUF		128
#define TELNET_STOP_WAIT_MS	2000

/* telnet protocol bytes */
#define T_IAC	255
#define T_DONT	254
#define T_WILL	251
#define T_SB	250
#define T_SE	240

typedef enum { IAC_DATA, IAC_CMD, IAC_OPT, IAC_SUB, IAC_SUB_IAC } iacState_t;

static int               s_listenFd = -1;
static int               s_clientFd = -1;
static int               s_wakeFd   = -1;
static volatile bool     s_running;
static volatile bool     s_dropReq;	/* telnetDropClient() pending */
static SemaphoreHandle_t s_txLock;
static SemaphoreHandle_t s_done;
static telnetRxCb_t      s_rxCb;
static telnetConnCb_t    s_connCb;
static iacState_t        s_iac;
static bool              s_lastCr;

static void   telnetTask(void *arg);
static void   telnetCloseClient(void);
static size_t telnetFilter(const uint8_t *in, size_t n, uint8_t *out);
static int    telnetSendAll(int fd, const void *data, size_t len);

/* ================= Public API ================= */

esp_err_t telnetStart(telnetRxCb_t rxCb, telnetConnCb_t connCb)
{
	static bool eventfdReady;

	if (s_running) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!s_txLock) {
		s_txLock = xSemaphoreCreateMutex();
		s_done   = xSemaphoreCreateBinary();
		if (!s_txLock || !s_done) {
			return ESP_ERR_NO_MEM;
		}
	}
	if (!eventfdReady) {
		esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
		ESP_RETURN_ON_ERROR(esp_vfs_eventfd_register(&cfg), TAG, "eventfd");
		eventfdReady = true;
	}

	s_rxCb   = rxCb;
	s_connCb = connCb;

	s_wakeFd = eventfd(0, 0);
	ESP_RETURN_ON_FALSE(s_wakeFd >= 0, ESP_FAIL, TAG, "eventfd()");

	s_listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s_listenFd < 0) {
		close(s_wakeFd);
		return ESP_FAIL;
	}

	int one = 1;
	setsockopt(s_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_port        = htons(nvGetInt("telnet.port")),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(s_listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s_listenFd, 1) != 0) {
		ESP_LOGE(TAG, "bind/listen failed: errno %d", errno);
		close(s_listenFd);
		close(s_wakeFd);
		s_listenFd = s_wakeFd = -1;
		return ESP_FAIL;
	}

	s_running = true;
	if (xTaskCreate(telnetTask, "telnet", TELNET_TASK_STACK, NULL, TELNET_TASK_PRIO, NULL) != pdPASS) {
		s_running = false;
		close(s_listenFd);
		close(s_wakeFd);
		s_listenFd = s_wakeFd = -1;
		return ESP_ERR_NO_MEM;
	}

	ESP_LOGI(TAG, "listening on port %d", nvGetInt("telnet.port"));
	return ESP_OK;
}

esp_err_t telnetStop(void)
{
	if (!s_running) {
		return ESP_OK;
	}
	s_running = false;

	uint64_t wake = 1;
	write(s_wakeFd, &wake, sizeof(wake));		/* unblock select() */

	if (xSemaphoreTake(s_done, pdMS_TO_TICKS(TELNET_STOP_WAIT_MS)) != pdTRUE) {
		ESP_LOGW(TAG, "server task did not stop");
		return ESP_ERR_TIMEOUT;
	}
	ESP_LOGI(TAG, "stopped");
	return ESP_OK;
}

void telnetDropClient(void)
{
	if (!s_running) {
		return;
	}
	s_dropReq = true;
	uint64_t wake = 1;
	write(s_wakeFd, &wake, sizeof(wake));		/* the server task closes it */
}

esp_err_t telnetSend(const uint8_t *data, size_t len)
{
	esp_err_t err = ESP_OK;

	xSemaphoreTake(s_txLock, portMAX_DELAY);
	if (s_clientFd < 0) {
		err = ESP_ERR_INVALID_STATE;
	} else {
		/* send runs between newlines as-is, each "\n" as "\r\n" */
		size_t start = 0;
		for (size_t i = 0; i <= len && err == ESP_OK; i++) {
			if (i == len || data[i] == '\n') {
				if (i > start && telnetSendAll(s_clientFd, data + start, i - start) != 0) {
					err = ESP_FAIL;
				}
				if (i < len && err == ESP_OK && telnetSendAll(s_clientFd, "\r\n", 2) != 0) {
					err = ESP_FAIL;
				}
				start = i + 1;
			}
		}
	}
	xSemaphoreGive(s_txLock);
	return err;
}

/* ================= Internals ================= */

static void telnetTask(void *arg)
{
	uint8_t rx[TELNET_RX_BUF];
	uint8_t clean[TELNET_RX_BUF];

	while (s_running) {
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s_listenFd, &rd);
		FD_SET(s_wakeFd, &rd);
		int maxFd = (s_listenFd > s_wakeFd) ? s_listenFd : s_wakeFd;
		if (s_clientFd >= 0) {
			FD_SET(s_clientFd, &rd);
			if (s_clientFd > maxFd) {
				maxFd = s_clientFd;
			}
		}

		if (select(maxFd + 1, &rd, NULL, NULL, NULL) < 0) {	/* blocks until activity */
			ESP_LOGE(TAG, "select failed: errno %d", errno);
			break;
		}
		if (FD_ISSET(s_wakeFd, &rd)) {
			uint64_t v;
			read(s_wakeFd, &v, sizeof(v));		/* clear */
			if (!s_running) {
				break;				/* telnetStop() */
			}
			if (s_dropReq) {			/* telnetDropClient() */
				s_dropReq = false;
				if (s_clientFd >= 0) {
					telnetCloseClient();
					ESP_LOGI(TAG, "client dropped");
					if (s_connCb) {
						s_connCb(false);
					}
				}
			}
			continue;
		}

		if (FD_ISSET(s_listenFd, &rd)) {
			int fd = accept(s_listenFd, NULL, NULL);
			if (fd >= 0 && s_clientFd >= 0) {
				ESP_LOGW(TAG, "second client rejected");
				close(fd);
			} else if (fd >= 0) {
				int one = 1;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
				xSemaphoreTake(s_txLock, portMAX_DELAY);
				s_clientFd = fd;
				xSemaphoreGive(s_txLock);
				s_iac    = IAC_DATA;
				s_lastCr = false;
				ESP_LOGI(TAG, "client connected");
				if (s_connCb) {
					s_connCb(true);
				}
			}
		}

		if (s_clientFd >= 0 && FD_ISSET(s_clientFd, &rd)) {
			int n = recv(s_clientFd, rx, sizeof(rx), 0);
			if (n <= 0) {
				telnetCloseClient();
				ESP_LOGI(TAG, "client disconnected");
				if (s_connCb) {
					s_connCb(false);
				}
			} else {
				size_t m = telnetFilter(rx, n, clean);
				if (m && s_rxCb) {
					s_rxCb(clean, m);
				}
			}
		}
	}

	telnetCloseClient();
	close(s_listenFd);
	close(s_wakeFd);
	s_listenFd = s_wakeFd = -1;
	s_running  = false;
	xSemaphoreGive(s_done);
	vTaskDelete(NULL);
}

static void telnetCloseClient(void)
{
	xSemaphoreTake(s_txLock, portMAX_DELAY);
	if (s_clientFd >= 0) {
		close(s_clientFd);
		s_clientFd = -1;
	}
	xSemaphoreGive(s_txLock);
}

/* Strip telnet IAC sequences and NULs. "\r\0" (bare CR) is turned into "\n". */
static size_t telnetFilter(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t m = 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t b = in[i];

		switch (s_iac) {
		case IAC_DATA:
			if (b == T_IAC) {
				s_iac = IAC_CMD;
			} else if (b == 0) {
				if (s_lastCr) {
					out[m++] = '\n';
				}
			} else {
				out[m++] = b;
			}
			s_lastCr = (b == '\r');
			break;
		case IAC_CMD:
			if (b >= T_WILL && b <= T_DONT) {
				s_iac = IAC_OPT;		/* option byte follows */
			} else if (b == T_SB) {
				s_iac = IAC_SUB;
			} else {
				s_iac = IAC_DATA;		/* 2-byte command (or escaped 0xFF): drop */
			}
			break;
		case IAC_OPT:
			s_iac = IAC_DATA;
			break;
		case IAC_SUB:
			if (b == T_IAC) {
				s_iac = IAC_SUB_IAC;
			}
			break;
		case IAC_SUB_IAC:
			s_iac = (b == T_SE) ? IAC_DATA : IAC_SUB;
			break;
		}
	}
	return m;
}

static int telnetSendAll(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len > 0) {
		int n = send(fd, p, len, 0);
		if (n <= 0) {
			return -1;
		}
		p   += n;
		len -= n;
	}
	return 0;
}
