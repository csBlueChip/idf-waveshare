#include "i2c.h"

#include "esp_check.h"

static const char *TAG = "i2c";

static i2c_master_bus_handle_t s_bus;

/* ================= Public API ================= */

esp_err_t i2cInit(void)
{
	i2c_master_bus_config_t cfg = {
		.i2c_port          = I2C_NUM_0,
		.sda_io_num        = I2C_PIN_SDA,
		.scl_io_num        = I2C_PIN_SCL,
		.clk_source        = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	return i2c_new_master_bus(&cfg, &s_bus);
}

esp_err_t i2cAddDevice(uint8_t addr, i2c_master_dev_handle_t *dev)
{
	ESP_RETURN_ON_FALSE(s_bus, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");

	i2c_device_config_t cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address  = addr,
		.scl_speed_hz    = I2C_FREQ_HZ,
	};
	return i2c_master_bus_add_device(s_bus, &cfg, dev);
}

esp_err_t i2cReadReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
	return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2cWriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
	uint8_t data[2] = { reg, val };
	return i2c_master_transmit(dev, data, sizeof(data), I2C_TIMEOUT_MS);
}
