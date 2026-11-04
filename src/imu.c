#include "imu.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "i2c.h"
#include "cmd.h"
#include "log.h"
#include "script.h"
#include "lcd.h"
#include "led.h"
#include "nv.h"
#include "sd.h"
#include "sys.h"
#include "ui.h"

static const char *TAG = "imu";

/* ---------- QMI8658A registers ---------- */
#define IMU_ADDR		0x6B
#define IMU_CHIP_ID		0x05

#define IMU_REG_WHO_AM_I	0x00
#define IMU_REG_CTRL1		0x02	/* interface, INT pins */
#define IMU_REG_CTRL2		0x03	/* accel: self-test, range, rate */
#define IMU_REG_CTRL3		0x04	/* gyro: self-test, range, rate */
#define IMU_REG_CTRL7		0x08	/* sensor enable */
#define IMU_REG_CTRL8		0x09	/* motion engines */
#define IMU_REG_CTRL9		0x0A	/* host command */
#define IMU_REG_CAL1_L		0x0B	/* CAL1_L .. CAL4_H: 8 bytes */
#define IMU_REG_FIFO_CTRL	0x14
#define IMU_REG_FIFO_CNT	0x15	/* FIFO_SMPL_CNT, then FIFO_STATUS */
#define IMU_REG_FIFO_DATA	0x17
#define IMU_REG_STATUSINT	0x2D
#define IMU_REG_STATUS1		0x2F	/* events; read clears WoM */
#define IMU_REG_TEMP_L		0x33
#define IMU_REG_AX_L		0x35	/* AX_L..AZ_H: 6 bytes */
#define IMU_REG_GX_L		0x3B	/* GX_L..GZ_H: 6 bytes */
#define IMU_REG_COD_STATUS	0x46
#define IMU_REG_DVX_L		0x51	/* self-test results: 6 bytes */
#define IMU_REG_TAP_STATUS	0x59
#define IMU_REG_STEP_CNT	0x5A	/* 3 bytes */
#define IMU_REG_RESET		0x60

#define IMU_CTRL1_ADDR_AI	0x40	/* auto-increment, little endian */
#define IMU_CTRL1_INT1_EN	0x08	/* INT1 push-pull (WoM wake line) */
#define IMU_CTRL_ST		0x80	/* self-test bit (CTRL2 aST / CTRL3 gST) */
#define IMU_CTRL7_AG		0x03	/* aEN | gEN */
#define IMU_CTRL7_A		0x01
#define IMU_CTRL8_BASE		0xC0	/* CTRL9 handshake via STATUSINT.7, events on INT1 */
#define IMU_CTRL8_PEDO		0x10

/* The chip's pedometer never counted on the board (no STATUS1 step flag at
 * 56 or 224 Hz, any imu.step.min; configuration checked against the
 * datasheet). 0 = IMU STEP replies "not implemented" and the pedometer engine
 * is never enabled; the code is kept for a later fix. */
#define IMU_STEP_ENABLE		0

/* The chip's significant-motion engine times its window from the first
 * any-motion and quiet does not restart it, so "move 1 s, rest 4 s, move"
 * fired SIGMO at once (board, SIGMO 4000). 0 = SIGMO is timed here from the
 * start of a motion session (as MOTION); the chip engine stays off, its
 * code is kept. */
#define IMU_SIGMO_CHIP		0
#define IMU_SIGMO_MAX		60000	/* firmware SIGMO limit, ms */

/* The chip's tap engine reports nothing while any other motion engine runs
 * (seen with IMU DEBUG), so TAP and MOTION/STILL/SIGMO/STEP are exclusive. */
#define IMU_TAP_ON()		(s_tapEn[0] || s_tapEn[1] || s_tapEn[2])
#define IMU_OTHERS_ON()		(s_motion || s_stillMs || s_sigmoMs || s_step)

#define IMU_CTRL8_SIG		0x08
#define IMU_CTRL8_NO		0x04
#define IMU_CTRL8_ANY		0x02
#define IMU_CTRL8_TAP		0x01
#define IMU_CTRL8_ENGINES	0x1F

#define IMU_ST1_SIG		0x80	/* STATUS1 bits */
#define IMU_ST1_NO		0x40
#define IMU_ST1_ANY		0x20
#define IMU_ST1_WOM		0x04
#define IMU_ST1_TAP		0x02

#define IMU_CMD_ACK		0x00	/* CTRL9 commands */
#define IMU_CMD_RST_FIFO	0x04
#define IMU_CMD_REQ_FIFO	0x05
#define IMU_CMD_WOM		0x08
#define IMU_CMD_TAP		0x0C
#define IMU_CMD_PEDO		0x0D
#define IMU_CMD_MOTION		0x0E
#define IMU_CMD_RESET_PEDO	0x0F
#define IMU_CMD_COD		0xA2

#define IMU_FIFO_STREAM_128	0x0E	/* FIFO_CTRL: 128 samples, stream mode */
#define IMU_FIFO_OVFLOW		0x20	/* FIFO_STATUS */
#define IMU_ODR_BASE		7174.4f	/* 6DOF ODR = base / 2^code */
#define IMU_LP_21HZ		0x0D	/* accel low-power 21 Hz (WoM) */
#define IMU_WOM_BLANK		4	/* samples ignored after entering WoM */
#define IMU_WOM_INT1_HIGH	0x80	/* CAL1_H: INT1, initial level 1 (event -> 0) */
#define IMU_TEMP_LSB_PER_C	256.0f

/* ---------- Timing ---------- */
#define IMU_STARTUP_MS		60	/* gyro start after enabling */
#define IMU_RESET_MS		20	/* soft reset (max 15 ms) */
#define IMU_CTRL9_MS		100	/* CTRL9 command timeout */
#define IMU_COD_MS		3000	/* calibration: ~1.5 s */
#define IMU_ST_MS		2000	/* self-test: accel ~25 ODRs, gyro ~400 ms */
#define IMU_POLL_MS		20	/* event task period */
#define IMU_REARM_MS		1000	/* MOTION re-arm quiet time when STILL is off */
#define IMU_SLEEP_DELAY_MS	300	/* let OK reach the client before RAD/CPU sleep */

/* ---------- Pins (schematic: IMU_INT1 = IO13, IMU_INT2 = IO12) ---------- */
#define IMU_PIN_INT1		GPIO_NUM_13
#define IMU_PIN_BOOT		GPIO_NUM_0

/* ---------- Limits ---------- */
#define IMU_STILL_MAX		60000
#define IMU_SIGMO_MIN		100
#define IMU_STEP_AUTO_MAX	10000
#define IMU_REC_BLE_MS		3000
#define IMU_REC_UI_MS		60000
#define IMU_REC_FILE_MS		600000
#define IMU_REC_DIR		SD_MOUNT_POINT "/imu"
#define IMU_REC_EXT		".csv"
#define IMU_REC_HEADER		"t ax ay az gx gy gz\n"
#define IMU_REC_LINE		72
#define IMU_FIFO_SAMPLES	128
#define IMU_TASK_STACK		6144	/* RAD wake restarts Wi-Fi from this task */
#define IMU_TASK_PRIO		4

/* ---------- Orientation ----------
 * Physical ROT -> accelerometer axis (0 X, 1 Y) and the sign that reads
 * +1 g when the screen is upright in that ROT (SCR FLIP not set).
 * Checked on the board: ROT 0..3 all report correctly. */
static const struct { uint8_t axis; int8_t sign; } s_orientMap[4] = {
	{ 1, -1 },	/* ROT 0: -Y up (checked on the board) */
	{ 0, -1 },	/* ROT 1: -X up */
	{ 1, +1 },	/* ROT 2: +Y up */
	{ 0, +1 },	/* ROT 3: +X up */
};
#define IMU_ORIENT_FLAT		4
#define IMU_ORIENT_NONE		-1
#define IMU_ORIENT_DOMINANCE	1.2f	/* major axis must beat the other by this (~50 deg) */

typedef enum { IMU_SLEEP_NONE = 0, IMU_SLEEP_SCR, IMU_SLEEP_RAD, IMU_SLEEP_CPU } imuSleep_t;

/* ---------- State ---------- */
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_lock;		/* every chip access sequence */

static int s_arange, s_arateCode, s_grange, s_grateCode;	/* from NV */

/* detectors: runtime only, off at boot */
static bool        s_tapEn[3];
static bool        s_motion;
static int         s_stillMs;
static int         s_sigmoMs;
static bool        s_sigmoRep;		/* SIGMO REPEAT: again every ms while moving */
static bool        s_step;
static int         s_stepAuto;
static imuOrMode_t s_orMode;
static bool        s_orAuto;

/* event state (IMU task) */
static int      s_tapN;
static int64_t  s_tapLast;
static bool     s_moving;
static int64_t  s_quietSince;		/* 0 = no quiet seen since the last motion */
static bool     s_sigSeen;		/* SIGMO reported in this motion session */
static int64_t  s_moveStart;		/* start of the current motion session */
static uint32_t s_stepBase;		/* steps counted before the last engine restart */
static uint32_t s_stepNext;
static int      s_orCand = IMU_ORIENT_NONE;
static int64_t  s_orSince;
static volatile bool s_dbg;		/* IMU DEBUG: log raw status changes (this boot) */
static uint16_t      s_dbgLast;		/* last logged STATUS1 << 8 | TAP_STATUS */
static int      s_orShown = IMU_ORIENT_NONE;

static volatile imuSleep_t s_sleepReq;
static volatile int        s_sleepThr;
static volatile bool       s_asleep;

/* ================= Prototypes ================= */

static esp_err_t   imuWrite(uint8_t reg, uint8_t val);
static esp_err_t   imuRead(uint8_t reg, uint8_t *buf, size_t len);
static esp_err_t   imuCtrl9(uint8_t cmd, const uint8_t cal[8], int timeoutMs);
static esp_err_t   imuReadVec(uint8_t reg, float scale, float *x, float *y, float *z);
static esp_err_t   imuSoftReset(void);
static void        imuLoadNv(void);
static float       imuOdr(void);
static int         imuSamples(float odr, int ms, int min, int max);
static esp_err_t   imuApply(void);
static uint8_t     imuCtrl8(void);
static esp_err_t   imuSetCtrl8(void);
static esp_err_t   imuEngines(void);
static void        imuStepSave(void);
static uint32_t    imuStepChip(void);
static uint32_t    imuSteps(void);
static bool        imuBusy(void);
static void        imuEvent(const char *text);
static void        imuTask(void *arg);
static void        imuPoll(int64_t now);
static void        imuTapOne(int64_t now);
static void        imuOrient(int64_t now, float x, float y, float z);
static void        imuAutoFlip(int rot);
static esp_err_t   imuWomEnter(int thr);
static esp_err_t   imuWomExit(void);
static void        imuSleepRun(imuSleep_t level, int thr);
static void        imuDeepSleep(void);
static const char *imuCmdAcc(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdGyr(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdTemp(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmd(int argc, char *argv[], cmdReply_t reply);
static void        imuStatus(cmdReply_t reply);
static const char *imuCmdRange(bool acc, int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdTap(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdStep(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdOrient(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdSet(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdSleep(int argc, char *argv[], cmdReply_t reply);
static const char *imuCmdCal(cmdReply_t reply);
static const char *imuCmdTest(cmdReply_t reply);
static const char *imuCmdRec(int argc, char *argv[], cmdReply_t reply);

/* ================= Public API ================= */

esp_err_t imuInit(void)
{
	s_lock = xSemaphoreCreateMutex();
	ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
	ESP_RETURN_ON_ERROR(i2cAddDevice(IMU_ADDR, &s_dev), TAG, "add device");

	uint8_t id = 0;
	ESP_RETURN_ON_ERROR(imuRead(IMU_REG_WHO_AM_I, &id, 1), TAG, "read id");
	if (id != IMU_CHIP_ID) {
		ESP_LOGE(TAG, "unexpected chip id 0x%02X", id);
		return ESP_ERR_NOT_FOUND;
	}
	ESP_RETURN_ON_ERROR(imuSoftReset(), TAG, "reset");	/* also leaves WoM after deep sleep */
	imuLoadNv();
	ESP_RETURN_ON_ERROR(imuApply(), TAG, "configure");

	ESP_RETURN_ON_FALSE(xTaskCreate(imuTask, "imu", IMU_TASK_STACK, NULL, IMU_TASK_PRIO, NULL) == pdPASS,
	                    ESP_ERR_NO_MEM, TAG, "task");

	ESP_RETURN_ON_ERROR(cmdRegister("ACC", 0, 0, imuCmdAcc, "ACC  accelerometer aX,aY,aZ (g)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("GYR", 0, 0, imuCmdGyr, "GYR  gyroscope gX,gY,gZ (dps)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("TEMP", 0, 0, imuCmdTemp, "TEMP  temperature (^C)"), TAG, "cmd");
	ESP_RETURN_ON_ERROR(cmdRegister("IMU", 0, 4, imuCmd,
	                    "IMU [ACC|GYR|RESET|TAP|MOTION|STILL|SIGMO|STEP|ORIENT|SET|WOM|SLEEP|CAL|TEST|REC ...]"),
	                    TAG, "cmd");

	ESP_LOGI(TAG, "QMI8658 ready");
	return ESP_OK;
}

esp_err_t imuReadAccel(float *x, float *y, float *z)
{
	return imuReadVec(IMU_REG_AX_L, s_arange / 32768.0f, x, y, z);
}

esp_err_t imuReadGyro(float *x, float *y, float *z)
{
	return imuReadVec(IMU_REG_GX_L, s_grange / 32768.0f, x, y, z);
}

esp_err_t imuReadTemp(float *c)
{
	uint8_t raw[2];
	ESP_RETURN_ON_ERROR(imuRead(IMU_REG_TEMP_L, raw, sizeof(raw)), TAG, "read temp");
	*c = (int16_t)(raw[0] | (raw[1] << 8)) / IMU_TEMP_LSB_PER_C;
	return ESP_OK;
}

/* ================= Chip access ================= */

static esp_err_t imuWrite(uint8_t reg, uint8_t val)
{
	return i2cWriteReg(s_dev, reg, val);
}

static esp_err_t imuRead(uint8_t reg, uint8_t *buf, size_t len)
{
	return i2cReadReg(s_dev, reg, buf, len);
}

/* CTRL9 protocol: optional CAL1_L..CAL4_H, command, wait CmdDone (STATUSINT.7), ACK. */
static esp_err_t imuCtrl9(uint8_t cmd, const uint8_t cal[8], int timeoutMs)
{
	if (cal) {
		for (int i = 0; i < 8; i++) {			/* no burst writes to these */
			ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CAL1_L + i, cal[i]), TAG, "cal");
		}
	}
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL9, cmd), TAG, "ctrl9");

	int64_t   end  = esp_timer_get_time() + (int64_t)timeoutMs * 1000;
	uint8_t   st   = 0;
	esp_err_t done = ESP_ERR_TIMEOUT;
	while (esp_timer_get_time() < end) {
		if (imuRead(IMU_REG_STATUSINT, &st, 1) == ESP_OK && (st & 0x80)) {
			done = ESP_OK;
			break;
		}
		vTaskDelay(1);
	}
	imuWrite(IMU_REG_CTRL9, IMU_CMD_ACK);
	if (done != ESP_OK) {
		ESP_LOGW(TAG, "CTRL9 0x%02X timeout", cmd);
	}
	return done;
}

/* Three little-endian int16 starting at reg, scaled to units. */
static esp_err_t imuReadVec(uint8_t reg, float scale, float *x, float *y, float *z)
{
	uint8_t raw[6];
	ESP_RETURN_ON_ERROR(imuRead(reg, raw, sizeof(raw)), TAG, "read 0x%02X", reg);
	*x = (int16_t)(raw[0] | (raw[1] << 8)) * scale;
	*y = (int16_t)(raw[2] | (raw[3] << 8)) * scale;
	*z = (int16_t)(raw[4] | (raw[5] << 8)) * scale;
	return ESP_OK;
}

static esp_err_t imuSoftReset(void)
{
	imuWrite(IMU_REG_RESET, 0xB0);
	vTaskDelay(pdMS_TO_TICKS(IMU_RESET_MS));
	uint8_t id = 0;
	ESP_RETURN_ON_ERROR(imuRead(IMU_REG_WHO_AM_I, &id, 1), TAG, "read id");
	return (id == IMU_CHIP_ID) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ================= Configuration ================= */

static int imuLog2(int v)
{
	int n = 0;
	while (v > 1) {
		v >>= 1;
		n++;
	}
	return n;
}

/* Range/rate from NV; rate 896 -> code 3 ... 28 -> code 8. */
static void imuLoadNv(void)
{
	s_arange    = nvGetInt("imu.arange");
	s_grange    = nvGetInt("imu.grange");
	s_arateCode = 3 + imuLog2(896 / nvGetInt("imu.arate"));
	s_grateCode = 3 + imuLog2(896 / nvGetInt("imu.grate"));
}

/* Accelerometer output data rate in Hz (gyro is on: 6DOF table). */
static float imuOdr(void)
{
	return IMU_ODR_BASE / (float)(1 << s_arateCode);
}

static int imuSamples(float odr, int ms, int min, int max)
{
	int n = (int)lroundf(odr * ms / 1000.0f);
	return (n < min) ? min : (n > max) ? max : n;
}

static void imuPut16(uint8_t *p, int v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
}

/* Full configuration from NV + detector settings: sensors off, ranges/rates,
 * engine parameters (only accepted with sensors off), sensors on, engines.
 * Caller holds s_lock. */
static esp_err_t imuApply(void)
{
	float   odr = imuOdr();
	uint8_t cal[8];

	imuStepSave();
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL7, 0), TAG, "ctrl7");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL1, IMU_CTRL1_ADDR_AI | IMU_CTRL1_INT1_EN), TAG, "ctrl1");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL8, IMU_CTRL8_BASE), TAG, "ctrl8");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL2, (imuLog2(s_arange) - 1) << 4 | s_arateCode), TAG, "ctrl2");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL3, (imuLog2(s_grange) - 4) << 4 | s_grateCode), TAG, "ctrl3");
	imuWrite(IMU_REG_FIFO_CTRL, 0);				/* bypass */

	/* Motion: slope thresholds in 1/32 g; any = OR of X/Y/Z, no = AND */
	int any  = imuSamples(32.0f, nvGetInt("imu.mot.thr"), 1, 255);	/* mg -> 1/32 g */
	int none = imuSamples(32.0f, nvGetInt("imu.still.thr"), 1, 255);
	memset(cal, 0, sizeof(cal));
	cal[0] = cal[1] = cal[2] = any;
	cal[3] = cal[4] = cal[5] = none;
	cal[6] = 0xF7;						/* NoAND, NoZYX, AnyOR, AnyZYX */
	cal[7] = 0x01;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_MOTION, cal, IMU_CTRL9_MS), TAG, "motion 1");
	int sigMs = s_sigmoMs ? s_sigmoMs : 1000;
	memset(cal, 0, sizeof(cal));
	cal[0] = imuSamples(odr, 10, 1, 255);			/* any: 10 ms */
	cal[1] = imuSamples(odr, 100, 1, 255);			/* no: 100 ms */
	imuPut16(&cal[2], imuSamples(odr, sigMs, 1, 65535));	/* SIGMO wait */
	imuPut16(&cal[4], imuSamples(odr, 2 * sigMs, 2, 65535));	/* SIGMO confirm */
	cal[7] = 0x02;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_MOTION, cal, IMU_CTRL9_MS), TAG, "motion 2");

	/* Tap: the chip reports each tap quickly (double-tap timeout just past the
	 * quiet window); 2TAP/3TAP are counted here using imu.tap.gap. */
	int tapWin = imuSamples(odr, 100, 2, 65000);
	float thr  = nvGetFloat("imu.tap.thr");
	int peak   = (int)lroundf(thr * thr * 1024.0f);	/* g -> g^2 in 1/1024 */
	memset(cal, 0, sizeof(cal));
	cal[0] = imuSamples(odr, 40, 1, 255);			/* peak window 40 ms */
	cal[1] = 0;						/* priority X > Y > Z */
	imuPut16(&cal[2], tapWin);				/* quiet after a tap: 100 ms */
	imuPut16(&cal[4], tapWin + imuSamples(odr, 20, 1, 255));	/* double-tap timeout */
	cal[7] = 0x01;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_TAP, cal, IMU_CTRL9_MS), TAG, "tap 1");
	memset(cal, 0, sizeof(cal));
	cal[0] = 8;						/* alpha 0.0625 */
	cal[1] = 32;						/* gamma 0.25 */
	imuPut16(&cal[2], peak);
	imuPut16(&cal[4], peak / 2);				/* quiet threshold (datasheet ratio) */
	cal[7] = 0x02;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_TAP, cal, IMU_CTRL9_MS), TAG, "tap 2");

	/* Pedometer: datasheet example (50 Hz) scaled to the ODR */
	int p2p = (int)lroundf(nvGetInt("imu.step.thr") * 1.024f);	/* mg -> 1/1024 g */
	memset(cal, 0, sizeof(cal));
	imuPut16(&cal[0], imuSamples(odr, 1000, 1, 65535));	/* window 1 s */
	imuPut16(&cal[2], p2p);					/* peak-to-peak */
	imuPut16(&cal[4], p2p / 2);				/* peak */
	cal[7] = 0x01;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_PEDO, cal, IMU_CTRL9_MS), TAG, "pedo 1");
	memset(cal, 0, sizeof(cal));
	imuPut16(&cal[0], imuSamples(odr, 4000, 1, 65535));	/* slowest step 4 s */
	cal[2] = imuSamples(odr, 400, 1, 255);			/* fastest step 0.4 s */
	cal[3] = nvGetInt("imu.step.min");			/* steps before counting starts */
	cal[4] = 0;						/* precision */
	cal[5] = 1;						/* update every step */
	cal[7] = 0x02;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_PEDO, cal, IMU_CTRL9_MS), TAG, "pedo 2");

	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL7, IMU_CTRL7_AG), TAG, "ctrl7");
	vTaskDelay(pdMS_TO_TICKS(IMU_STARTUP_MS));
	return imuSetCtrl8();
}

/* Engine bits for the enabled detectors. */
static uint8_t imuCtrl8(void)
{
	uint8_t v = IMU_CTRL8_BASE;
	if (s_tapEn[0] || s_tapEn[1] || s_tapEn[2]) {
		v |= IMU_CTRL8_TAP;
	}
	if (s_motion || s_stillMs || s_sigmoMs) {
		v |= IMU_CTRL8_ANY | IMU_CTRL8_NO;
	}
	if (s_sigmoMs && IMU_SIGMO_CHIP) {
		v |= IMU_CTRL8_SIG;
	}
	if (s_step) {
		v |= IMU_CTRL8_PEDO;
	}
	return v;
}

/* Detector settings changed: if the set of engines changed, reconfigure the
 * whole chip with the sensors off (switching engines while the sensors run
 * can leave the tap engine stuck); otherwise leave the chip alone.
 * Caller holds s_lock. */
static esp_err_t imuEngines(void)
{
	uint8_t cur = 0;
	if (imuRead(IMU_REG_CTRL8, &cur, 1) == ESP_OK &&
	    (cur & IMU_CTRL8_ENGINES) == (imuCtrl8() & IMU_CTRL8_ENGINES)) {
		return ESP_OK;
	}
	return imuApply();
}

/* Write the engine bits (end of imuApply, sensors just enabled). Caller holds s_lock. */
static esp_err_t imuSetCtrl8(void)
{
	uint8_t v = imuCtrl8();
	if (s_dbg) {
		dbgf("IMU %lld CTRL8=%02X (chip reconfigured)", (long long)(esp_timer_get_time() / 1000), v);
	}
	if (!(v & IMU_CTRL8_PEDO)) {
		imuStepSave();					/* keep the count while off */
	}
	return imuWrite(IMU_REG_CTRL8, v);
}

/* Pedometer about to stop or restart (restart clears the chip count): add
 * the chip count to the base. Caller holds s_lock. */
static void imuStepSave(void)
{
	uint8_t c8 = 0;
	if (imuRead(IMU_REG_CTRL8, &c8, 1) == ESP_OK && (c8 & IMU_CTRL8_PEDO)) {
		s_stepBase += imuStepChip();
		imuWrite(IMU_REG_CTRL8, c8 & ~IMU_CTRL8_PEDO);
	}
}

static uint32_t imuStepChip(void)
{
	uint8_t raw[3] = { 0 };
	imuRead(IMU_REG_STEP_CNT, raw, sizeof(raw));
	return raw[0] | (raw[1] << 8) | ((uint32_t)raw[2] << 16);
}

/* Total steps. Caller holds s_lock. */
static uint32_t imuSteps(void)
{
	uint8_t c8 = 0;
	imuRead(IMU_REG_CTRL8, &c8, 1);
	return s_stepBase + ((c8 & IMU_CTRL8_PEDO) ? imuStepChip() : 0);
}

static bool imuBusy(void)
{
	return s_asleep || s_sleepReq != IMU_SLEEP_NONE;
}

/* ================= Event task ================= */

static void imuEvent(const char *text)
{
	char msg[40];
	snprintf(msg, sizeof(msg), "<<IMU %s>>\n", text);
	uiSend(msg);
	if (s_dbg) {
		dbgf("IMU %lld <<IMU %s>>", (long long)(esp_timer_get_time() / 1000), text);
	}
}

static void imuTask(void *arg)
{
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));

		if (s_sleepReq != IMU_SLEEP_NONE) {
			imuSleepRun(s_sleepReq, s_sleepThr);
			s_sleepReq = IMU_SLEEP_NONE;
			continue;
		}
		imuPoll(esp_timer_get_time() / 1000);
	}
}

/* Read events / accel for the enabled detectors and report. */
static void imuPoll(int64_t now)
{
	uint8_t  st = 0, tap = 0;
	float    x = 0, y = 0, z = 0;
	bool     haveAcc = false, haveSt = false;
	uint32_t steps = 0;

	xSemaphoreTake(s_lock, portMAX_DELAY);
	if ((imuCtrl8() & IMU_CTRL8_ENGINES) && imuRead(IMU_REG_STATUS1, &st, 1) == ESP_OK) {
		haveSt = true;
		if ((st & IMU_ST1_TAP) || s_dbg) {
			imuRead(IMU_REG_TAP_STATUS, &tap, 1);
		}
		if (s_dbg && (uint16_t)(st << 8 | tap) != s_dbgLast) {
			s_dbgLast = st << 8 | tap;
			dbgf("IMU %lld ST1=%02X TAP=%02X", (long long)now, st, tap);
		}
		if (s_step && s_stepAuto) {
			steps = imuSteps();
		}
	}
	if (s_orMode != IMU_OR_OFF) {
		haveAcc = (imuReadAccel(&x, &y, &z) == ESP_OK);
	}
	xSemaphoreGive(s_lock);

	if (haveSt) {
		int taps = (st & IMU_ST1_TAP) ? (tap & 0x03) : 0;	/* 1 single, 2 double */
		for (int i = 0; i < taps && i < 2; i++) {
			imuTapOne(now);
		}

		if (st & IMU_ST1_ANY) {				/* motion */
			s_quietSince = 0;
			if (!s_moving) {
				s_moving    = true;
				s_moveStart = now;
				if (!IMU_SIGMO_CHIP) {
					s_sigSeen = false;	/* new session */
				}
				if (s_motion) {
					imuEvent("MOTION");
				}
			}
		} else if ((st & IMU_ST1_NO) && s_moving && s_quietSince == 0) {
			s_quietSince = now;			/* quiet starts */
		}
		if (s_moving && s_quietSince) {
			int win = s_stillMs ? s_stillMs : IMU_REARM_MS;
			if (now - s_quietSince >= win) {
				s_moving = false;
				if (s_stillMs) {
					imuEvent("STILL");
				}
			}
		}

		if (!IMU_SIGMO_CHIP) {
			/* still moving sigmo ms after the session began: once per session */
			if (s_sigmoMs && s_moving && !s_sigSeen && (st & IMU_ST1_ANY) &&
			    now - s_moveStart >= s_sigmoMs) {
				if (s_sigmoRep) {
					s_moveStart = now;	/* REPEAT: time the next one from here */
				} else {
					s_sigSeen = true;
				}
				imuEvent("SIGMO");
			}
		} else if ((st & IMU_ST1_SIG) && !s_sigSeen) {	/* chip: once until quiet again */
			s_sigSeen = true;
			if (s_sigmoMs) {
				imuEvent("SIGMO");
			}
		} else if (st & IMU_ST1_NO) {
			s_sigSeen = false;
		}

		if (s_step && s_stepAuto && steps >= s_stepNext) {
			char text[24];
			snprintf(text, sizeof(text), "STEP %lu", (unsigned long)steps);
			imuEvent(text);
			s_stepNext = (steps / s_stepAuto + 1) * s_stepAuto;
		}
	}

	if (haveAcc) {
		imuOrient(now, x, y, z);
	}
}

/* One tap from the chip. Taps within imu.tap.gap of each other form a burst;
 * the n-th tap (1..3) is reported if that order is enabled. After the highest
 * enabled order the count starts again at 1. */
static void imuTapOne(int64_t now)
{
	static const char *names[3] = { "TAP", "2TAP", "3TAP" };
	int hi = s_tapEn[2] ? 3 : s_tapEn[1] ? 2 : s_tapEn[0] ? 1 : 0;
	if (hi == 0) {
		return;
	}
	if (now - s_tapLast > nvGetInt("imu.tap.gap") || s_tapN >= hi) {
		s_tapN = 0;
	}
	s_tapN++;
	s_tapLast = now;
	if (s_tapEn[s_tapN - 1]) {
		imuEvent(names[s_tapN - 1]);
	}
}

/* Orientation from gravity: FLAT, or the ROT whose "up" axis points up.
 * Reported once it has held for imu.orient.hold ms. */
static void imuOrient(int64_t now, float x, float y, float z)
{
	float mag = sqrtf(x * x + y * y + z * z);
	if (mag < 0.5f || mag > 1.5f) {
		return;						/* being moved: ignore */
	}

	int cand = s_orCand;
	if (fabsf(z) / mag > cosf(nvGetInt("imu.orient.flat") * (float)M_PI / 180.0f)) {
		cand = IMU_ORIENT_FLAT;
	} else {
		float a[2]  = { x, y };
		int   major = (fabsf(x) > fabsf(y)) ? 0 : 1;
		if (fabsf(a[major]) > fabsf(a[1 - major]) * IMU_ORIENT_DOMINANCE) {
			for (int r = 0; r < 4; r++) {
				if (s_orientMap[r].axis == major && (a[major] > 0) == (s_orientMap[r].sign > 0)) {
					cand = r;
				}
			}
		}						/* else diagonal: keep the candidate */
	}
	if (cand >= 0 && cand < 4) {
		bool portrait = (cand % 2 == 0);
		if ((s_orMode == IMU_OR_P && !portrait) || (s_orMode == IMU_OR_L && portrait)) {
			cand = s_orCand;			/* not watched in this mode */
		}
	}
	if (cand != s_orCand) {
		s_orCand  = cand;
		s_orSince = now;
	}
	if (cand == IMU_ORIENT_NONE || cand == s_orShown || now - s_orSince < nvGetInt("imu.orient.hold")) {
		return;
	}
	s_orShown = cand;
	if (cand == IMU_ORIENT_FLAT) {
		imuEvent("ORIENT FLAT");
		return;
	}

	int rot = cand;						/* logical ROT that shows upright */
	if ((rot % 2 == 0 && nvGetInt("scr.pflip")) || (rot % 2 == 1 && nvGetInt("scr.lflip"))) {
		rot ^= 2;
	}
	char text[16];
	snprintf(text, sizeof(text), "ORIENT %d", rot);
	imuEvent(text);
	if (s_orAuto) {
		uiRunMain(imuAutoFlip, rot);
	}
}

/* Main task (ORIENT AUTO): side rot is up. Same as ROT -> view flip off;
 * ROT turned 180 -> view flip on; 90 degrees -> no change (event only). */
static void imuAutoFlip(int rot)
{
	int cur = lcdGetRotation();
	if (rot == cur) {
		lcdSetViewFlip(false);
	} else if (rot == (cur ^ 2)) {
		lcdSetViewFlip(true);
	}
}

/* ================= Sleep ================= */

/* Wake on Motion: accel low-power 21 Hz, INT1 starts high and toggles on
 * each event. Caller holds s_lock. */
static esp_err_t imuWomEnter(int thr)
{
	uint8_t cal[8] = { 0 };
	imuStepSave();
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL7, 0), TAG, "ctrl7");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL8, IMU_CTRL8_BASE), TAG, "ctrl8");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL2, (imuLog2(s_arange) - 1) << 4 | IMU_LP_21HZ), TAG, "ctrl2");
	cal[0] = thr;						/* mg */
	cal[1] = IMU_WOM_INT1_HIGH | IMU_WOM_BLANK;
	ESP_RETURN_ON_ERROR(imuCtrl9(IMU_CMD_WOM, cal, IMU_CTRL9_MS), TAG, "wom");
	ESP_RETURN_ON_ERROR(imuWrite(IMU_REG_CTRL7, IMU_CTRL7_A), TAG, "ctrl7");
	uint8_t st;
	return imuRead(IMU_REG_STATUS1, &st, 1);		/* clear */
}

/* Leave WoM and restore the normal configuration. Caller holds s_lock. */
static esp_err_t imuWomExit(void)
{
	uint8_t cal[8] = { 0 };					/* threshold 0 = WoM off */
	imuWrite(IMU_REG_CTRL7, 0);
	imuCtrl9(IMU_CMD_WOM, cal, IMU_CTRL9_MS);
	return imuApply();
}

/* IMU task: SCR = backlight off, RAD = also radio off, CPU = deep sleep.
 * Wakes on motion (WoM) or BOOT. */
static void imuSleepRun(imuSleep_t level, int thr)
{
	if (level != IMU_SLEEP_SCR) {
		vTaskDelay(pdMS_TO_TICKS(IMU_SLEEP_DELAY_MS));	/* OK out before the radio goes */
	}
	s_asleep = true;

	xSemaphoreTake(s_lock, portMAX_DELAY);
	esp_err_t err = imuWomEnter(thr);
	if (err != ESP_OK) {
		imuWomExit();
	}
	xSemaphoreGive(s_lock);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "WoM failed");
		s_asleep = false;
		imuEvent("WAKE");
		return;
	}
	if (level == IMU_SLEEP_CPU) {
		imuDeepSleep();					/* does not return */
	}

	uint8_t bl  = lcdGetBacklight();
	uiIf_t  ifc = uiActive();
	lcdBacklight(0);
	ledHide(true);					/* LED commands now only store */
	if (level == IMU_SLEEP_RAD) {
		uiDisable();
	}
	gpio_input_enable(IMU_PIN_BOOT);
	gpio_pullup_en(IMU_PIN_BOOT);
	ESP_LOGI(TAG, "asleep (%s)", level == IMU_SLEEP_RAD ? "RAD" : "SCR");

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
		uint8_t st = 0;
		xSemaphoreTake(s_lock, portMAX_DELAY);
		imuRead(IMU_REG_STATUS1, &st, 1);
		xSemaphoreGive(s_lock);
		if ((st & IMU_ST1_WOM) || gpio_get_level(IMU_PIN_BOOT) == 0) {
			break;
		}
	}

	xSemaphoreTake(s_lock, portMAX_DELAY);
	imuWomExit();
	xSemaphoreGive(s_lock);
	lcdBacklight(bl);
	ledHide(false);					/* latest colour */
	s_moving     = false;
	s_quietSince = 0;
	s_asleep     = false;
	if (level == IMU_SLEEP_RAD && ifc != UI_NONE) {
		uiSelect(ifc);					/* the client has to reconnect */
	}
	ESP_LOGI(TAG, "awake");
	imuEvent("WAKE");
}

/* Deep sleep until INT1 (motion) or BOOT goes low; waking = reboot. */
static void imuDeepSleep(void)
{
	uint8_t st;
	xSemaphoreTake(s_lock, portMAX_DELAY);
	imuRead(IMU_REG_STATUS1, &st, 1);			/* INT1 back to its initial high */
	xSemaphoreGive(s_lock);

	ledOff();
	lcdDeepSleepPrep();
	rtc_gpio_pullup_en(IMU_PIN_BOOT);
	rtc_gpio_pulldown_dis(IMU_PIN_BOOT);
	esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);	/* keep the pull-up */
	esp_sleep_enable_ext1_wakeup_io((1ULL << IMU_PIN_BOOT) | (1ULL << IMU_PIN_INT1), ESP_EXT1_WAKEUP_ANY_LOW);
	ESP_LOGI(TAG, "deep sleep");
	vTaskDelay(pdMS_TO_TICKS(50));				/* let the log out */
	esp_deep_sleep_start();
}

/* ================= Commands ================= */

static const char *imuCmdAcc(int argc, char *argv[], cmdReply_t reply)
{
	float x, y, z;
	if (imuBusy()) {
		return "IMU asleep";
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	esp_err_t err = imuReadAccel(&x, &y, &z);
	xSemaphoreGive(s_lock);
	if (err != ESP_OK) {
		return "IMU read failed";
	}
	char text[64];
	snprintf(text, sizeof(text), "aX:%7.3f\naY:%7.3f\naZ:%7.3f\n", x, y, z);
	reply(text);
	return NULL;
}

static const char *imuCmdGyr(int argc, char *argv[], cmdReply_t reply)
{
	float x, y, z;
	if (imuBusy()) {
		return "IMU asleep";
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	esp_err_t err = imuReadGyro(&x, &y, &z);
	xSemaphoreGive(s_lock);
	if (err != ESP_OK) {
		return "IMU read failed";
	}
	char text[64];
	snprintf(text, sizeof(text), "gX:%8.2f\ngY:%8.2f\ngZ:%8.2f\n", x, y, z);
	reply(text);
	return NULL;
}

static const char *imuCmdTemp(int argc, char *argv[], cmdReply_t reply)
{
	float c;
	if (imuBusy()) {
		return "IMU asleep";
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	esp_err_t err = imuReadTemp(&c);
	xSemaphoreGive(s_lock);
	if (err != ESP_OK) {
		return "IMU read failed";
	}
	char text[24];
	snprintf(text, sizeof(text), "^C:%7.2f\n", c);
	reply(text);
	return NULL;
}

static const char *imuCmd(int argc, char *argv[], cmdReply_t reply)
{
	if (imuBusy()) {
		return "IMU asleep";
	}
	if (argc == 0) {
		imuStatus(reply);
		return NULL;
	}

	const char *sub = argv[0];
	int         n   = argc - 1;
	char      **a   = argv + 1;
	int         v;
	const char *err;
	char        text[24];

	if (strcasecmp(sub, "ACC") == 0 || strcasecmp(sub, "GYR") == 0) {
		return imuCmdRange(toupper((unsigned char)sub[0]) == 'A', n, a, reply);
	}
	if (strcasecmp(sub, "RESET") == 0 && n == 0) {
		xSemaphoreTake(s_lock, portMAX_DELAY);
		memset(s_tapEn, 0, sizeof(s_tapEn));
		s_motion   = s_step = s_orAuto = false;
		s_stillMs  = s_sigmoMs = s_stepAuto = 0;
		s_sigmoRep = false;
		s_orMode   = IMU_OR_OFF;
		s_stepBase = 0;
		s_moving   = false;
		s_orCand   = s_orShown = IMU_ORIENT_NONE;
		esp_err_t e = imuSoftReset();
		imuLoadNv();
		if (e == ESP_OK) {
			e = imuApply();
		}
		xSemaphoreGive(s_lock);
		lcdSetViewFlip(false);
		return (e == ESP_OK) ? NULL : "IMU reset failed";
	}
	if (strcasecmp(sub, "TAP") == 0) {
		return imuCmdTap(n, a, reply);
	}
	if (strcasecmp(sub, "MOTION") == 0 && n <= 1) {
		if (n == 0) {
			reply(s_motion ? "1\n" : "0\n");
			return NULL;
		}
		if (!cmdParseInt(a[0], 0, 1, &v)) {
			return "usage: IMU MOTION 0|1";
		}
		if (v && IMU_TAP_ON()) {
			return "MOTION cannot run with TAP (IMU TAP NONE first)";
		}
		xSemaphoreTake(s_lock, portMAX_DELAY);
		s_motion = v;
		err = (imuEngines() == ESP_OK) ? NULL : "IMU write failed";
		xSemaphoreGive(s_lock);
		return err;
	}
	if (strcasecmp(sub, "STILL") == 0 && n <= 1) {
		if (n == 0) {
			snprintf(text, sizeof(text), "%d\n", s_stillMs);
			reply(text);
			return NULL;
		}
		if (!cmdParseInt(a[0], 0, IMU_STILL_MAX, &v)) {
			return "ms must be 0..60000 (0 = off)";
		}
		if (v && IMU_TAP_ON()) {
			return "STILL cannot run with TAP (IMU TAP NONE first)";
		}
		xSemaphoreTake(s_lock, portMAX_DELAY);
		s_stillMs = v;
		err = (imuEngines() == ESP_OK) ? NULL : "IMU write failed";
		xSemaphoreGive(s_lock);
		return err;
	}
	if (strcasecmp(sub, "SIGMO") == 0 && n <= 2) {
		if (n == 0) {
			snprintf(text, sizeof(text), "%d%s\n", s_sigmoMs, s_sigmoRep ? " REPEAT" : "");
			reply(text);
			return NULL;
		}
		static char msg[48];
		int max = IMU_SIGMO_CHIP ? (int)(65535 * 1000.0f / imuOdr() / 2)	/* confirm = 2 x ms */
		                         : IMU_SIGMO_MAX;
		bool repeat = (n == 2);
		if (repeat && strcasecmp(a[1], "REPEAT") != 0) {
			return "usage: IMU SIGMO [ms [REPEAT]]";
		}
		if (!cmdParseInt(a[0], 0, max, &v) || (v > 0 && v < IMU_SIGMO_MIN)) {
			snprintf(msg, sizeof(msg), "ms must be 0 or %d..%d", IMU_SIGMO_MIN, max);
			return msg;
		}
		if (v && IMU_TAP_ON()) {
			return "SIGMO cannot run with TAP (IMU TAP NONE first)";
		}
		xSemaphoreTake(s_lock, portMAX_DELAY);
		s_sigmoMs  = v;
		s_sigmoRep = v && repeat;
		s_sigSeen  = false;
		esp_err_t e = IMU_SIGMO_CHIP ? imuApply() : imuEngines();	/* chip: new windows */
		err = (e == ESP_OK) ? NULL : "IMU write failed";
		xSemaphoreGive(s_lock);
		return err;
	}
	if (strcasecmp(sub, "STEP") == 0) {
		return IMU_STEP_ENABLE ? imuCmdStep(n, a, reply) : "not implemented";
	}
	if (strcasecmp(sub, "ORIENT") == 0) {
		return imuCmdOrient(n, a, reply);
	}
	if (strcasecmp(sub, "SET") == 0) {
		return imuCmdSet(n, a, reply);
	}
	if (strcasecmp(sub, "WOM") == 0 && n <= 1) {
		if (n == 0) {
			snprintf(text, sizeof(text), "%s\n", nvGet("imu.wom"));
			reply(text);
			return NULL;
		}
		char  key[] = "wom";
		char *sa[2] = { key, a[0] };
		return imuCmdSet(2, sa, reply);
	}
	if (strcasecmp(sub, "SLEEP") == 0) {
		return imuCmdSleep(n, a, reply);
	}
	if (strcasecmp(sub, "CAL") == 0 && n == 0) {
		return imuCmdCal(reply);
	}
	if (strcasecmp(sub, "TEST") == 0 && n == 0) {
		return imuCmdTest(reply);
	}
	if (strcasecmp(sub, "REC") == 0) {
		return imuCmdRec(n, a, reply);
	}
	if (strcasecmp(sub, "DEBUG") == 0 && n <= 1) {
		if (n == 0) {
			reply(s_dbg ? "1\n" : "0\n");
			return NULL;
		}
		if (!cmdParseInt(a[0], 0, 1, &v)) {
			return "usage: IMU DEBUG 0|1";
		}
		s_dbgLast = 0xFFFF;				/* log the current state first */
		s_dbg     = v;
		return NULL;
	}
	return "usage: IMU [ACC|GYR [range rate] | RESET | TAP | MOTION | STILL | SIGMO | STEP | ORIENT | SET | WOM | SLEEP | CAL | TEST | REC | DEBUG]";
}

static void imuStatus(cmdReply_t reply)
{
	static const char *orNames[] = { "NONE", "P", "L", "ROT" };
	char text[64];

	snprintf(text, sizeof(text), "ACC %s %s\n", nvGet("imu.arange"), nvGet("imu.arate"));
	reply(text);
	snprintf(text, sizeof(text), "GYR %s %s\n", nvGet("imu.grange"), nvGet("imu.grate"));
	reply(text);
	if (s_tapEn[0] || s_tapEn[1] || s_tapEn[2]) {
		snprintf(text, sizeof(text), "TAP %d %d %d\n", s_tapEn[0], s_tapEn[1], s_tapEn[2]);
	} else {
		snprintf(text, sizeof(text), "TAP NONE\n");
	}
	reply(text);
	snprintf(text, sizeof(text), "MOTION %d\nSTILL %d\nSIGMO %d%s\n", s_motion, s_stillMs, s_sigmoMs,
	         s_sigmoRep ? " REPEAT" : "");
	reply(text);
	if (IMU_STEP_ENABLE) {
		xSemaphoreTake(s_lock, portMAX_DELAY);
		uint32_t steps = imuSteps();
		xSemaphoreGive(s_lock);
		snprintf(text, sizeof(text), "STEP %d AUTO %d COUNT %lu\n", s_step, s_stepAuto, (unsigned long)steps);
		reply(text);
	}
	snprintf(text, sizeof(text), "ORIENT %s%s\n", orNames[s_orMode], s_orAuto ? " AUTO" : "");
	reply(text);
	snprintf(text, sizeof(text), "WOM %s\n", nvGet("imu.wom"));
	reply(text);
}

/* IMU ACC|GYR [range rate] */
static const char *imuCmdRange(bool acc, int argc, char *argv[], cmdReply_t reply)
{
	const char *kr = acc ? "imu.arange" : "imu.grange";
	const char *kt = acc ? "imu.arate" : "imu.grate";

	if (argc == 0) {
		char text[32];
		snprintf(text, sizeof(text), "%s %s\n", nvGet(kr), nvGet(kt));
		reply(text);
		return NULL;
	}
	if (argc != 2) {
		return acc ? "usage: IMU ACC [2|4|8|16 28|56|112|224|448|896]"
		           : "usage: IMU GYR [16|32|64|128|256|512|1024|2048 28|56|112|224|448|896]";
	}
	const char *k[] = { kr, kt }, *v[] = { argv[0], argv[1] };
	xSemaphoreTake(s_lock, portMAX_DELAY);
	const char *err = nvSetMany(2, k, v, NULL);
	if (!err) {
		imuLoadNv();
		if (IMU_SIGMO_CHIP && s_sigmoMs > (int)(65535 * 1000.0f / imuOdr() / 2)) {
			s_sigmoMs = 0;				/* no longer fits: off */
		}
		err = (imuApply() == ESP_OK) ? NULL : "IMU write failed";
	}
	xSemaphoreGive(s_lock);
	return err;
}

/* IMU TAP [s d t | NONE] */
/* "TAP cannot run with MOTION, SIGMO (turn them off first)": only the
 * detectors that are on. */
static const char *imuTapClash(void)
{
	static char msg[72];
	const char *on[4];
	int         n = 0;

	if (s_motion)  on[n++] = "MOTION";
	if (s_stillMs) on[n++] = "STILL";
	if (s_sigmoMs) on[n++] = "SIGMO";
	if (s_step)    on[n++] = "STEP";
	int len = snprintf(msg, sizeof(msg), "TAP cannot run with");
	for (int i = 0; i < n; i++) {
		len += snprintf(msg + len, sizeof(msg) - len, "%s %s", i ? "," : "", on[i]);
	}
	snprintf(msg + len, sizeof(msg) - len, " (turn %s off first)", n > 1 ? "them" : "it");
	return msg;
}

static const char *imuCmdTap(int argc, char *argv[], cmdReply_t reply)
{
	bool en[3] = { false, false, false };
	int  v;

	if (argc == 0) {
		char text[16];
		if (s_tapEn[0] || s_tapEn[1] || s_tapEn[2]) {
			snprintf(text, sizeof(text), "%d %d %d\n", s_tapEn[0], s_tapEn[1], s_tapEn[2]);
		} else {
			snprintf(text, sizeof(text), "NONE\n");
		}
		reply(text);
		return NULL;
	}
	if (argc == 3) {
		for (int i = 0; i < 3; i++) {
			if (!cmdParseInt(argv[i], 0, 1, &v)) {
				return "usage: IMU TAP s d t (0|1 each) | NONE";
			}
			en[i] = v;
		}
	} else if (argc != 1 || strcasecmp(argv[0], "NONE") != 0) {
		return "usage: IMU TAP s d t (0|1 each) | NONE";
	}
	if ((en[0] || en[1] || en[2]) && IMU_OTHERS_ON()) {
		return imuTapClash();
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	memcpy(s_tapEn, en, sizeof(en));
	s_tapN = 0;
	const char *err = (imuEngines() == ESP_OK) ? NULL : "IMU write failed";
	xSemaphoreGive(s_lock);
	if (!err && imuOdr() < 200.0f && (en[0] || en[1] || en[2])) {
		reply("warning: tap works best with imu.arate 224 or more\n");
	}
	return err;
}

/* IMU STEP [RESET|0|1|AUTO n] */
static const char *imuCmdStep(int argc, char *argv[], cmdReply_t reply)
{
	int         v;
	const char *err = NULL;

	if (argc == 0) {
		xSemaphoreTake(s_lock, portMAX_DELAY);
		uint32_t steps = imuSteps();
		xSemaphoreGive(s_lock);
		char text[16];
		snprintf(text, sizeof(text), "%lu\n", (unsigned long)steps);
		reply(text);
		return NULL;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	if (argc == 1 && strcasecmp(argv[0], "RESET") == 0) {
		uint8_t cal[8] = { 0 };
		s_stepBase = 0;
		s_stepNext = s_stepAuto;
		err = (imuCtrl9(IMU_CMD_RESET_PEDO, cal, IMU_CTRL9_MS) == ESP_OK) ? NULL : "IMU write failed";
	} else if (argc == 1 && cmdParseInt(argv[0], 0, 1, &v) && v && IMU_TAP_ON()) {
		err = "STEP cannot run with TAP (IMU TAP NONE first)";
	} else if (argc == 1 && cmdParseInt(argv[0], 0, 1, &v)) {
		s_step = v;
		err = (imuEngines() == ESP_OK) ? NULL : "IMU write failed";
		if (v && s_stepAuto) {
			s_stepNext = (imuSteps() / s_stepAuto + 1) * s_stepAuto;
		}
	} else if (argc == 2 && strcasecmp(argv[0], "AUTO") == 0 && cmdParseInt(argv[1], 0, IMU_STEP_AUTO_MAX, &v)) {
		s_stepAuto = v;
		s_stepNext = v ? (imuSteps() / v + 1) * v : 0;
	} else {
		err = "usage: IMU STEP [RESET | 0|1 | AUTO n (0..10000, 0 = off)]";
	}
	xSemaphoreGive(s_lock);
	return err;
}

/* IMU ORIENT [NONE|P|L|ROT] [AUTO] */
static const char *imuCmdOrient(int argc, char *argv[], cmdReply_t reply)
{
	static const char *names[] = { "NONE", "P", "L", "ROT" };

	if (argc == 0) {
		char text[24];
		snprintf(text, sizeof(text), "%s%s\n", names[s_orMode], s_orAuto ? " AUTO" : "");
		reply(text);
		return NULL;
	}
	int mode = -1;
	for (int i = 0; i < 4; i++) {
		if (strcasecmp(argv[0], names[i]) == 0) {
			mode = i;
		}
	}
	bool autoRot = (argc == 2 && strcasecmp(argv[1], "AUTO") == 0);
	if (mode < 0 || argc > 2 || (argc == 2 && !autoRot) || (mode == IMU_OR_OFF && autoRot)) {
		return "usage: IMU ORIENT NONE | P|L|ROT [AUTO]";
	}
	imuOrientSet((imuOrMode_t)mode, autoRot);
	return NULL;
}

int imuOrientLast(void)
{
	return s_orShown;
}

void imuOrientSet(imuOrMode_t mode, bool autoFlip)
{
	if (mode == IMU_OR_OFF) {
		autoFlip = false;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	s_orMode  = mode;
	s_orAuto  = autoFlip;
	s_orCand  = IMU_ORIENT_NONE;			/* report the current side again */
	s_orShown = IMU_ORIENT_NONE;
	xSemaphoreGive(s_lock);
	if (!autoFlip) {
		lcdSetViewFlip(false);
	}
}

/* IMU SET [key value]: the imu.* tuning keys (stored in NV, applied now). */
static const char *imuCmdSet(int argc, char *argv[], cmdReply_t reply)
{
	static const char *keys[] = {
		"wom", "tap.thr", "tap.gap", "mot.thr", "still.thr",
		"step.thr", "step.min", "orient.hold", "orient.flat",
	};
	static const bool engine[] = {			/* needs the chip reconfigured */
		false, true, false, true, true,
		true, true, false, false,
	};
	const int n = sizeof(keys) / sizeof(keys[0]);
	char      full[24], text[48];

	if (argc == 0) {
		for (int i = 0; i < n; i++) {
			snprintf(full, sizeof(full), "imu.%s", keys[i]);
			snprintf(text, sizeof(text), "%s=%s\n", keys[i], nvGet(full));
			reply(text);
		}
		return NULL;
	}
	const char *key = argv[0];
	if (strncasecmp(key, "imu.", 4) == 0) {
		key += 4;
	}
	int i;
	for (i = 0; i < n && strcasecmp(key, keys[i]) != 0; i++) {
	}
	if (argc != 2 || i == n) {
		return "usage: IMU SET [key value]  keys: wom tap.thr tap.gap mot.thr still.thr step.thr step.min orient.hold orient.flat";
	}
	snprintf(full, sizeof(full), "imu.%s", keys[i]);
	const char *k[] = { full }, *v[] = { argv[1] };
	xSemaphoreTake(s_lock, portMAX_DELAY);
	const char *err = nvSetMany(1, k, v, NULL);
	if (!err && engine[i]) {
		err = (imuApply() == ESP_OK) ? NULL : "IMU write failed";
	}
	xSemaphoreGive(s_lock);
	return err;
}

/* IMU SLEEP [SCR|RAD|CPU] [thr] */
static const char *imuCmdSleep(int argc, char *argv[], cmdReply_t reply)
{
	imuSleep_t level = IMU_SLEEP_SCR;
	int        thr   = nvGetInt("imu.wom");
	int        i     = 0;

	if (argc > 0) {
		if (strcasecmp(argv[0], "SCR") == 0) {
			i = 1;
		} else if (strcasecmp(argv[0], "RAD") == 0) {
			level = IMU_SLEEP_RAD;
			i     = 1;
		} else if (strcasecmp(argv[0], "CPU") == 0) {
			level = IMU_SLEEP_CPU;
			i     = 1;
		}
	}
	if (argc - i > 1 || (argc - i == 1 && !cmdParseInt(argv[i], 1, 255, &thr))) {
		return "usage: IMU SLEEP [SCR|RAD|CPU] [thr 1..255 mg]";
	}
	s_sleepThr = thr;
	s_sleepReq = level;					/* the IMU task takes it from here */
	return NULL;
}

/* IMU CAL: gyro calibration-on-demand (keep still ~1.5 s). */
static const char *imuCmdCal(cmdReply_t reply)
{
	uint8_t st = 0xFF;

	xSemaphoreTake(s_lock, portMAX_DELAY);
	imuWrite(IMU_REG_CTRL7, 0);
	esp_err_t err = imuCtrl9(IMU_CMD_COD, NULL, IMU_COD_MS);
	if (err == ESP_OK) {
		imuRead(IMU_REG_COD_STATUS, &st, 1);
	}
	imuApply();
	xSemaphoreGive(s_lock);

	if (err != ESP_OK) {
		return "calibration timed out";
	}
	char text[40];
	snprintf(text, sizeof(text), "COD 0x%02X %s\n", st, st == 0 ? "PASS" : "FAIL (keep still)");
	reply(text);
	return NULL;
}

/* Self-test of one sensor: set the ST bit in reg, wait for STATUSINT.0,
 * clear it, read the result (lsb per unit). Caller holds s_lock. */
static esp_err_t imuSelfTest(uint8_t reg, float lsb, float out[3])
{
	uint8_t v = 0, st = 0, raw[6];
	imuWrite(IMU_REG_CTRL7, 0);
	imuRead(reg, &v, 1);
	ESP_RETURN_ON_ERROR(imuWrite(reg, v | IMU_CTRL_ST), TAG, "st on");

	int64_t   end = esp_timer_get_time() + (int64_t)IMU_ST_MS * 1000;
	esp_err_t err = ESP_ERR_TIMEOUT;
	while (esp_timer_get_time() < end) {
		vTaskDelay(1);
		if (imuRead(IMU_REG_STATUSINT, &st, 1) == ESP_OK && (st & 0x01)) {
			err = ESP_OK;
			break;
		}
	}
	imuRead(reg, &v, 1);
	imuWrite(reg, v & ~IMU_CTRL_ST);
	for (int i = 0; i < 20; i++) {				/* STATUSINT.0 back to 0 */
		vTaskDelay(1);
		if (imuRead(IMU_REG_STATUSINT, &st, 1) == ESP_OK && !(st & 0x01)) {
			break;
		}
	}
	if (err == ESP_OK) {
		err = imuRead(IMU_REG_DVX_L, raw, sizeof(raw));
		for (int i = 0; i < 3; i++) {
			out[i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) / lsb;
		}
	}
	return err;
}

/* IMU TEST: accelerometer (> 200 mg) and gyroscope (> 300 dps) self-test. */
static const char *imuCmdTest(cmdReply_t reply)
{
	float a[3] = { 0 }, g[3] = { 0 };

	xSemaphoreTake(s_lock, portMAX_DELAY);
	esp_err_t ea = imuSelfTest(IMU_REG_CTRL2, 2.048f, a);	/* 1/2048 g -> mg */
	esp_err_t eg = imuSelfTest(IMU_REG_CTRL3, 16.0f, g);	/* 1/16 dps */
	imuApply();
	xSemaphoreGive(s_lock);

	char text[72];
	if (ea == ESP_OK) {
		bool ok = fabsf(a[0]) > 200 && fabsf(a[1]) > 200 && fabsf(a[2]) > 200;
		snprintf(text, sizeof(text), "ACC %.0f %.0f %.0f mg %s\n", a[0], a[1], a[2], ok ? "PASS" : "FAIL");
	} else {
		snprintf(text, sizeof(text), "ACC FAIL (no result)\n");
	}
	reply(text);
	if (eg == ESP_OK) {
		bool ok = fabsf(g[0]) > 300 && fabsf(g[1]) > 300 && fabsf(g[2]) > 300;
		snprintf(text, sizeof(text), "GYR %.0f %.0f %.0f dps %s\n", g[0], g[1], g[2], ok ? "PASS" : "FAIL");
	} else {
		snprintf(text, sizeof(text), "GYR FAIL (no result)\n");
	}
	reply(text);
	return NULL;
}

/* Append to the recording file (card locked per write, not for the whole run). */
static bool imuRecWrite(const char *path, const char *buf, size_t len)
{
	sdLock();
	FILE *f  = fopen(path, "a");
	bool  ok = f && fwrite(buf, 1, len, f) == len;
	if (f && fclose(f) != 0) {
		ok = false;
	}
	sdUnlock();
	return ok;
}

/* Create SD:/imu/<name>.csv (* = timestamp) with the header line; an
 * existing file is an error unless force. */
static const char *imuRecOpen(const char *name, bool force, char *file, size_t fileSize, char *path, size_t pathSize)
{
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	if (mkdir(IMU_REC_DIR, 0777) != 0 && errno != EEXIST) {
		sdUnlock();
		return "cannot create /imu";
	}
	if (strcmp(name, "*") == 0) {
		const char *e = sysTimestampFile(IMU_REC_DIR, IMU_REC_EXT, file, fileSize);
		if (e) {
			sdUnlock();
			return e;
		}
	} else {
		size_t len    = strlen(name);
		bool   hasExt = len >= 4 && strcasecmp(name + len - 4, IMU_REC_EXT) == 0;
		if (strpbrk(name, "/\\:*?") || len + (hasExt ? 0 : 4) >= fileSize) {
			sdUnlock();
			return "bad file name";
		}
		snprintf(file, fileSize, "%s%s", name, hasExt ? "" : IMU_REC_EXT);
	}
	snprintf(path, pathSize, "%s/%s", IMU_REC_DIR, file);
	struct stat st;
	if (!force && stat(path, &st) == 0) {
		sdUnlock();
		return "file exists (-f to overwrite)";
	}
	FILE *f  = fopen(path, "w");
	bool  ok = f && fputs(IMU_REC_HEADER, f) >= 0;
	if (f && fclose(f) != 0) {
		ok = false;
	}
	sdUnlock();
	return ok ? NULL : "cannot write file";
}

/* IMU REC ms [name|* [-f]]: accel + gyro at the ODR through the FIFO.
 * Without a name the lines are replied; with one they go to
 * SD:/imu/name.csv (-f overwrites; -f may come before or after the name). */
static const char *imuCmdRec(int argc, char *argv[], cmdReply_t reply)
{
	static char msg[48];
	int         ms;
	const char *name  = NULL;
	bool        force = false;
	char        file[SD_NAME_MAX] = "";
	char        path[sizeof(IMU_REC_DIR) + SD_NAME_MAX + 1] = "";

	if (argc < 1) {
		return "usage: IMU REC ms [name|* [-f]]";
	}
	for (int i = 1; i < argc; i++) {
		if (strcasecmp(argv[i], "-f") == 0) {
			force = true;
		} else if (!name) {
			name = argv[i];
		} else {
			return "usage: IMU REC ms [name|* [-f]]";
		}
	}
	if (force && !name) {
		return "usage: IMU REC ms [name|* [-f]]";
	}
	bool toFile = (name != NULL);
	uiIf_t ui   = uiActive();
	int  max    = toFile ? IMU_REC_FILE_MS : (ui == UI_BLE || ui == UI_UART) ? IMU_REC_BLE_MS : IMU_REC_UI_MS;
	if (!cmdParseInt(argv[0], 1, max, &ms)) {
		snprintf(msg, sizeof(msg), "ms must be 1..%d%s", max,
		         (!toFile && max == IMU_REC_BLE_MS) ? (ui == UI_UART ? " on UART" : " on BLE") : "");
		return msg;
	}
	if (s_arateCode != s_grateCode) {
		return "REC needs imu.arate = imu.grate";
	}
	if (toFile) {
		const char *e = imuRecOpen(name, force, file, sizeof(file), path, sizeof(path));
		if (e) {
			return e;
		}
	}

	size_t   bufMax = IMU_FIFO_SAMPLES * IMU_REC_LINE;
	char    *buf    = heap_caps_malloc(bufMax, MALLOC_CAP_SPIRAM);
	uint8_t *raw    = heap_caps_malloc(IMU_FIFO_SAMPLES * 12, MALLOC_CAP_INTERNAL);
	if (!buf || !raw) {
		free(buf);
		free(raw);
		return "out of memory";
	}
	if (!toFile) {
		reply(IMU_REC_HEADER);
	}

	float    odr   = imuOdr();
	float    as    = s_arange / 32768.0f, gs = s_grange / 32768.0f;
	uint32_t total = (uint32_t)ceilf(odr * ms / 1000.0f);	/* samples wanted */
	uint32_t idx   = 0;
	bool     ovf   = false, wrOk = true;

	xSemaphoreTake(s_lock, portMAX_DELAY);
	imuWrite(IMU_REG_FIFO_CTRL, 0);
	imuCtrl9(IMU_CMD_RST_FIFO, NULL, IMU_CTRL9_MS);
	imuWrite(IMU_REG_FIFO_CTRL, IMU_FIFO_STREAM_128);
	xSemaphoreGive(s_lock);

	bool aborted = false;
	while (idx < total && wrOk) {
		if (scriptAbortRequested()) {
			aborted = true;				/* STOP NOW */
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));

		uint8_t cnt[2] = { 0 };
		int     n      = 0;
		xSemaphoreTake(s_lock, portMAX_DELAY);
		if (imuRead(IMU_REG_FIFO_CNT, cnt, 2) == ESP_OK) {
			ovf |= (cnt[1] & IMU_FIFO_OVFLOW) != 0;
			n = (2 * (((cnt[1] & 0x03) << 8) | cnt[0])) / 12;	/* words -> samples */
			if (n > IMU_FIFO_SAMPLES) {
				n = IMU_FIFO_SAMPLES;
			}
		}
		if (n > 0 && imuCtrl9(IMU_CMD_REQ_FIFO, NULL, IMU_CTRL9_MS) == ESP_OK) {
			imuWrite(IMU_REG_CTRL1, IMU_CTRL1_INT1_EN);	/* no auto-increment: stay on FIFO_DATA */
			if (imuRead(IMU_REG_FIFO_DATA, raw, n * 12) != ESP_OK) {
				n = 0;
			}
			imuWrite(IMU_REG_CTRL1, IMU_CTRL1_ADDR_AI | IMU_CTRL1_INT1_EN);
		} else {
			n = 0;
		}
		imuWrite(IMU_REG_FIFO_CTRL, IMU_FIFO_STREAM_128);	/* leave read mode */
		xSemaphoreGive(s_lock);

		size_t len = 0;
		for (int i = 0; i < n && idx < total; i++, idx++) {
			const uint8_t *p = raw + i * 12;
			int16_t        d[6];
			for (int k = 0; k < 6; k++) {
				d[k] = (int16_t)(p[2 * k] | (p[2 * k + 1] << 8));
			}
			len += snprintf(buf + len, bufMax - len, "%.1f %.4f %.4f %.4f %.2f %.2f %.2f\n",
			                idx * 1000.0f / odr, d[0] * as, d[1] * as, d[2] * as,
			                d[3] * gs, d[4] * gs, d[5] * gs);
		}
		if (len > 0) {
			if (toFile) {
				wrOk = imuRecWrite(path, buf, len);
			} else {
				reply(buf);
			}
		}
	}

	xSemaphoreTake(s_lock, portMAX_DELAY);
	imuWrite(IMU_REG_FIFO_CTRL, 0);
	imuCtrl9(IMU_CMD_RST_FIFO, NULL, IMU_CTRL9_MS);
	xSemaphoreGive(s_lock);
	free(buf);
	free(raw);

	if (aborted) {
		return "aborted";
	}
	if (!wrOk) {
		return "cannot write file";
	}
	if (toFile) {
		char text[SD_NAME_MAX + 24];
		snprintf(text, sizeof(text), "%s %lu samples\n", file, (unsigned long)idx);
		reply(text);
	}
	return ovf ? "FIFO overflow - samples lost" : NULL;
}
