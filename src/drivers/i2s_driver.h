#ifndef I2S_DRIVER_H
#define I2S_DRIVER_H

#include <Arduino.h>
#include <driver/i2s.h>

// I2S 配置参数
#define I2S_PORT_NUM        I2S_NUM_0
#define I2S_SAMPLE_RATE     44100
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define I2S_DMA_BUF_COUNT   8
#define I2S_DMA_BUF_LEN     256

/**
 * @brief 初始化 I2S 驱动
 * @return true 成功, false 失败
 */
bool i2s_driver_init(void);

/**
 * @brief 卸载 I2S 驱动
 */
void i2s_driver_deinit(void);

/**
 * @brief 写入音频数据
 * @param data 数据指针
 * @param len 数据长度
 * @param bytes_written 实际写入字节数
 * @return true 成功
 */
bool i2s_write(const void *data, size_t len, size_t *bytes_written);

/**
 * @brief 读取音频数据
 * @param data 数据缓冲区
 * @param len 读取长度
 * @param bytes_read 实际读取字节数
 * @return true 成功
 */
bool i2s_read(void *data, size_t len, size_t *bytes_read);

/**
 * @brief 设置采样率
 * @param rate 采样率
 */
void i2s_set_sample_rate(uint32_t rate);

/**
 * @brief 设置音量 (软件音量控制)
 * @param volume 音量 0-100
 */
void i2s_set_volume(uint8_t volume);

/**
 * @brief 获取当前音量
 * @return 音量 0-100
 */
uint8_t i2s_get_volume(void);

/**
 * @brief 静音
 * @param mute true 静音
 */
void i2s_set_mute(bool mute);

#endif // I2S_DRIVER_H
