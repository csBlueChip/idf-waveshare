#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * QMI8658A 6-axis IMU (I2C 0x6B; INT1 = GPIO13, INT2 = GPIO12).
 * Range and rate from NV (imu.arange/arate/grange/grate); engines tuned by
 * the imu.* keys (IMU SET). Detectors are off at every boot.
 *
 * Commands registered by imuInit():
 *   ACC | GYR | TEMP          read accel (g), gyro (dps), die temperature (^C)
 *   IMU                       status
 *   IMU ACC|GYR [range rate]  show / store + apply range and rate
 *   IMU RESET                 chip reset, NV config, all detectors off
 *   IMU TAP [s d t | NONE]    <<IMU TAP>> <<IMU 2TAP>> <<IMU 3TAP>>
 *   IMU MOTION [0|1]          <<IMU MOTION>> when movement starts
 *   IMU STILL [ms]            <<IMU STILL>> quiet for ms after a motion (0 off)
 *   IMU SIGMO [ms [REPEAT]]   <<IMU SIGMO>> still moving ms after the motion session
 *                             began; once per session, or REPEAT: every ms while moving
 *                             (0 off, 100..60000; firmware-timed)
 *   IMU STEP [RESET|0|1|AUTO n]  pedometer; AUTO n: <<IMU STEP count>> every n steps.
 *                             Only with IMU_STEP_ENABLE 1 (imu.c); now 0 = replies
 *                             "not implemented" (the chip's pedometer never counted)
 *   TAP is exclusive with MOTION / STILL / SIGMO / STEP (the chip's tap engine
 *   reports nothing while another engine runs): enabling one while the other is
 *   on replies an error
 *   IMU ORIENT [NONE|P|L|ROT] [AUTO]  <<IMU ORIENT 0..3|FLAT>>; AUTO turns the
 *                             picture 180 degrees (view flip) when ROT is upside down
 *   IMU SET [key value]       imu.* tuning keys (stored, applied now)
 *   IMU WOM [thr]             wake-on-motion threshold, mg (NV imu.wom)
 *   IMU SLEEP [SCR|RAD|CPU] [thr]  sleep until motion or BOOT (<<IMU WAKE>>)
 *   IMU CAL | IMU TEST        gyro calibration / self-test
 *   IMU DEBUG [0|1]           dbgf lines "IMU <ms> ...": raw STATUS1 / TAP_STATUS on
 *                             every change, CTRL8 on every reconfiguration, a copy of
 *                             every <<IMU ...>> event (this boot only)
 *   IMU REC ms [name|* [-f]]  record t ax ay az gx gy gz (reply, or SD:/imu/name.csv;
 *                             -f overwrites)
 */

/* Reset and configure the chip, start the event task, register commands.
 * Call after i2cInit(), cmdInit() and nvInit(). */
esp_err_t imuInit(void);

/* Orientation detector (as IMU ORIENT mode [AUTO]). OFF also ends the view
 * flip. Call from the main task (it may re-send the screen). The Wait screen
 * uses L + AUTO; a client connect turns it OFF. */
typedef enum { IMU_OR_OFF = 0, IMU_OR_P, IMU_OR_L, IMU_OR_ROT } imuOrMode_t;
void imuOrientSet(imuOrMode_t mode, bool autoFlip);

/* Last reported orientation: 0..3, 4 = FLAT, -1 = none yet. */
int imuOrientLast(void);

/* Raw reads without the IMU lock (the IMU commands take it themselves). */
esp_err_t imuReadAccel(float *x, float *y, float *z);	/* g */
esp_err_t imuReadGyro(float *x, float *y, float *z);	/* degrees per second */
esp_err_t imuReadTemp(float *c);			/* degrees C */
