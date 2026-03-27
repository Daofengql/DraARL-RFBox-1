#ifndef ES8388_DRIVER_H
#define ES8388_DRIVER_H

#include <Arduino.h>

// ES8388 I2C 地址
#define ES8388_ADDR     0x10

// 音频格式
enum class ES8388Format {
    STANDARD_I2S = 0,
    LEFT_JUSTIFIED,
    RIGHT_JUSTIFIED,
    DSP_MODE_A,
    DSP_MODE_B
};

// 采���率
enum class ES8388SampleRate {
    RATE_8K = 0,
    RATE_11_025K,
    RATE_16K,
    RATE_22_05K,
    RATE_24K,
    RATE_32K,
    RATE_44_1K,
    RATE_48K
};

// 输入通道
enum class ES8388Input {
    INPUT1 = 0,  // LIN1/RIN1
    INPUT2,      // LIN2/RIN2
    INPUT_DIFF   // 差分输入
};

// 输出通道
enum class ES8388Output {
    OUT1 = 0,    // LOUT1/ROUT1
    OUT2,        // LOUT2/ROUT2
    OUT_BOTH     // 两个输出
};

/**
 * @brief 初始化 ES8388 编解码器
 * @return true 成功, false 失败
 */
bool es8388_init(void);

/**
 * @brief 卸载 ES8388
 */
void es8388_deinit(void);

/**
 * @brief 检测 ES8388 是否存在
 * @return true 存在, false 不存在
 */
bool es8388_is_present(void);

// ==================== 音频格式配置 ====================

void es8388_set_format(ES8388Format format);
void es8388_set_sample_rate(ES8388SampleRate rate);
void es8388_set_bits_per_sample(uint8_t bits);

// ==================== ADC 配置 (录音) ====================

void es8388_set_adc_input(ES8388Input input);
void es8388_set_adc_volume(uint8_t volume);
void es8388_set_adc_gain(uint8_t gain_db);
void es8388_enable_adc(bool enable);
void es8388_set_adc_mute(bool mute);

// ==================== DAC 配置 (播放) ====================

void es8388_set_dac_output(ES8388Output output);
void es8388_set_dac_volume(uint8_t volume);
void es8388_set_dac_mute(bool mute);
void es8388_enable_dac(bool enable);

// ==================== 混音器配置 ====================

void es8388_set_dac_volume_left(uint8_t volume);
void es8388_set_dac_volume_right(uint8_t volume);
void es8388_set_stereo_balance(int8_t balance);

// ==================== 音效配置 ====================

void es8388_enable_3d(bool enable);
void es8388_enable_mono_mix(bool enable);

// ==================== 电源管理 ====================

void es8388_enter_low_power(void);
void es8388_exit_low_power(void);
void es8388_reset(void);

// ==================== 状态查询 ====================

uint8_t es8388_get_dac_volume(void);
uint8_t es8388_get_adc_volume(void);

#endif // ES8388_DRIVER_H
