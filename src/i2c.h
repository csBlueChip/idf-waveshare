#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Shared I2C master bus (onboard IMU etc.) */
#define I2C_PIN_SDA		48
#define I2C_PIN_SCL		47
#define I2C_FREQ_HZ		400000
#define I2C_TIMEOUT_MS		100

esp_err_t i2cInit(void);

/* Attach a 7-bit device to the bus. */
esp_err_t i2cAddDevice(uint8_t addr, i2c_master_dev_handle_t *dev);

/* Read len bytes starting at reg. */
esp_err_t i2cReadReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);

/* Write one byte to reg. */
esp_err_t i2cWriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
