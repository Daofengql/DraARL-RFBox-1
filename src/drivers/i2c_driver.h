#ifndef I2C_DRIVER_H
#define I2C_DRIVER_H

#include <Wire.h>
#include <Arduino.h>

// I2C 配置参数
#define I2C_BUS_FREQ_HZ     400000  // 400kHz

/**
 * @brief 初始化 I2C 驱动
 * @return true 成功, false 失败
 */
bool i2c_driver_init(void);

/**
 * @brief 卸载 I2C 驱动
 */
void i2c_driver_deinit(void);

/**
 * @brief I2C 写寄存器
 * @param dev_addr 设备地址
 * @param reg_addr 寄存器地址
 * @param data 数据
 * @param len 数据长度
 * @return true 成功
 */
bool i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *data, size_t len);

/**
 * @brief I2C 读寄存器
 * @param dev_addr 设备地址
 * @param reg_addr 寄存器地址
 * @param data 数据缓冲区
 * @param len 读取长度
 * @return true 成功
 */
bool i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len);

/**
 * @brief I2C 写单字节寄存器
 * @param dev_addr 设备地址
 * @param reg_addr 寄存器地址
 * @param value 值
 * @return true 成功
 */
bool i2c_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);

/**
 * @brief I2C 读单字节寄存器
 * @param dev_addr 设备地址
 * @param reg_addr 寄存器地址
 * @param value 输出值
 * @return true 成功
 */
bool i2c_read_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t *value);

/**
 * @brief 检测设备是否存在
 * @param dev_addr 设备地址
 * @return true 存在
 */
bool i2c_device_exists(uint8_t dev_addr);

/**
 * @brief 扫描 I2C 总线上的所有设备并输出日志
 * @return 检测到的设备数量
 */
size_t i2c_scan_bus(void);

#endif // I2C_DRIVER_H
