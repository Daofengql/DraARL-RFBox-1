#include "i2c_driver.h"
#include "../config.h"

static bool i2c_initialized = false;

bool i2c_driver_init(void) {
    Wire.begin(I2C_SDA, I2C_SCL, I2C_BUS_FREQ_HZ);
    i2c_initialized = true;
    return true;
}

void i2c_driver_deinit(void) {
    Wire.end();
    i2c_initialized = false;
}

bool i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *data, size_t len) {
    if (!i2c_initialized) return false;

    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

bool i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len) {
    if (!i2c_initialized) return false;

    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    Wire.requestFrom(dev_addr, len);
    for (size_t i = 0; i < len && Wire.available(); i++) {
        data[i] = Wire.read();
    }
    return true;
}

bool i2c_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t value) {
    return i2c_write_reg(dev_addr, reg_addr, &value, 1);
}

bool i2c_read_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t *value) {
    return i2c_read_reg(dev_addr, reg_addr, value, 1);
}

bool i2c_device_exists(uint8_t dev_addr) {
    if (!i2c_initialized) return false;

    Wire.beginTransmission(dev_addr);
    return Wire.endTransmission() == 0;
}
